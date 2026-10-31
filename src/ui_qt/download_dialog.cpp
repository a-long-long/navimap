#include "download_dialog.h"
#include <cmath>

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QBrush>
#include <QColor>
#include <QPair>
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
#include "i18n/i18n.h"

/* 以中文为 key 取翻译（默认中文原样返回）。 */
static QString trd(const char *zh) { return QString::fromUtf8(i18n_tr(zh)); }

/* 树节点存的角色：UserRole = geofabrik path（枝为空串）；
 * UserRole+1 = 节点基础显示名（刷新状态时据此重算后缀，避免后缀叠加） */
static const int ROLE_PATH = Qt::UserRole;
static const int ROLE_NAME = Qt::UserRole + 1;

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
    setWindowTitle(QString::fromUtf8(i18n_tr("下载地图（全球可下载区域 · Geofabrik）")));
    resize(840, 580);

    m_tree = new QTreeWidget;
    m_tree->setHeaderLabel(QString::fromUtf8(i18n_tr("可下载区域（✓已下载 ·未下载）")));
    fillTree(m_tree->invisibleRootItem(), buildRegions());
    refreshStatus();                 /* 计算每个节点的已下载状态/子区域计数 */
    m_tree->expandToDepth(0);        /* 默认展开到洲级 */

    m_info = new QLabel(QString::fromUtf8(i18n_tr("选择左侧一个地区，查看下载链接并下载。")));
    m_url  = new QLineEdit; m_url->setReadOnly(true);
    m_prog = new QProgressBar; m_prog->setRange(0, 100); m_prog->setValue(0);
    m_log  = new QTextEdit; m_log->setReadOnly(true);
    m_dl   = new QPushButton(QString::fromUtf8(i18n_tr("下载并转换"))); m_dl->setEnabled(false);
    m_copy = new QPushButton(QString::fromUtf8(i18n_tr("复制下载链接"))); m_copy->setEnabled(false);
    m_close= new QPushButton(QString::fromUtf8(i18n_tr("关闭")));
    m_contourCb = new QCheckBox(QString::fromUtf8(i18n_tr("同时生成 SRTM 等高线")));

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
    cn << leaf(i18n_tr("北京"), "asia/china/beijing")
       << leaf(i18n_tr("天津"), "asia/china/tianjin")
       << leaf(i18n_tr("河北"), "asia/china/hebei")
       << leaf(i18n_tr("山西"), "asia/china/shanxi")
       << leaf(i18n_tr("内蒙古"), "asia/china/inner-mongolia")
       << leaf(i18n_tr("辽宁"), "asia/china/liaoning")
       << leaf(i18n_tr("吉林"), "asia/china/jilin")
       << leaf(i18n_tr("黑龙江"), "asia/china/heilongjiang")
       << leaf(i18n_tr("上海"), "asia/china/shanghai")
       << leaf(i18n_tr("江苏"), "asia/china/jiangsu")
       << leaf(i18n_tr("浙江"), "asia/china/zhejiang")
       << leaf(i18n_tr("安徽"), "asia/china/anhui")
       << leaf(i18n_tr("福建"), "asia/china/fujian")
       << leaf(i18n_tr("江西"), "asia/china/jiangxi")
       << leaf(i18n_tr("山东"), "asia/china/shandong")
       << leaf(i18n_tr("河南"), "asia/china/henan")
       << leaf(i18n_tr("湖北"), "asia/china/hubei")
       << leaf(i18n_tr("湖南"), "asia/china/hunan")
       << leaf(i18n_tr("广东"), "asia/china/guangdong")
       << leaf(i18n_tr("广西"), "asia/china/guangxi")
       << leaf(i18n_tr("海南"), "asia/china/hainan")
       << leaf(i18n_tr("重庆"), "asia/china/chongqing")
       << leaf(i18n_tr("四川"), "asia/china/sichuan")
       << leaf(i18n_tr("贵州"), "asia/china/guizhou")
       << leaf(i18n_tr("云南"), "asia/china/yunnan")
       << leaf(i18n_tr("西藏"), "asia/china/xizang")
       << leaf(i18n_tr("陕西"), "asia/china/shaanxi")
       << leaf(i18n_tr("甘肃"), "asia/china/gansu")
       << leaf(i18n_tr("青海"), "asia/china/qinghai")
       << leaf(i18n_tr("宁夏"), "asia/china/ningxia")
       << leaf(i18n_tr("新疆"), "asia/china/xinjiang")
       << leaf(i18n_tr("香港"), "asia/china/hongkong")
       << leaf(i18n_tr("澳门"), "asia/china/macau")
       << leaf(i18n_tr("台湾"), "asia/china/taiwan");

    QVector<GeoRegion> asia;
    asia << leaf(i18n_tr("中国（全国）"), "asia/china")
          << branch(i18n_tr("中国·省级"), cn)
          << leaf(i18n_tr("日本"), "asia/japan")
          << leaf(i18n_tr("韩国"), "asia/south-korea")
          << leaf(i18n_tr("朝鲜"), "asia/north-korea")
          << leaf(i18n_tr("蒙古"), "asia/mongolia")
          << leaf(i18n_tr("印度"), "asia/india")
          << leaf(i18n_tr("巴基斯坦"), "asia/pakistan")
          << leaf(i18n_tr("孟加拉国"), "asia/bangladesh")
          << leaf(i18n_tr("尼泊尔"), "asia/nepal")
          << leaf(i18n_tr("不丹"), "asia/bhutan")
          << leaf(i18n_tr("斯里兰卡"), "asia/sri-lanka")
          << leaf(i18n_tr("哈萨克斯坦"), "asia/kazakhstan")
          << leaf(i18n_tr("乌兹别克斯坦"), "asia/uzbekistan")
          << leaf(i18n_tr("土库曼斯坦"), "asia/turkmenistan")
          << leaf(i18n_tr("吉尔吉斯斯坦"), "asia/kyrgyzstan")
          << leaf(i18n_tr("塔吉克斯坦"), "asia/tajikistan")
          << leaf(i18n_tr("阿富汗"), "asia/afghanistan")
          << leaf(i18n_tr("伊朗"), "asia/iran")
          << leaf(i18n_tr("伊拉克"), "asia/iraq")
          << leaf(i18n_tr("土耳其"), "asia/turkey")
          << leaf(i18n_tr("叙利亚"), "asia/syria")
          << leaf(i18n_tr("约旦"), "asia/jordan")
          << leaf(i18n_tr("黎巴嫩"), "asia/lebanon")
          << leaf(i18n_tr("以色列"), "asia/israel")
          << leaf(i18n_tr("巴勒斯坦"), "asia/palestine")
          << leaf(i18n_tr("沙特阿拉伯"), "asia/saudi-arabia")
          << leaf(i18n_tr("也门"), "asia/yemen")
          << leaf(i18n_tr("阿曼"), "asia/oman")
          << leaf(i18n_tr("阿联酋"), "asia/united-arab-emirates")
          << leaf(i18n_tr("卡塔尔"), "asia/qatar")
          << leaf(i18n_tr("科威特"), "asia/kuwait")
          << leaf(i18n_tr("巴林"), "asia/bahrain")
          << leaf(i18n_tr("格鲁吉亚"), "asia/georgia")
          << leaf(i18n_tr("亚美尼亚"), "asia/armenia")
          << leaf(i18n_tr("阿塞拜疆"), "asia/azerbaijan")
          << leaf(i18n_tr("印度尼西亚"), "asia/indonesia")
          << leaf(i18n_tr("马来西亚"), "asia/malaysia")
          << leaf(i18n_tr("新加坡"), "asia/singapore")
          << leaf(i18n_tr("泰国"), "asia/thailand")
          << leaf(i18n_tr("越南"), "asia/vietnam")
          << leaf(i18n_tr("菲律宾"), "asia/philippines")
          << leaf(i18n_tr("缅甸"), "asia/myanmar")
          << leaf(i18n_tr("柬埔寨"), "asia/cambodia")
          << leaf(i18n_tr("老挝"), "asia/laos")
          << leaf(i18n_tr("文莱"), "asia/brunei")
          << leaf(i18n_tr("东帝汶"), "asia/timor-leste");

    QVector<GeoRegion> europe;
    europe << leaf(i18n_tr("德国"), "europe/germany")
            << leaf(i18n_tr("法国"), "europe/france")
            << leaf(i18n_tr("英国"), "europe/united-kingdom")
            << leaf(i18n_tr("爱尔兰"), "europe/ireland")
            << leaf(i18n_tr("意大利"), "europe/italy")
            << leaf(i18n_tr("西班牙"), "europe/spain")
            << leaf(i18n_tr("葡萄牙"), "europe/portugal")
            << leaf(i18n_tr("荷兰"), "europe/netherlands")
            << leaf(i18n_tr("比利时"), "europe/belgium")
            << leaf(i18n_tr("卢森堡"), "europe/luxembourg")
            << leaf(i18n_tr("瑞士"), "europe/switzerland")
            << leaf(i18n_tr("奥地利"), "europe/austria")
            << leaf(i18n_tr("列支敦士登"), "europe/liechtenstein")
            << leaf(i18n_tr("摩纳哥"), "europe/monaco")
            << leaf(i18n_tr("安道尔"), "europe/andorra")
            << leaf(i18n_tr("圣马力诺"), "europe/san-marino")
            << leaf(i18n_tr("马耳他"), "europe/malta")
            << leaf(i18n_tr("丹麦"), "europe/denmark")
            << leaf(i18n_tr("瑞典"), "europe/sweden")
            << leaf(i18n_tr("挪威"), "europe/norway")
            << leaf(i18n_tr("芬兰"), "europe/finland")
            << leaf(i18n_tr("冰岛"), "europe/iceland")
            << leaf(i18n_tr("爱沙尼亚"), "europe/estonia")
            << leaf(i18n_tr("拉脱维亚"), "europe/latvia")
            << leaf(i18n_tr("立陶宛"), "europe/lithuania")
            << leaf(i18n_tr("波兰"), "europe/poland")
            << leaf(i18n_tr("捷克"), "europe/czech-republic")
            << leaf(i18n_tr("斯洛伐克"), "europe/slovakia")
            << leaf(i18n_tr("匈牙利"), "europe/hungary")
            << leaf(i18n_tr("斯洛文尼亚"), "europe/slovenia")
            << leaf(i18n_tr("克罗地亚"), "europe/croatia")
            << leaf(i18n_tr("波黑"), "europe/bosnia-herzegovina")
            << leaf(i18n_tr("塞尔维亚"), "europe/serbia")
            << leaf(i18n_tr("黑山"), "europe/montenegro")
            << leaf(i18n_tr("科索沃"), "europe/kosovo")
            << leaf(i18n_tr("阿尔巴尼亚"), "europe/albania")
            << leaf(i18n_tr("北马其顿"), "europe/north-macedonia")
            << leaf(i18n_tr("罗马尼亚"), "europe/romania")
            << leaf(i18n_tr("摩尔多瓦"), "europe/moldova")
            << leaf(i18n_tr("保加利亚"), "europe/bulgaria")
            << leaf(i18n_tr("希腊"), "europe/greece")
            << leaf(i18n_tr("塞浦路斯"), "europe/cyprus")
            << leaf(i18n_tr("俄罗斯（欧洲部分）"), "europe/russia")
            << leaf(i18n_tr("乌克兰"), "europe/ukraine")
            << leaf(i18n_tr("白俄罗斯"), "europe/belarus");

    QVector<GeoRegion> africa;
    africa << leaf(i18n_tr("埃及"), "africa/egypt")
            << leaf(i18n_tr("利比亚"), "africa/libya")
            << leaf(i18n_tr("突尼斯"), "africa/tunisia")
            << leaf(i18n_tr("阿尔及利亚"), "africa/algeria")
            << leaf(i18n_tr("摩洛哥"), "africa/morocco")
            << leaf(i18n_tr("毛里塔尼亚"), "africa/mauritania")
            << leaf(i18n_tr("马里"), "africa/mali")
            << leaf(i18n_tr("尼日尔"), "africa/niger")
            << leaf(i18n_tr("乍得"), "africa/chad")
            << leaf(i18n_tr("苏丹"), "africa/sudan")
            << leaf(i18n_tr("南苏丹"), "africa/south-sudan")
            << leaf(i18n_tr("厄立特里亚"), "africa/eritrea")
            << leaf(i18n_tr("吉布提"), "africa/djibouti")
            << leaf(i18n_tr("索马里"), "africa/somalia")
            << leaf(i18n_tr("埃塞俄比亚"), "africa/ethiopia")
            << leaf(i18n_tr("肯尼亚"), "africa/kenya")
            << leaf(i18n_tr("乌干达"), "africa/uganda")
            << leaf(i18n_tr("坦桑尼亚"), "africa/tanzania")
            << leaf(i18n_tr("卢旺达"), "africa/rwanda")
            << leaf(i18n_tr("布隆迪"), "africa/burundi")
            << leaf(i18n_tr("刚果（金）"), "africa/democratic-republic-of-congo")
            << leaf(i18n_tr("刚果（布）"), "africa/congo")
            << leaf(i18n_tr("中非"), "africa/central-african-republic")
            << leaf(i18n_tr("喀麦隆"), "africa/cameroon")
            << leaf(i18n_tr("加蓬"), "africa/gabon")
            << leaf(i18n_tr("尼日利亚"), "africa/nigeria")
            << leaf(i18n_tr("贝宁"), "africa/benin")
            << leaf(i18n_tr("多哥"), "africa/togo")
            << leaf(i18n_tr("加纳"), "africa/ghana")
            << leaf(i18n_tr("科特迪瓦"), "africa/ivory-coast")
            << leaf(i18n_tr("利比里亚"), "africa/liberia")
            << leaf(i18n_tr("塞拉利昂"), "africa/sierra-leone")
            << leaf(i18n_tr("几内亚"), "africa/guinea")
            << leaf(i18n_tr("塞内加尔"), "africa/senegal")
            << leaf(i18n_tr("冈比亚"), "africa/gambia")
            << leaf(i18n_tr("马达加斯加"), "africa/madagascar")
            << leaf(i18n_tr("南非"), "africa/south-africa")
            << leaf(i18n_tr("纳米比亚"), "africa/namibia")
            << leaf(i18n_tr("博茨瓦纳"), "africa/botswana")
            << leaf(i18n_tr("津巴布韦"), "africa/zimbabwe")
            << leaf(i18n_tr("赞比亚"), "africa/zambia")
            << leaf(i18n_tr("莫桑比克"), "africa/mozambique")
            << leaf(i18n_tr("马拉维"), "africa/malawi")
            << leaf(i18n_tr("安哥拉"), "africa/angola");

    QVector<GeoRegion> namerica;
    namerica << leaf(i18n_tr("美国（本土）"), "north-america/us")
             << leaf(i18n_tr("加拿大"), "north-america/canada")
             << leaf(i18n_tr("墨西哥"), "north-america/mexico")
             << leaf(i18n_tr("格陵兰"), "north-america/greenland")
             << leaf(i18n_tr("百慕大"), "north-america/bermuda");

    QVector<GeoRegion> samerica;
    samerica << leaf(i18n_tr("巴西"), "south-america/brazil")
             << leaf(i18n_tr("阿根廷"), "south-america/argentina")
             << leaf(i18n_tr("智利"), "south-america/chile")
             << leaf(i18n_tr("玻利维亚"), "south-america/bolivia")
             << leaf(i18n_tr("秘鲁"), "south-america/peru")
             << leaf(i18n_tr("厄瓜多尔"), "south-america/ecuador")
             << leaf(i18n_tr("哥伦比亚"), "south-america/colombia")
             << leaf(i18n_tr("委内瑞拉"), "south-america/venezuela")
             << leaf(i18n_tr("圭亚那"), "south-america/guyana")
             << leaf(i18n_tr("苏里南"), "south-america/suriname")
             << leaf(i18n_tr("乌拉圭"), "south-america/uruguay")
             << leaf(i18n_tr("巴拉圭"), "south-america/paraguay");

    QVector<GeoRegion> oceania;
    oceania << leaf(i18n_tr("澳大利亚"), "australia/australia")
            << leaf(i18n_tr("新西兰"), "australia/new-zealand")
            << leaf(i18n_tr("巴布亚新几内亚"), "australia/papua-new-guinea")
            << leaf(i18n_tr("斐济"), "australia/fiji");

    QVector<GeoRegion> root;
    root << branch(i18n_tr("亚洲"), asia)
         << branch(i18n_tr("欧洲"), europe)
         << branch(i18n_tr("非洲"), africa)
         << branch(i18n_tr("北美洲"), namerica)
         << branch(i18n_tr("南美洲"), samerica)
         << branch(i18n_tr("大洋洲"), oceania);
    return root;
}

void DownloadDialog::fillTree(QTreeWidgetItem *parent, const QVector<GeoRegion> &rs)
{
    for (const GeoRegion &r : rs) {
        auto *it = new QTreeWidgetItem(parent);
        /* 仅搭结构 + 存 path/name；文本与颜色由 refreshStatus() 统一计算，
         * 这样刷新状态不必重建整棵树（保留展开/选中状态）。 */
        it->setData(0, ROLE_PATH, QVariant(r.path));   /* 枝的 path 为空串 */
        it->setData(0, ROLE_NAME, QVariant(r.name));
        it->setChildIndicatorPolicy(r.kids.isEmpty()
                                    ? QTreeWidgetItem::DontShowIndicator
                                    : QTreeWidgetItem::ShowIndicator);
        if (!r.kids.isEmpty()) fillTree(it, r.kids);
    }
}

bool DownloadDialog::regionDownloaded(const QString &path) const
{
    if (path.isEmpty()) return false;              /* 枝不计已下载 */
    QString nm = path.section('/', -1);            /* asia/china/gansu → gansu */
    if (QDir(m_repo + "/products/graph/" + nm).exists()) return true;
    if (QFile::exists(m_sources + "/" + nm + "-latest.osm.pbf")) return true;
    return false;
}

void DownloadDialog::refreshStatus()
{
    QTreeWidgetItem *root = m_tree->invisibleRootItem();
    for (int i = 0; i < root->childCount(); i++)
        refreshItem(root->child(i));
}

QPair<int,int> DownloadDialog::refreshItem(QTreeWidgetItem *it)
{
    QString path = it->data(0, ROLE_PATH).toString();
    QString base = it->data(0, ROLE_NAME).toString();

    if (it->childCount() == 0) {                  /* 叶子：直接判定 */
        bool dl = regionDownloaded(path);
        it->setText(0, QString::fromUtf8(i18n_tr(base.toUtf8().constData())) + (dl ? QString::fromUtf8(i18n_tr("  ✓已下载"))
                                  : QString::fromUtf8(i18n_tr("  ·未下载"))));
        it->setForeground(0, QBrush(dl ? QColor(0, 128, 0) : QColor(120, 120, 120)));
        return qMakePair(1, dl ? 1 : 0);
    }

    int total = 0, d = 0;                          /* 枝：汇总子节点 */
    for (int i = 0; i < it->childCount(); i++) {
        QPair<int,int> c = refreshItem(it->child(i));
        total += c.first; d += c.second;
    }
    it->setText(0, QString::fromUtf8(i18n_tr(base.toUtf8().constData())) + QString::fromUtf8(i18n_tr("  (%1/%2)")).arg(d).arg(total));
    it->setForeground(0, QBrush(Qt::black));
    return qMakePair(total, d);
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

bool DownloadDialog::isLeafDownloaded(const QString &leaf) const
{
    if (leaf.isEmpty() || !m_tree) return false;
    std::function<QString(QTreeWidgetItem *)> walk =
        [&](QTreeWidgetItem *parent) -> QString {
        for (int i = 0; i < parent->childCount(); i++) {
            QTreeWidgetItem *it = parent->child(i);
            QString path = it->data(0, ROLE_PATH).toString();
            if (!path.isEmpty() && path.section('/', -1) == leaf) return path;
            if (QString sub = walk(it); !sub.isEmpty()) return sub;
        }
        return QString();
    };
    QString p = walk(m_tree->invisibleRootItem());
    return !p.isEmpty() && regionDownloaded(p);
}

void DownloadDialog::onSelChanged()
{
    QString p = currentPath();
    if (p.isEmpty()) {
        m_url->clear();
        m_info->setText(QString::fromUtf8(i18n_tr("请选择最末级地区（带下载链接的）查看详情。")));
        m_dl->setEnabled(false);
        m_copy->setEnabled(false);
        return;
    }
    m_url->setText(currentUrl());
    m_info->setText(QString::fromUtf8(i18n_tr("将下载：%1\n保存到：%2/%3-latest.osm.pbf"
                    "\n（若本地已存在，会先比对远端；有更新则询问是否更新）")).arg(currentName()).arg(m_sources).arg(currentName()));
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
        m_log->append(QString::fromUtf8(i18n_tr("本地已存在 %1。正在比对远端是否有更新…")).arg(m_target));
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
    m_log->append(QString::fromUtf8(i18n_tr("开始下载：%1\n  → %2")).arg(currentUrl(), m_target));

    m_dl->setEnabled(false); m_copy->setEnabled(false);
    m_prog->setValue(0);

    QDir().mkpath(m_sources);
    m_file = new QFile(m_target, this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_log->append(QString::fromUtf8(i18n_tr("无法创建目标文件：%1")).arg(m_target));
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
        m_log->append(QString::fromUtf8(i18n_tr("无法比对远端（%1）。为稳妥起见保留本地文件，跳过下载。"
                        "如需强制刷新，请先删除本地文件后重试。")).arg(r->errorString()));
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
        m_log->append(QString::fromUtf8(i18n_tr("远端无更新（本地已是最新），跳过下载。")));
        return;
    }

    QMessageBox::StandardButton ret = QMessageBox::question(
        this, QString::fromUtf8(i18n_tr("发现更新")),
        QString::fromUtf8(i18n_tr("本地已有 %1，远端存在更新版本。是否下载更新？\n（选择\"否\"将保留本地文件。）")).arg(m_dlName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (ret == QMessageBox::Yes) {
        m_log->append(QString::fromUtf8(i18n_tr("用户选择更新，开始下载…")));
        beginDownload();
    } else {
        m_log->append(QString::fromUtf8(i18n_tr("用户选择保留本地文件，跳过下载。")));
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
        m_log->append(QString::fromUtf8(i18n_tr("下载失败：%1")).arg(r->errorString()));
        m_dl->setEnabled(true); m_copy->setEnabled(true);
        r->deleteLater(); m_reply = nullptr;
        return;
    }
    m_log->append(QString::fromUtf8(i18n_tr("下载完成（%1 字节），开始转换…")).arg(m_target));
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
    m_log->append(QString::fromUtf8(i18n_tr("运行 sync_maps.sh（转换 + 建地名库 + 地标表）…")));
}

void DownloadDialog::onConvertOutput()
{
    if (m_proc) m_log->append(QString::fromLocal8Bit(m_proc->readAllStandardOutput()));
}

void DownloadDialog::onConvertFinished(int code, QProcess::ExitStatus)
{
    if (code == 0) {
        m_log->append(QString::fromUtf8(i18n_tr("转换完成 ✓ 地图已就绪。")));
        refreshStatus();                 /* 刚下载的省翻成「✓已下载」 */
        if (m_onFinished) m_onFinished(m_dlName);
        // 生成等高线（如果用户勾选了）
        if (m_contourCb && m_contourCb->isChecked()) {
            runContoursGeneration();
        }
    } else {
        m_log->append(QString::fromUtf8(i18n_tr("转换失败（exit %1）。可在终端手动运行 scripts/sync_maps.sh 查看详情。")).arg(code));
    }
}

/* 运行等高线生成（若工具可用）*/
void DownloadDialog::runContoursGeneration()
{
    // 省目录路径
    QString provDir = m_repo + "/products/graph/" + m_dlName;
    
    // 用 skedaddle_pbf_info 获取 PBF 的 bbox
    QString pbfPath = m_sources + "/" + m_dlName + "-latest.osm.pbf";
    if (!QFile::exists(pbfPath)) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] PBF 文件不存在，跳过生成。")));
        return;
    }
    
    // 检查 skedaddle_pbf_info 工具是否存在（构建产物在 build/ 顶层，历史曾在 build/tools/，两者都探）
    QString pbfInfo = m_repo + "/build/skedaddle_pbf_info.exe";
    if (!QFile::exists(pbfInfo))
        pbfInfo = m_repo + "/build/tools/pbf_info.exe";
    if (!QFile::exists(pbfInfo)) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] skedaddle_pbf_info 工具未编译，跳过生成。")));
        return;
    }
    
    QProcess proc;
    proc.setWorkingDirectory(m_repo);
    proc.start(pbfInfo, QStringList() << pbfPath);
    if (!proc.waitForFinished(5000)) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] skedaddle_pbf_info 运行超时，跳过生成。")));
        return;
    }
    
    // 解析输出，寻找 "bbox_observed"（由 skedaddle_pbf_info 输出）
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
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 无法从 PBF 提取 bbox，跳过生成。")));
        return;
    }
    m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 区域范围：%1,%2 – %3,%4"))
                  .arg(lat0, 0, 'f', 4).arg(lon0, 0, 'f', 4)
                  .arg(lat1, 0, 'f', 4).arg(lon1, 0, 'f', 4));
    
    // 首先检查是否已存在该区域的等高线文件（避免重复生成）
    QString contPath = provDir + "/" + m_dlName + "_contours.bin";
    if (!QFile::exists(contPath)) contPath = provDir + "/contours.bin";  // 旧命名回退
    if (QFile::exists(contPath)) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 检测到已存在 %1，跳过生成。")).arg(contPath));
        return;
    }
    
    // 创建 SRTM 缓存目录（在项目根下）
    QString srtmDir = m_repo + "/srtm_cache";
    if (!QDir().mkpath(srtmDir)) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 无法创建 SRTM 缓存目录，跳过生成。")));
        return;
    }
    
    // 检查 bbox 覆盖的 SRTM 瓦片，判断是否全部已存在
    int tlat_lo = (int)std::floor(lat0), tlat_hi = (int)std::floor(lat1);
    int tlon_lo = (int)std::floor(lon0), tlon_hi = (int)std::floor(lon1);
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
    
    // 瓦片去重：仅当 existingTiles < totalTiles 才在下方追加 --download（见后文）

    
    QString contourExe = m_repo + "/build/skedaddle_gen_contours.exe";
    if (!QFile::exists(contourExe)) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] skedaddle_gen_contours 工具未编译，跳过生成。")));
        return;
    }
    
    if (existingTiles == totalTiles) {
        // 所有需要的 SRTM 瓦片都已存在，跳过下载，只生成等高线
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 区域内 %1 张 SRTM 瓦片已全部存在，仅生成等高线。")).arg(totalTiles));
        // 不传 --download 参数，直接使用已有的瓦片生成
    } else if (existingTiles > 0) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 区域内 %1/%2 张 SRTM 瓦片已存在，自动下载缺失的瓦片。")).arg(existingTiles).arg(totalTiles));
    }
    
    // 构造命令：等高线文件按区域命名（<name>_contours.bin 等）
    QString bboxStr = QString("%1,%2,%3,%4").arg(lat0).arg(lon0).arg(lat1).arg(lon1);
    QStringList args;
    args << "--out-dir=" + provDir
         << "--hgt-dir=" + srtmDir
         << "--prefix=" + m_dlName
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
                m_log->append(QString::fromUtf8(i18n_tr("  [等高线] %1")).arg(
                    QString::fromLocal8Bit(genProc.readAllStandardOutput()).trimmed()));
            });
    connect(&genProc, &QProcess::readyReadStandardError,
            [this, &genProc]() {
                m_log->append(QString::fromUtf8(i18n_tr("  [等高线]⚠ %1")).arg(
                    QString::fromLocal8Bit(genProc.readAllStandardError()).trimmed()));
            });
    if (!genProc.waitForFinished(120000)) {  // 2 分钟超时
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 生成超时（2 分钟），已强制停止。")));
        genProc.kill();
        return;
    }
    int rc = genProc.exitCode();
    if (rc == 0) {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 生成完成 ✓ 地图目录已包含等高线数据。")));
    } else {
        m_log->append(QString::fromUtf8(i18n_tr("  [等高线] 生成失败（exit %1）。可在命令行手动运行：%2 %3"))
                      .arg(rc).arg(QFileInfo(contourExe).fileName()).arg(args.join(" ")));
    }
}

void DownloadDialog::retranslateUi()
{
    setWindowTitle(trd("下载地图（全球可下载区域 · Geofabrik）"));
    if (m_tree) m_tree->setHeaderLabel(trd("可下载区域（✓已下载 ·未下载）"));
    if (m_info) {
        QTreeWidgetItem *sel = m_tree ? m_tree->currentItem() : nullptr;
        if (sel && !sel->data(0, ROLE_PATH).toString().isEmpty())
            onSelChanged();   /* 重算选中项信息行（走 i18n） */
        else
            m_info->setText(trd("选择左侧一个地区，查看下载链接并下载。"));
    }
    if (m_dl)        m_dl->setText(trd("下载并转换"));
    if (m_copy)      m_copy->setText(trd("复制下载链接"));
    if (m_close)     m_close->setText(trd("关闭"));
    if (m_contourCb) m_contourCb->setText(trd("同时生成 SRTM 等高线"));
    refreshStatus();   /* 重画整棵树（叶子/枝名 + ✓/· 后缀） */
}
