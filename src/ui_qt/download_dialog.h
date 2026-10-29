#pragma once
#include <QDialog>
#include <QVector>
#include <QProcess>   /* onConvertFinished 的 ExitStatus 参数需要完整类型 */
#include <functional> /* 用回调代替信号槽：本项目 Qt 前端刻意不用 Q_OBJECT，免去 moc */

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTextEdit;
class QCheckBox;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QFile;

/* 一个可下载区域（Geofabrik）。叶子（kids 空）才有真实下载路径；
 * 枝只用于树形归类。path 是 geofabrik 相对路径（不含 -latest.osm.pbf 后缀）。 */
struct GeoRegion {
    QString name;                 /* 显示名 */
    QString path;                 /* geofabrik 相对路径；枝为空串 */
    QVector<GeoRegion> kids;
};

class DownloadDialog : public QDialog {
public:
    /* sourcesDir：项目的 sources/ 目录（PBF 落盘处）；
     * repoRoot：项目根（含 scripts/sync_maps.sh），用于触发转换。
     * 二者用于"下载 → 转换 → 重新加载"这条链，无需重启程序。 */
    DownloadDialog(const QString &sourcesDir, const QString &repoRoot,
                   QWidget *parent = nullptr);

    /* 预选一个地区（按 Geofabrik 叶子名，如 "hebei"）：展开祖先、选中叶子并滚动到
     * 可见处。供"浏览到无图区 → 反查省 → 一键下载该地区"使用。
     * 返回是否找到；找不到时对话框保持原样（调用方据此决定提示语）。 */
    bool selectByLeaf(const QString &leaf);

    /* 当前选中项的叶子名（如 "chongqing"）；无选中返回空串。
     * 自检用：只验"selectByLeaf 返回 true"不够——同名叶子可能出现在别的国家，
     * 真正要钉的是"选中的那一行就是期望的那个"。 */
    QString currentLeafForTest() const;

    /* 自检用：按叶子名查该区域是否已下载。复用与树标注（✓/·）同源的
     * regionDownloaded 判定，避免在测试里解析树控件文本。 */
    bool isLeafDownloaded(const QString &leaf) const;

    /* 下载 + 转换都成功后回调；name 为省/地区名（不含扩展名），便于提示。
     * 用回调代替 Qt 信号槽：本项目 Qt 前端刻意不用 Q_OBJECT，免去 moc 步骤。 */
    void setOnFinished(std::function<void(const QString &name)> cb)
    { m_onFinished = std::move(cb); }

private:
    /* 判定某 geofabrik 路径（叶子）是否已下载：检查 products/graph/<名> 目录
     * 或 sources/<名>-latest.osm.pbf 是否存在。枝（path 空）恒返回 false。 */
    bool regionDownloaded(const QString &path) const;
    /* 刷新整棵树的状态标注（已下载/未下载 + 枝节点子区域计数）。 */
    void refreshStatus();
    /* 递归刷新单个节点；返回 (叶子总数, 已下载叶子数)，供父节点汇总。 */
    QPair<int,int> refreshItem(QTreeWidgetItem *it);

    void onSelChanged();
    void startDownload();
    void beginDownload();                       /* 真正开始下载（跳过查存/HEAD 之后） */
    void onHeadFinished(QNetworkReply *r);      /* 比对远端新鲜度后决定是否询问更新 */
    void onReplyFinished(QNetworkReply *r);
    void onDownloadProgress(qint64 got, qint64 total);
    void onReadyRead();
    void copyUrl();
    void onConvertFinished(int code, QProcess::ExitStatus st);
    void onConvertOutput();

private:
    void           fillTree(QTreeWidgetItem *parent, const QVector<GeoRegion> &rs);
    QVector<GeoRegion> buildRegions() const;
    QString        currentPath() const;   /* 选中叶子的 geofabrik 路径（枝为空） */
    QString        currentUrl()  const;   /* 完整下载 URL */
    QString        currentName() const;   /* 文件名（不含扩展名），如 gansu */
    void           runContoursGeneration(); /* 生成等高线（若 m_contourCb 勾选） */

    QTreeWidget   *m_tree  = nullptr;
    QLabel        *m_info  = nullptr;
    QLineEdit     *m_url   = nullptr;
    QProgressBar  *m_prog  = nullptr;
    QTextEdit     *m_log   = nullptr;
    QPushButton   *m_dl    = nullptr;
    QPushButton   *m_copy  = nullptr;
    QCheckBox          *m_contourCb = nullptr;  /* "同时生成 SRTM 等高线" 勾选框 */
    QPushButton   *m_close = nullptr;

    QNetworkAccessManager *m_nam   = nullptr;
    QNetworkReply         *m_reply = nullptr;
    QFile                 *m_file  = nullptr;
    QProcess              *m_proc  = nullptr;

    QString m_sources;     /* sources/ 目录 */
    QString m_repo;        /* 项目根 */
    QString m_target;      /* 下载目标文件（sources/<name>-latest.osm.pbf） */
    QString m_dlName;      /* 当前下载的省/地区名（回调用） */
    std::function<void(const QString &name)> m_onFinished;  /* 下载+转换完成回调 */
};
