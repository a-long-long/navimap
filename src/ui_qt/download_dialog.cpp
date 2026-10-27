#include "download_dialog.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTextEdit>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QMessageBox>
#include <QProcess>
#include <QUrl>
#include <QApplication>
#include <QClipboard>

/* 树节点存的角色：UserRole = geofabrik path（枝为空串） */
static const int ROLE_PATH = Qt::UserRole;

namespace {
    static GeoRegion leaf(const char *nm, const char *p)
    {
        GeoRegion g;
        g.name = QString::fromUtf8(nm);
        g.path = QString::fromUtf8(p);
        return g;
    }
    static GeoRegion branch(const char *nm, const QVector<GeoRegion> &k)
    {
        GeoRegion g;
        g.name = QString::fromUtf8(nm);
        g.kids = k;
        return g;
    }
}

DownloadDialog::DownloadDialog(const QString &sourcesDir, const QString &repoRoot,
                               QWidget *parent)
    : QDialog(parent), m_sources(sourcesDir), m_repo(repoRoot)
{
    setWindowTitle(QString::fromUtf8("下载地图（全球可下载区域 · Geofabrik）"));
    resize(840, 580);

    m_tree = new QTreeWidget;
    m_tree->setHeaderLabel(QString::fromUtf8("可下载区域"));
    fillTree(m_tree->invisibleRootItem(), buildRegions());
    m_tree->expandToDepth(0);

    m_info = new QLabel(QString::fromUtf8("选择左侧一个地区，查看下载链接并下载。"));
    m_url  = new QLineEdit; m_url->setReadOnly(true);
    m_prog = new QProgressBar; m_prog->setRange(0, 100); m_prog->setValue(0);
    m_log  = new QTextEdit; m_log->setReadOnly(true);
    m_dl   = new QPushButton(QString::fromUtf8("下载并转换")); m_dl->setEnabled(false);
    m_copy = new QPushButton(QString::fromUtf8("复制下载链接")); m_copy->setEnabled(false);
    m_close= new QPushButton(QString::fromUtf8("关闭"));
    m_contourCb = new QCheckBox(QString::fromUtf8("同时生成 SRTM 等高线"));

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &DownloadDialog::onSelChanged);
    connect(m_dl,    &QPushButton::clicked, this, &DownloadDialog::startDownload);
    connect(m_copy,  &QPushButton::clicked, this, &DownloadDialog::copyUrl);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);

    m_nam = new QNetworkAccessManager(this);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(m_info);
    rv->addWidget(m_url);
    rv->addWidget(m_prog);
    rv->addWidget(m_log, 1);
    rv->addWidget(m_contourCb);
    rv->addSpacing(6);
    auto *hb = new QHBoxLayout;
    hb->addWidget(m_dl); hb->addWidget(m_copy); hb->addStretch(1); hb->addWidget(m_close);
    rv->addLayout(hb);

    auto *hl = new QHBoxLayout(this);
    hl->addWidget(m_tree, 1);
    hl->addWidget(right, 1);
}

/* ---------------- 区域树（Geofabrik 真实路径）---------------- */

QVector<GeoRegion> DownloadDialog::buildRegions() const
{
    /* 中国省级（asia/china/<prov>），本程序主战场，全部列出 */
    QVector<GeoRegion> cn;
    cn << leaf("北京", "asia/china/beijing")
       << leaf("天津", "asia/china/tianjin")
       << leaf("河北", "asia/china/hebei")
       << leaf("山西", "asia/china/shanxi")
       << leaf("内蒙古", "asia/china/inner-mongolia")
       << leaf("辽宁", "asia/china/liaoning")
       << leaf("吉林", "asia/china/jilin")
       << leaf("黑龙江", "asia/china/heilongjiang")
       << leaf("上海", "asia/china/shanghai")
       << leaf("江苏", "asia/china/jiangsu")
       << leaf("浙江", "asia/china/zhejiang")
       << leaf("安徽", "asia/china/anhui")
       << leaf("福建", "asia/china/fujian")
       << leaf("江西", "asia/china/jiangxi")
       << leaf("山东", "asia/china/shandong")
       << leaf("河南", "asia/china/henan")
       << leaf("湖北", "asia/china/hubei")
       << leaf("湖南", "asia/china/hunan")
       << leaf("广东", "asia/china/guangdong")
       << leaf("广西", "asia/china/guangxi")
       << leaf("海南", "asia/china/hainan")
       << leaf("重庆", "asia/china/chongqing")
       << leaf("四川", "asia/china/sichuan")
       << leaf("贵州", "asia/china/guizhou")
       << leaf("云南", "asia/china/yunnan")
       << leaf("西藏", "asia/china/xizang")
       << leaf("陕西", "asia/china/shaanxi")
       << leaf("甘肃", "asia/china/gansu")
       << leaf("青海", "asia/china/qinghai")
       << leaf("宁夏", "asia/china/ningxia")
       << leaf("新疆", "asia/china/xinjiang")
       << leaf("香港", "asia/china/hongkong")
       << leaf("澳门", "asia/china/macau")
       << leaf("台湾", "asia/china/taiwan");

    QVector<GeoRegion> asia;
    asia << leaf("中国（全国）", "asia/china")
          << branch("中国·省级", cn)
          << leaf("日本", "asia/japan")
          << leaf("韩国", "asia/south-korea")
          << leaf("朝鲜", "asia/north-korea")
          << leaf("蒙古", "asia/mongolia")
          << leaf("印度", "asia/india")
          << leaf("巴基斯坦", "asia/pakistan")
          << leaf("孟加拉国", "asia/bangladesh")
          << leaf("尼泊尔", "asia/nepal")
          << leaf("不丹", "asia/bhutan")
          << leaf("斯里兰卡", "asia/sri-lanka")
          << leaf("哈萨克斯坦", "asia/kazakhstan")
          << leaf("乌兹别克斯坦", "asia/uzbekistan")
          << leaf("土库曼斯坦", "asia/turkmenistan")
          << leaf("吉尔吉斯斯坦", "asia/kyrgyzstan")
          << leaf("塔吉克斯坦", "asia/tajikistan")
          << leaf("阿富汗", "asia/afghanistan")
          << leaf("伊朗", "asia/iran")
          << leaf("伊拉克", "asia/iraq")
          << leaf("土耳其", "asia/turkey")
          << leaf("叙利亚", "asia/syria")
          << leaf("约旦", "asia/jordan")
          << leaf("黎巴嫩", "asia/lebanon")
          << leaf("以色列", "asia/israel")
          << leaf("巴勒斯坦", "asia/palestine")
          << leaf("沙特阿拉伯", "asia/saudi-arabia")
          << leaf("也门", "asia/yemen")
          << leaf("阿曼", "asia/oman")
          << leaf("阿联酋", "asia/united-arab-emirates")
          << leaf("卡塔尔", "asia/qatar")
          << leaf("科威特", "asia/kuwait")
          << leaf("巴林", "asia/bahrain")
          << leaf("格鲁吉亚", "asia/georgia")
          << leaf("亚美尼亚", "asia/armenia")
          << leaf("阿塞拜疆", "asia/azerbaijan")
          << leaf("印度尼西亚", "asia/indonesia")
          << leaf("马来西亚", "asia/malaysia")
          << leaf("新加坡", "asia/singapore")
          << leaf("泰国", "asia/thailand")
          << leaf("越南", "asia/vietnam")
          << leaf("菲律宾", "asia/philippines")
          << leaf("缅甸", "asia/myanmar")
          << leaf("柬埔寨", "asia/cambodia")
          << leaf("老挝", "asia/laos")
          << leaf("文莱", "asia/brunei")
          << leaf("东帝汶", "asia/timor-leste");

    QVector<GeoRegion> europe;
    europe << leaf("德国", "europe/germany")
            << leaf("法国", "europe/france")
            << leaf("英国", "europe/united-kingdom")
            << leaf("爱尔兰", "europe/ireland")
            << leaf("意大利", "europe/italy")
            << leaf("西班牙", "europe/spain")
            << leaf("葡萄牙", "europe/portugal")
            << leaf("荷兰", "europe/netherlands")
            << leaf("比利时", "europe/belgium")
            << leaf("卢森堡", "europe/luxembourg")
            << leaf("瑞士", "europe/switzerland")
            << leaf("奥地利", "europe/austria")
            << leaf("列支敦士登", "europe/liechtenstein")
            << leaf("摩纳哥", "europe/monaco")
            << leaf("安道尔", "europe/andorra")
            << leaf("圣马力诺", "europe/san-marino")
            << leaf("马耳他", "europe/malta")
            << leaf("丹麦", "europe/denmark")
            << leaf("瑞典", "europe/sweden")
            << leaf("挪威", "europe/norway")
            << leaf("芬兰", "europe/finland")
            << leaf("冰岛", "europe/iceland")
            << leaf("爱沙尼亚", "europe/estonia")
            << leaf("拉脱维亚", "europe/latvia")
            << leaf("立陶宛", "europe/lithuania")
            << leaf("波兰", "europe/poland")
            << leaf("捷克", "europe/czech-republic")
            << leaf("斯洛伐克", "europe/slovakia")
            << leaf("匈牙利", "europe/hungary")
            << leaf("斯洛文尼亚", "europe/slovenia")
            << leaf("克罗地亚", "europe/croatia")
            << leaf("波黑", "europe/bosnia-herzegovina")
            << leaf("塞尔维亚", "europe/serbia")
            << leaf("黑山", "europe/montenegro")
            << leaf("科索沃", "europe/kosovo")
            << leaf("阿尔巴尼亚", "europe/albania")
            << leaf("北马其顿", "europe/north-macedonia")
            << leaf("罗马尼亚", "europe/romania")
            << leaf("摩尔多瓦", "europe/moldova")
            << leaf("保加利亚", "europe/bulgaria")
            << leaf("希腊", "europe/greece")
            << leaf("塞浦路斯", "europe/cyprus")
            << leaf("俄罗斯（欧洲部分）", "europe/russia")
            << leaf("乌克兰", "europe/ukraine")
            << leaf("白俄罗斯", "europe/belarus");

    QVector<GeoRegion> africa;
    africa << leaf("埃及", "africa/egypt")
            << leaf("利比亚", "africa/libya")
            << leaf("突尼斯", "africa/tunisia")
            << leaf("阿尔及利亚", "africa/algeria")
            << leaf("摩洛哥", "africa/morocco")
            << leaf("毛里塔尼亚", "africa/mauritania")
            << leaf("马里", "africa/mali")
            << leaf("尼日尔", "africa/niger")
            << leaf("乍得", "africa/chad")
            << leaf("苏丹", "africa/sudan")
            << leaf("南苏丹", "africa/south-sudan")
            << leaf("厄立特里亚", "africa/eritrea")
            << leaf("吉布提", "africa/djibouti")
            << leaf("索马里", "africa/somalia")
            << leaf("埃塞俄比亚", "africa/ethiopia")
            << leaf("肯尼亚", "africa/kenya")
            << leaf("乌干达", "africa/uganda")
            << leaf("坦桑尼亚", "africa/tanzania")
            << leaf("卢旺达", "africa/rwanda")
            << leaf("布隆迪", "africa/burundi")
            << leaf("刚果（金）", "africa/democratic-republic-of-congo")
            << leaf("刚果（布）", "africa/congo")
            << leaf("中非", "africa/central-african-republic")
            << leaf("喀麦隆", "africa/cameroon")
            << leaf("加蓬", "africa/gabon")
            << leaf("尼日利亚", "africa/nigeria")
            << leaf("贝宁", "africa/benin")
            << leaf("多哥", "africa/togo")
            << leaf("加纳", "africa/ghana")
            << leaf("科特迪瓦", "africa/ivory-coast")
            << leaf("利比里亚", "africa/liberia")
            << leaf("塞拉利昂", "africa/sierra-leone")
            << leaf("几内亚", "africa/guinea")
            << leaf("塞内加尔", "africa/senegal")
            << leaf("冈比亚", "africa/gambia")
            << leaf("马达加斯加", "africa/madagascar")
            << leaf("南非", "africa/south-africa")
            << leaf("纳米比亚", "africa/namibia")
            << leaf("博茨瓦纳", "africa/botswana")
            << leaf("津巴布韦", "africa/zimbabwe")
            << leaf("赞比亚", "africa/zambia")
            << leaf("莫桑比克", "africa/mozambique")
            << leaf("马拉维", "africa/malawi")
            << leaf("安哥拉", "africa/angola");

    QVector<GeoRegion> namerica;
    namerica << leaf("美国（本土）", "north-america/us")
             << leaf("加拿大", "north-america/canada")
             << leaf("墨西哥", "north-america/mexico")
             << leaf("格陵兰", "north-america/greenland")
             << leaf("百慕大", "north-america/bermuda");

    QVector<GeoRegion> samerica;
    samerica << leaf("巴西", "south-america/brazil")
             << leaf("阿根廷", "south-america/argentina")
             << leaf("智利", "south-america/chile")
             << leaf("玻利维亚", "south-america/bolivia")
             << leaf("秘鲁", "south-america/peru")
             << leaf("厄瓜多尔", "south-america/ecuador")
             << leaf("哥伦比亚", "south-america/colombia")
             << leaf("委内瑞拉", "south-america/venezuela")
             << leaf("圭亚那", "south-america/guyana")
             << leaf("苏里南", "south-america/suriname")
             << leaf("乌拉圭", "south-america/uruguay")
             << leaf("巴拉圭", "south-america/paraguay");

    QVector<GeoRegion> oceania;
    oceania << leaf("澳大利亚", "australia/australia")
            << leaf("新西兰", "australia/new-zealand")
            << leaf("巴布亚新几内亚", "australia/papua-new-guinea")
            << leaf("斐济", "australia/fiji");

    QVector<GeoRegion> root;
    root << branch("亚洲", asia)
         << branch("欧洲", europe)
         << branch("非洲", africa)
         << branch("北美洲", namerica)
         << branch("南美洲", samerica)
         << branch("大洋洲", oceania);
    return root;
}

void DownloadDialog::fillTree(QTreeWidgetItem *parent, const QVector<GeoRegion> &rs)
{
    for (const GeoRegion &r : rs) {
        auto *it = new QTreeWidgetItem(parent);
        it->setText(0, r.name);
        it->setData(0, ROLE_PATH, QVariant(r.path));   /* 枝的 path 为空串 */
        it->setChildIndicatorPolicy(r.kids.isEmpty()
                                    ? QTreeWidgetItem::DontShowIndicator
                                    : QTreeWidgetItem::ShowIndicator);
        if (!r.kids.isEmpty()) fillTree(it, r.kids);
    }
}

/* ---------------- 选中 / 信息 ---------------- */

QString DownloadDialog::currentPath() const
{
    QTreeWidgetItem *it = m_tree->currentItem();
    if (!it) return QString();
    return it->data(0, ROLE_PATH).toString();
}

QString DownloadDialog::currentUrl() const
{
    QString p = currentPath();
    if (p.isEmpty()) return QString();
    return QString("https://download.geofabrik.de/%1-latest.osm.pbf").arg(p);
}

QString DownloadDialog::currentName() const
{
    QString p = currentPath();
    if (p.isEmpty()) return QString();
    return p.section('/', -1);   /* asia/china/gansu → gansu */
}

/* 按叶子名预选：递归找 path 最后一段 == leaf 的节点（如 "hebei" 命中
 * asia/china/hebei）。找到后展开所有祖先再选中，否则 setCurrentItem 选中了
 * 也看不见。setCurrentItem 会发 itemSelectionChanged → onSelChanged（幂等，
 * 只刷 URL/信息行），所以不重复调用。 */
bool DownloadDialog::selectByLeaf(const QString &leaf)
{
    if (leaf.isEmpty() || !m_tree) return false;

    std::function<QTreeWidgetItem *(QTreeWidgetItem *)> walk =
        [&](QTreeWidgetItem *parent) -> QTreeWidgetItem * {
        for (int i = 0; i < parent->childCount(); i++) {
            QTreeWidgetItem *it = parent->child(i);
            QString path = it->data(0, ROLE_PATH).toString();
            if (!path.isEmpty() && path.section('/', -1) == leaf) return it;
            if (QTreeWidgetItem *sub = walk(it)) return sub;   /* 枝继续往下找 */
        }
        return nullptr;
    };

    QTreeWidgetItem *hit = walk(m_tree->invisibleRootItem());
    if (!hit) return false;

    for (QTreeWidgetItem *p = hit->parent(); p; p = p->parent()) p->setExpanded(true);
    m_tree->setCurrentItem(hit);
    m_tree->scrollToItem(hit);
    return true;
}

QString DownloadDialog::currentLeafForTest() const
{
    QString p = currentPath();
    return p.isEmpty() ? QString() : p.section('/', -1);
}

void DownloadDialog::onSelChanged()
{
    QString p = currentPath();
    if (p.isEmpty()) {
        m_url->clear();
        m_info->setText(QString::fromUtf8("请选择最末级地区（带下载链接的）查看详情。"));
        m_dl->setEnabled(false);
        m_copy->setEnabled(false);
        return;
    }
    m_url->setText(currentUrl());
    m_info->setText(QString::fromUtf8("将下载：%1\n保存到：%2/%3-latest.osm.pbf"
                    "\n（若本地已存在，会先比对远端；有更新则询问是否更新）")
                    .arg(currentName()).arg(m_sources).arg(currentName()));
    m_dl->setEnabled(true);
    m_copy->setEnabled(true);
}

void DownloadDialog::copyUrl()
{
    QString u = currentUrl();
    if (!u.isEmpty() && QApplication::clipboard())
        QApplication::clipboard()->setText(u);
}

/* ---------------- 下载 ---------------- */

void DownloadDialog::startDownload()
{
    QString path = currentPath();
    if (path.isEmpty()) return;

    m_dlName = currentName();
    m_target = m_sources + "/" + m_dlName + "-latest.osm.pbf";

    /* 先查本地：pbf 已存在且非空 → 比对远端是否有更新，再决定是否下载。
     * 与 SRTM 的"缺失才下载"一致；存在时不再盲目覆盖，有更新才询问用户。 */
    if (QFile::exists(m_target) && QFileInfo(m_target).size() > 0) {
        m_dl->setEnabled(false); m_copy->setEnabled(false);
        m_log->append(QString::fromUtf8("本地已存在 %1。正在比对远端是否有更新…").arg(m_target));
        QNetworkRequest req(currentUrl());
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
        QNetworkReply *r = m_nam->head(req);
        connect(r, &QNetworkReply::finished, this, [this, r]() { onHeadFinished(r); });
        return;
    }

    beginDownload();
}

void DownloadDialog::beginDownload()
{
    m_log->append(QString::fromUtf8("开始下载：%1\n  → %2").arg(currentUrl(), m_target));

    m_dl->setEnabled(false); m_copy->setEnabled(false);
    m_prog->setValue(0);

    QDir().mkpath(m_sources);
    m_file = new QFile(m_target, this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_log->append(QString::fromUtf8("无法创建目标文件：%1").arg(m_target));
        m_dl->setEnabled(true); m_copy->setEnabled(true);
        return;
    }

    QNetworkRequest req(currentUrl());   /* Qt6 默认跟随重定向，无需额外属性 */
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::downloadProgress,
            this, &DownloadDialog::onDownloadProgress);
    connect(m_reply, &QIODevice::readyRead, this, &DownloadDialog::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this,
            [this]() { onReplyFinished(m_reply); });
}

/* 本地已有 pbf 时，先发 HEAD 比对远端新鲜度：
 *   - 比对失败（无网络/无法访问）→ 稳妥起见跳过下载；
 *   - 远端无更新 → 跳过下载；
 *   - 远端有更新 → 弹窗询问用户是否更新。 */
void DownloadDialog::onHeadFinished(QNetworkReply *r)
{
    r->deleteLater();
    m_dl->setEnabled(true); m_copy->setEnabled(true);

    if (r->error() != QNetworkReply::NoError) {
        m_log->append(QString::fromUtf8("无法比对远端（%1）。为稳妥起见保留本地文件，跳过下载。"
                        "如需强制刷新，请先删除本地文件后重试。").arg(r->errorString()));
        return;
    }

    bool remoteNewer = false;
    QString lm = r->header(QNetworkRequest::LastModifiedHeader).toString();
    if (!lm.isEmpty()) {
        QDateTime remoteMod = QDateTime::fromString(lm, Qt::RFC2822Date);
        if (remoteMod.isValid() && QFileInfo(m_target).lastModified() < remoteMod)
            remoteNewer = true;
    }
    if (!remoteNewer) {
        qint64 remoteSize = r->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        qint64 localSize  = QFileInfo(m_target).size();
        if (remoteSize > 0 && remoteSize != localSize)
            remoteNewer = true;
    }

    if (!remoteNewer) {
        m_log->append(QString::fromUtf8("远端无更新（本地已是最新），跳过下载。"));
        return;
    }

    QMessageBox::StandardButton ret = QMessageBox::question(
        this, QString::fromUtf8("发现更新"),
        QString::fromUtf8("本地已有 %1，远端存在更新版本。是否下载更新？\n（选择\"否\"将保留本地文件。）").arg(m_dlName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret == QMessageBox::Yes) {
        m_log->append(QString::fromUtf8("用户选择更新，开始下载…"));
        beginDownload();
    } else {
        m_log->append(QString::fromUtf8("用户选择保留本地文件，跳过下载。"));
    }
}

void DownloadDialog::onReadyRead()
{
    if (m_file && m_reply) m_file->write(m_reply->readAll());
}

void DownloadDialog::onDownloadProgress(qint64 got, qint64 total)
{
    if (total > 0) m_prog->setValue((int)(got * 100 / total));
}

void DownloadDialog::onReplyFinished(QNetworkReply *r)
{
    if (m_file) { m_file->close(); delete m_file; m_file = nullptr; }

    if (r->error() != QNetworkReply::NoError) {
        m_log->append(QString::fromUtf8("下载失败：%1").arg(r->errorString()));
        m_dl->setEnabled(true); m_copy->setEnabled(true);
        r->deleteLater(); m_reply = nullptr;
        return;
    }
    m_log->append(QString::fromUtf8("下载完成（%1 字节），开始转换…").arg(m_target));
    r->deleteLater(); m_reply = nullptr;

    /* 下载完即触发转换：sync_maps.sh 会扫整个 sources/ 把新 PBF 转成可加载的路网。
     * 异步跑（转换一个省可能几分钟），完成后 downloadFinished 通知主窗口重载。 */
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &DownloadDialog::onConvertOutput);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DownloadDialog::onConvertFinished);
    m_proc->setWorkingDirectory(m_repo);
    m_proc->start(QStringLiteral("bash"),
                 QStringList() << (m_repo + "/scripts/sync_maps.sh"));
    m_log->append(QString::fromUtf8("运行 sync_maps.sh（转换 + 建地名库 + 地标表）…"));
}

void DownloadDialog::onConvertOutput()
{
    if (m_proc) m_log->append(QString::fromLocal8Bit(m_proc->readAllStandardOutput()));
}

void DownloadDialog::onConvertFinished(int code, QProcess::ExitStatus)
{
    if (code == 0) {
        m_log->append(QString::fromUtf8("转换完成 ✓ 地图已就绪。"));
        if (m_onFinished) m_onFinished(m_dlName);
        // 生成等高线（如果用户勾选了）
        if (m_contourCb && m_contourCb->isChecked()) {
            runContoursGeneration();
        }
    } else {
        m_log->append(QString::fromUtf8("转换失败（exit %1）。可在终端手动运行 scripts/sync_maps.sh 查看详情。").arg(code));
    /* 运行等高线生成（若工具可用）*/
void DownloadDialog::runContoursGeneration()
{
    // 省目录路径
    QString provDir = m_repo + "/products/graph/" + m_dlName;
    
    // 用 pbf_info 获取 PBF 的 bbox
    QString pbfPath = m_sources + "/" + m_dlName + "-latest.osm.pbf";
    if (!QFile::exists(pbfPath)) {
        m_log->append(QString::fromUtf8("  [等高线] PBF 文件不存在，跳过生成。"));
        return;
    }
    
    // 检查 pbf_info 工具是否存在
    QString pbfInfo = m_repo + "/build/tools/pbf_info.exe";
    if (!QFile::exists(pbfInfo)) {
        m_log->append(QString::fromUtf8("  [等高线] pbf_info 工具未编译，跳过生成。"));
        return;
    }
    
    QProcess proc;
    proc.setWorkingDirectory(m_repo);
    proc.start(pbfInfo, QStringList() << pbfPath);
    if (!proc.waitForFinished(5000)) {
        m_log->append(QString::fromUtf8("  [等高线] pbf_info 运行超时，跳过生成。"));
        return;
    }
    
    // 解析输出，寻找 "bbox_observed"（由 pbf_info 输出）
    QByteArray out = proc.readAllStandardOutput();
    QList<QByteArray> lines = out.split('\n');
    double lat0 = 0.0, lon0 = 0.0, lat1 = 0.0, lon1 = 0.0;
    bool found = false;
    for (const QByteArray &line : lines) {
        if (line.startsWith("bbox_observed")) {
            QList<QByteArray> parts = line.split(' ');
            if (parts.size() >= 6) {
                lat0 = parts[2].toDouble() / 1e9;
                lon0 = parts[3].toDouble() / 1e9;
                lat1 = parts[4].toDouble() / 1e9;
                lon1 = parts[5].toDouble() / 1e9;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        m_log->append(QString::fromUtf8("  [等高线] 无法从 PBF 提取 bbox，跳过生成。"));
        return;
    }
    m_log->append(QString::fromUtf8("  [等高线] 区域范围：%1,%2 – %3,%4")
                  .arg(lat0, 0, 'f', 4).arg(lon0, 0, 'f', 4)
                  .arg(lat1, 0, 'f', 4).arg(lon1, 0, 'f', 4));
    
    // 首先检查是否已存在等高线文件（避免重复生成）
    QString contPath = provDir + "/contours.bin";
    if (QFile::exists(contPath)) {
        m_log->append(QString::fromUtf8("  [等高线] 检测到已存在等高线文件，跳过生成。"));
        return;
    }
    
    // 创建 SRTM 缓存目录（在项目根下）
    QString srtmDir = m_repo + "/srtm_cache";
    if (!QDir().mkpath(srtmDir)) {
        m_log->append(QString::fromUtf8("  [等高线] 无法创建 SRTM 缓存目录，跳过生成。"));
        return;
    }
    
    // 检查 bbox 覆盖的 SRTM 瓦片，判断是否全部已存在
    int tlat_lo = (int)floor(lat0), tlat_hi = (int)floor(lat1);
    int tlon_lo = (int)floor(lon0), tlon_hi = (int)floor(lon1);
    int totalTiles = 0, existingTiles = 0;
    for (int tlat = tlat_lo; tlat <= tlat_hi; tlat++) {
        for (int tlon = tlon_lo; tlon <= tlon_hi; tlon++) {
            totalTiles++;
            QString ns = (tlat >= 0) ? "N" : "S";
            QString ew = (tlon >= 0) ? "E" : "W";
            QString tileName = QString("%1%2%3%4.hgt")
                                  .arg(ns).arg(abs(tlat), 2, 10, QChar('0'))
                                  .arg(ew).arg(abs(tlon), 3, 10, QChar('0'));
            QString tilePath = srtmDir + "/" + tileName;
            if (QFile::exists(tilePath) && QFileInfo(tilePath).size() > 1000) {
                existingTiles++;
            }
        }
    }
    
    if (totalTiles > 0 && existingTiles == totalTiles) {
        // 所有需要的 SRTM 瓦片都已存在，跳过下载，只生成等高线
        m_log->append(QString::fromUtf8("  [等高线] 区域内 %1 张 SRTM 瓦片已全部存在，仅生成等高线。").arg(totalTiles));
        args.removeOne("--download");
    } else if (existingTiles > 0) {
        m_log->append(QString::fromUtf8("  [等高线] 区域内 %1/%2 张 SRTM 瓦片已存在，只下载缺失的瓦片。").arg(existingTiles).arg(totalTiles));
    }
    
    QString contourExe = m_repo + "/build/gen_contours.exe";
    if (!QFile::exists(contourExe)) {
        m_log->append(QString::fromUtf8("  [等高线] gen_contours 工具未编译，跳过生成。"));
        return;
    }
    
    if (existingTiles == totalTiles) {
        // 所有需要的 SRTM 瓦片都已存在，跳过下载，只生成等高线
        m_log->append(QString::fromUtf8("  [等高线] 区域内 %1 张 SRTM 瓦片已全部存在，仅生成等高线。").arg(totalTiles));
        // 不传 --download 参数，直接使用已有的瓦片生成
    } else if (existingTiles > 0) {
        m_log->append(QString::fromUtf8("  [等高线] 区域内 %1/%2 张 SRTM 瓦片已存在，自动下载缺失的瓦片。").arg(existingTiles).arg(totalTiles));
    }
    
    // 构造命令
    QString bboxStr = QString("%1,%2,%3,%4").arg(lat0).arg(lon0).arg(lat1).arg(lon1);
    QStringList args;
    args << "--out-dir=" + provDir
         << "--hgt-dir=" + srtmDir
         << "--bbox=" + bboxStr
         << "--interval=20"
         << "--index=100";
    
    // 只有当有缺失瓦片时才添加 --download 参数
    if (existingTiles < totalTiles) {
        args << "--download";  // 自动下载缺失瓦片
    }
    
    QProcess genProc;
    genProc.setWorkingDirectory(m_repo);
    genProc.start(contourExe, args);
    connect(&genProc, &QProcess::readyReadStandardOutput,
            [this, &genProc]() {
                m_log->append(QString::fromUtf8("  [等高线] %1").arg(
                    QString::fromLocal8Bit(genProc.readAllStandardOutput()).trimmed()));
            });
    connect(&genProc, &QProcess::readyReadStandardError,
            [this, &genProc]() {
                m_log->append(QString::fromUtf8("  [等高线]⚠ %1").arg(
                    QString::fromLocal8Bit(genProc.readAllStandardError()).trimmed()));
            });
    if (!genProc.waitForFinished(120000)) {  // 2 分钟超时
        m_log->append(QString::fromUtf8("  [等高线] 生成超时（2 分钟），已强制停止。"));
        genProc.kill();
        return;
    }
    int rc = genProc.exitCode();
    if (rc == 0) {
        m_log->append(QString::fromUtf8("  [等高线] 生成完成 ✓ 地图目录已包含等高线数据。"));
    } else {
        m_log->append(QString::fromUtf8("  [等高线] 生成失败（exit %1）。可在命令行手动运行：%2 %3")
                      .arg(rc).arg(QFileInfo(contourExe).fileName()).arg(args.join(" ")));
    }
}
