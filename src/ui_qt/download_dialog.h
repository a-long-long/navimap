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

    /* 下载 + 转换都成功后回调；name 为省/地区名（不含扩展名），便于提示。
     * 用回调代替 Qt 信号槽：本项目 Qt 前端刻意不用 Q_OBJECT，免去 moc 步骤。 */
    void setOnFinished(std::function<void(const QString &name)> cb)
    { m_onFinished = std::move(cb); }

private:
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

    QTreeWidget   *m_tree  = nullptr;
    QLabel        *m_info  = nullptr;
    QLineEdit     *m_url   = nullptr;
    QProgressBar  *m_prog  = nullptr;
    QTextEdit     *m_log   = nullptr;
    QPushButton   *m_dl    = nullptr;
    QPushButton   *m_copy  = nullptr;
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
