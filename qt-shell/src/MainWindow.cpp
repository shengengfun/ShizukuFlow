#include "MainWindow.h"

#include "PreviewPane.h"
#include "TimelinePane.h"
#include "core/sf_audio.h"
#include "core/sf_edit.h"
#include "core/sf_lrc.h"
#include "core/sf_presets.h"
#include "core/sf_render.h"
#include "theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSpinBox>
#include <QStandardPaths>
#include <QEvent>
#include <QKeySequence>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRadialGradient>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtMath>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

// ═════════════════════════════════════════════════════════════════════════════
// 小部件与辅助函数
// ═════════════════════════════════════════════════════════════════════════════

// 背景部件不一定排在 background[0] —— 照片底图、角色立绘都可能顶在它前面。
// 按下标写会把 kind 塞进立绘的 params、让背景下拉看起来"生效了但画面没变"。
// 按 part 名找第一个；找不到才补一个。
//
// 补的位置是**数组末尾**：场景图之后的 background 渐变现在是「文字衬底」
// （左浓右透），它属于压在底图上、垫在文字下的一层，不再是最底层。
static QJsonObject *backgroundParamsOf(sf::Shot &s)
{
    for (auto &part : s.background) {
        if (part.part == QLatin1String("background"))
            return &part.params;
    }
    sf::PartRef pr;
    pr.part = QStringLiteral("background");
    pr.valid = true;
    s.background.push_back(pr);
    return &s.background.last().params;
}

static QString backgroundKindOf(const sf::Shot &s)
{
    for (const auto &part : s.background) {
        if (part.part == QLatin1String("background"))
            return part.params.value(QStringLiteral("kind")).toString();
    }
    return QString();
}

// background 现在是混合数组：底图 image、立绘 portrait、渐变 background 三种共存，
// 数组顺序就是绘制顺序（底图 → 立绘 → 文字）。所以定位一律走 part（必要时再加
// path），任何 background[0] 的写法都会在「先插了底图/立绘」的镜头上改错对象。
static sf::PartRef *findBackgroundPart(sf::Shot &s, const QString &part,
                                       const QString &path = QString())
{
    for (auto &pr : s.background) {
        if (pr.part != part)
            continue;
        if (!path.isEmpty() && pr.params.value(QStringLiteral("path")).toString() != path)
            continue;
        return &pr;
    }
    return nullptr;
}

static const sf::PartRef *findBackgroundPart(const sf::Shot &s, const QString &part,
                                             const QString &path = QString())
{
    for (const auto &pr : s.background) {
        if (pr.part != part)
            continue;
        if (!path.isEmpty() && pr.params.value(QStringLiteral("path")).toString() != path)
            continue;
        return &pr;
    }
    return nullptr;
}

/**
 * 立绘的「组」：同一个 path 在**相邻镜头**里重复出现，共用一组 at/outAt。
 *
 * 判定只看 path + 时间相邻（不依赖生成器已经删掉的 grp 字段）：
 *   - 同 track
 *   - 中间没有断档的镜头（断档 = 人物已经下台，不该跟着变）
 *   - 前后镜头在时间上首尾相接（间隔 < 0.05s）
 * 返回包含 anchor 的那一段镜头下标（升序）。
 */
static QVector<int> portraitGroup(const sf::Project &p, int anchor, const QString &path)
{
    QVector<int> out;
    if (anchor < 0 || anchor >= p.shots.size() || path.isEmpty())
        return out;

    const int track = p.shots.at(anchor).track;
    QVector<int> hits;
    for (int i = 0; i < p.shots.size(); ++i) {
        if (p.shots.at(i).track != track)
            continue;
        if (findBackgroundPart(p.shots.at(i), QStringLiteral("portrait"), path))
            hits.push_back(i);
    }
    const int at = hits.indexOf(anchor);
    if (at < 0)
        return out;

    int lo = at;
    int hi = at;
    while (lo > 0 && hits.at(lo) - hits.at(lo - 1) == 1
           && p.shots.at(hits.at(lo)).start - p.shots.at(hits.at(lo - 1)).end < 0.05) {
        --lo;
    }
    while (hi + 1 < hits.size() && hits.at(hi + 1) - hits.at(hi) == 1
           && p.shots.at(hits.at(hi + 1)).start - p.shots.at(hits.at(hi)).end < 0.05) {
        ++hi;
    }
    for (int k = lo; k <= hi; ++k)
        out.push_back(hits.at(k));
    return out;
}

// 视频默认插在 background 层，但也允许挂在 decor（压在文字之下、图片之上）
static sf::PartRef *findVideoPart(sf::Shot &s)
{
    for (auto &pr : s.background) {
        if (pr.part == QLatin1String("video"))
            return &pr;
    }
    for (auto &pr : s.decor) {
        if (pr.part == QLatin1String("video"))
            return &pr;
    }
    return nullptr;
}

static const sf::PartRef *findVideoPart(const sf::Shot &s)
{
    for (const auto &pr : s.background) {
        if (pr.part == QLatin1String("video"))
            return &pr;
    }
    for (const auto &pr : s.decor) {
        if (pr.part == QLatin1String("video"))
            return &pr;
    }
    return nullptr;
}

/** 工程音轨的绝对路径：工程里可能存相对 .sf 的路径（与导出、渲染同一套解析）。 */
static QString resolveProjectAudio(const sf::Project &p)
{
    if (p.audioPath.isEmpty())
        return QString();
    const QFileInfo fi(p.audioPath);
    if (fi.isAbsolute())
        return fi.exists() ? fi.absoluteFilePath() : QString();
    const QString base = p.path.isEmpty() ? QDir::currentPath() : QFileInfo(p.path).absolutePath();
    const QString cand = QDir(base).absoluteFilePath(p.audioPath);
    return QFileInfo::exists(cand) ? QFileInfo(cand).absoluteFilePath() : QString();
}

/** ffmpeg：显式环境变量 → exe 旁边的 tools/ → PATH。BPM 检测与导出共用一套找法。 */
static QString resolveFfmpeg()
{
    const QByteArray env = qgetenv("SHIZUKUFLOW_FFMPEG");
    if (!env.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(env)))
        return QString::fromLocal8Bit(env);
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList cands = {
        dir + QStringLiteral("/tools/ffmpeg.exe"),
        dir + QStringLiteral("/../../app/src-tauri/tools/ffmpeg.exe"),
        dir + QStringLiteral("/../app/src-tauri/tools/ffmpeg.exe"),
    };
    for (const QString &c : cands)
        if (QFileInfo::exists(c))
            return c;
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

/**
 * 找仓库里的素材：先 CWD，再 exe 目录，各往上探 4 层。
 * selftest 以前只认 CWD，在 build 目录里跑就找不到 assets/ —— 那是测试的锅，不是功能的。
 */
static QString findRepoAsset(const QString &rel)
{
    QStringList roots = {QDir::currentPath(), QCoreApplication::applicationDirPath()};
    for (const QString &base : roots) {
        QDir d(base);
        for (int up = 0; up <= 4; ++up) {
            const QString cand = d.absoluteFilePath(rel);
            if (QFileInfo::exists(cand) && QFileInfo(cand).isFile())
                return QFileInfo(cand).absoluteFilePath();
            if (!d.cdUp())
                break;
        }
    }
    return QString();
}

// 染色色板：下标与「染色」下拉一一对应；0 = 不染色（空串 ⇒ 删掉 tint / tintAmount）
static const QStringList &tintSwatches()
{
    static const QStringList s = {QString(), QStringLiteral("#9fd8ff"), QStringLiteral("#ffd08a"),
                                  QStringLiteral("#ff7ab6"), QStringLiteral("#7fe0c0")};
    return s;
}

// 旋转目标用「层号:下标」定位，层号 0=background 1=text 2=decor 3=overlay
static QVector<sf::PartRef> *layerParts(sf::Shot &s, int layer)
{
    switch (layer) {
    case 0:
        return &s.background;
    case 1:
        return &s.text;
    case 2:
        return &s.decor;
    case 3:
        return &s.overlay;
    default:
        return nullptr;
    }
}

static const QVector<sf::PartRef> *layerParts(const sf::Shot &s, int layer)
{
    switch (layer) {
    case 0:
        return &s.background;
    case 1:
        return &s.text;
    case 2:
        return &s.decor;
    case 3:
        return &s.overlay;
    default:
        return nullptr;
    }
}

namespace {

// ── logo 圆点：原型用的是 `box-shadow:0 0 8px` 发光，QSS 没有 box-shadow，
//    这里用径向渐变直接画，比 QGraphicsDropShadowEffect 更轻也更准
class LogoMark : public QWidget
{
public:
    explicit LogoMark(QWidget *parent = nullptr) : QWidget(parent) { setFixedSize(14, 14); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor a = Theme::c(Theme::Col::Accent);
        const QPointF ctr(rect().center());
        const qreal rad = width() / 2.0;

        QRadialGradient g(ctr, rad);
        g.setColorAt(0.00, QColor(a.red(), a.green(), a.blue(), 170));
        g.setColorAt(0.45, QColor(a.red(), a.green(), a.blue(), 55));
        g.setColorAt(1.00, QColor(a.red(), a.green(), a.blue(), 0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(rect());

        p.setBrush(a);
        const qreal core = rad * 0.62;
        p.drawEllipse(ctr, core, core);
    }
};

// ── 自绘标题栏（原型 .menubar 的沉浸版）
//    用 std::function 回调而不是 Q_OBJECT：匿名命名空间里加 Q_OBJECT 还得额外
//    include moc 产物，收益不抵复杂度。子控件（菜单按钮）自己收点击，
//    QLabel 默认忽略鼠标事件会冒泡上来，正好实现「点空白拖动窗口」。
class TitleBar : public QWidget
{
public:
    explicit TitleBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_StyledBackground, true);
        setFixedHeight(Theme::M::MenuH);
    }

    std::function<void(const QPoint &)> onDragStart;
    std::function<void(const QPoint &)> onDragMove;
    std::function<void()> onDragEnd;
    std::function<void()> onToggleMaximize;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(e);
            return;
        }
        if (onDragStart)
            onDragStart(e->globalPosition().toPoint());
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!onDragMove || !(e->buttons() & Qt::LeftButton)) {
            QWidget::mouseMoveEvent(e);
            return;
        }
        onDragMove(e->globalPosition().toPoint());
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onDragEnd)
            onDragEnd();
        QWidget::mouseReleaseEvent(e);
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && onToggleMaximize) {
            onToggleMaximize();
            e->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(e);
    }
};

// ── 部件树 delegate：分类行画折叠三角 + 计数，部件行画 16×12 小方块
//    （原型 .cat-hd / .part 的 .ico，QSS 画不出来，只能自绘）
class PartDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &idx) const override
    {
        return QSize(0, idx.parent().isValid() ? 22 : 26);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        const bool isCat = !idx.parent().isValid();
        const bool sel = opt.state & QStyle::State_Selected;
        const bool hov = opt.state & QStyle::State_MouseOver;
        const bool open = opt.state & QStyle::State_Open;
        const QRect r = opt.rect;

        if (sel)
            p->fillRect(r, Theme::c(Theme::Col::Accent, 0x26));
        else if (hov)
            p->fillRect(r, Theme::c(Theme::Col::Panel2));

        const int cy = r.center().y();
        const QColor fg = sel ? Theme::c(Theme::Col::Accent)
                              : (hov ? Theme::c(Theme::Col::Tx) : Theme::c(Theme::Col::Tx2));

        if (isCat) {
            // 折叠三角（原型 ▶ / 展开时旋转 90° 向下）
            p->setPen(Qt::NoPen);
            p->setBrush(Theme::c(Theme::Col::Tx3));
            const qreal cxp = r.left() + 14.0;
            QPolygonF tri;
            if (open) {
                tri << QPointF(cxp - 4, cy - 2) << QPointF(cxp + 4, cy - 2) << QPointF(cxp, cy + 3);
            } else {
                tri << QPointF(cxp - 2, cy - 4) << QPointF(cxp - 2, cy + 4) << QPointF(cxp + 3, cy);
            }
            p->drawPolygon(tri);

            p->setFont(Theme::ui(Theme::F::Small));
            p->setPen(fg);
            p->drawText(QRect(r.left() + 24, r.top(), r.width() - 24, r.height()),
                        Qt::AlignVCenter | Qt::AlignLeft, idx.data(Qt::DisplayRole).toString());

            const int cnt = idx.data(Qt::UserRole).toInt();
            if (cnt > 0) {
                p->setFont(Theme::mono(Theme::F::Micro));
                p->setPen(Theme::c(Theme::Col::Tx3));
                p->drawText(QRect(r.left(), r.top(), r.width() - 10, r.height()),
                            Qt::AlignVCenter | Qt::AlignRight, QString::number(cnt));
            }
        } else {
            // 16×12 圆角 2 的小方块
            const QRectF ico(r.left() + 24, cy - 6.0, 16, 12);
            p->setPen(Qt::NoPen);
            p->setBrush(sel ? Theme::c(Theme::Col::Accent) : Theme::c(Theme::Col::Panel3));
            p->drawRoundedRect(ico, 2, 2);

            p->setFont(Theme::ui(Theme::F::Tiny));
            p->setPen(fg);
            p->drawText(QRect(r.left() + 47, r.top(), r.width() - 47, r.height()),
                        Qt::AlignVCenter | Qt::AlignLeft, idx.data(Qt::DisplayRole).toString());
        }
        p->restore();
    }
};

// ── 缓动曲线预览（44px 高，等价于原型 .ease-curve canvas）
class EaseCurve : public QWidget
{
public:
    explicit EaseCurve(QWidget *parent = nullptr) : QWidget(parent) { setFixedHeight(44); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        QPainterPath box;
        box.addRoundedRect(r, Theme::M::RSm, Theme::M::RSm);
        p.fillPath(box, Theme::c(Theme::Col::Bg));
        p.setPen(QPen(Theme::c(Theme::Col::Line), 1));
        p.drawPath(box);

        p.save();
        p.setClipPath(box);

        p.setPen(QPen(Theme::c(Theme::Col::LineSoft), 1));
        for (int i = 1; i < 4; ++i) {
            const qreal x = r.left() + r.width() * i / 4.0;
            const qreal y = r.top() + r.height() * i / 4.0;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }

        // expo.out —— 与原型 EASES['expo.out'] 同一函数
        p.setPen(QPen(Theme::c(Theme::Col::Accent), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath curve;
        const qreal pad = 4;
        for (int i = 0; i <= 60; ++i) {
            const qreal t = i / 60.0;
            const qreal y = t >= 1.0 ? 1.0 : 1.0 - qPow(2.0, -10.0 * t);
            const QPointF pt(r.left() + pad + t * (r.width() - 2 * pad),
                             r.bottom() - pad - y * (r.height() - 2 * pad));
            if (i == 0)
                curve.moveTo(pt);
            else
                curve.lineTo(pt);
        }
        p.drawPath(curve);
        p.restore();
    }
};

// ── 通用构造件
QLabel *mkLabel(const QString &text, int px, const char *color,
                int weight = QFont::Normal, qreal ls = 0.0)
{
    auto *l = new QLabel(text);
    l->setFont(Theme::ui(px, weight, ls));
    l->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                         .arg(QString::fromLatin1(color)));
    return l;
}

QPushButton *mkBtn(const QString &text, const char *role = nullptr, int h = Theme::M::BtnH)
{
    auto *b = new QPushButton(text);
    if (role)
        b->setProperty("role", QString::fromLatin1(role));
    b->setFont(Theme::ui(role && qstrcmp(role, "sm") == 0 ? Theme::F::Small : Theme::F::Body));
    b->setFixedHeight(h);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QWidget *vsep()
{
    auto *f = new QFrame;
    f->setFixedSize(1, 18);
    f->setStyleSheet(QStringLiteral("background:%1;border:none;").arg(Theme::Col::Line));
    return f;
}

QLabel *mkChip(const QString &html)
{
    auto *l = new QLabel(html);
    l->setTextFormat(Qt::RichText);
    l->setProperty("role", "chip");
    l->setFont(Theme::ui(Theme::F::Tiny));
    l->setFixedHeight(Theme::M::ChipH);
    l->setAlignment(Qt::AlignCenter);
    return l;
}

QFrame *mkPanelHead(const QString &text, QWidget *right = nullptr)
{
    auto *f = new QFrame;
    f->setProperty("role", "panelHead");
    f->setFixedHeight(Theme::M::PanelHeadH);
    auto *l = new QHBoxLayout(f);
    l->setContentsMargins(Theme::M::Gutter, 0, Theme::M::Gutter, 0);
    l->setSpacing(6);
    l->addWidget(mkLabel(text, Theme::F::Micro, Theme::Col::Tx3, QFont::DemiBold, 0.5));
    l->addStretch(1);
    if (right)
        l->addWidget(right);
    return f;
}

QComboBox *mkCombo(const QStringList &items)
{
    auto *c = new QComboBox;
    c->addItems(items);
    c->setFixedHeight(Theme::M::InputH);
    c->setFont(Theme::ui(Theme::F::Small));
    c->setCursor(Qt::PointingHandCursor);
    c->setFocusPolicy(Qt::NoFocus);
    return c;
}

QSlider *mkSlider(int lo, int hi, int val)
{
    auto *s = new QSlider(Qt::Horizontal);
    s->setRange(lo, hi);
    s->setValue(val);
    s->setFixedHeight(12);
    s->setCursor(Qt::PointingHandCursor);
    s->setFocusPolicy(Qt::NoFocus);
    return s;
}

// 0.08 → ".08"，-0.08 → "-.08"，1.12 → "1.12"（与原型 .val 的显示一致）
QString fmtFrac(double v)
{
    QString s = QString::number(v, 'f', 2);
    if (s.startsWith(QLatin1String("0.")))
        s.remove(0, 1);
    else if (s.startsWith(QLatin1String("-0.")))
        s = QLatin1Char('-') + s.mid(2);
    return s;
}

void bindSlider(QSlider *s, QLabel *v, int kind)
{
    auto upd = [s, v, kind]() {
        const int x = s->value();
        QString t;
        switch (kind) {
        case 0: t = QString::number(x); break;              // 整数
        case 1: t = fmtFrac(x / 100.0); break;              // .08
        case 2: t = QString::number(x / 100.0, 'f', 2); break; // 1.12
        default: t = fmtFrac(x / 1000.0); break;            // -.08
        }
        v->setText(t);
    };
    QObject::connect(s, &QSlider::valueChanged, v, [upd](int) { upd(); });
    upd();
}

QLabel *mkVal()
{
    auto *l = new QLabel;
    l->setFixedWidth(40);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->setFont(Theme::mono(Theme::F::Mono));
    l->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(Theme::Col::Tx));
    return l;
}

QWidget *rowWithLabel(const QString &name, QWidget *field)
{
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);
    auto *n = mkLabel(name, Theme::F::Small, Theme::Col::Tx2);
    n->setFixedWidth(56);
    l->addWidget(n);
    l->addWidget(field, 1);
    return w;
}

QWidget *sliderRow(const QString &name, QSlider *s, QLabel *v)
{
    auto *w = rowWithLabel(name, s);
    qobject_cast<QHBoxLayout *>(w->layout())->addWidget(v);
    return w;
}

struct Sect
{
    QFrame *f;
    QVBoxLayout *l;
};

Sect mkSection(const QString &title)
{
    auto *f = new QFrame;
    f->setStyleSheet(QStringLiteral("border-bottom:1px solid %1;").arg(Theme::Col::LineSoft));
    auto *l = new QVBoxLayout(f);
    l->setContentsMargins(11, 9, 11, 12);
    l->setSpacing(8);
    l->addWidget(mkLabel(title, Theme::F::Micro, Theme::Col::Tx3, QFont::DemiBold, 0.5));
    return {f, l};
}

// 编辑点跳转：上一镜 / 下一镜（基于工程镜头起点序列）
double prevShot(const QVector<double> &starts, double t)
{
    double best = 0.0;
    for (double s : starts)
        if (s < t - 0.02)
            best = s;
    return best;
}

double nextShot(const QVector<double> &starts, double t)
{
    for (double s : starts)
        if (s > t + 0.02)
            return s;
    return t;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// MainWindow
// ═════════════════════════════════════════════════════════════════════════════
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ShizukuFlow"));
    // 无边框：标题栏由 buildMenuBar 自绘（原型 .menubar 的沉浸版）。
    // 系统那条标题栏会把深色界面从顶上割开，也带不进原型的 logo 与按钮排布。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    resize(1460, 900);
    setMinimumSize(1180, 740);
    setAcceptDrops(true); // 图片/视频可以直接拖进窗口

    auto *central = new QWidget;
    central->setObjectName(QStringLiteral("sfCentral"));
    central->setAttribute(Qt::WA_StyledBackground, true);
    // 无边框窗口没有系统描边，用 1px 线色 padding 补出轮廓，夜底色下才不糊在桌面上。
    // 选择器必须限定到 objectName：裸声明会连带染到所有子控件。
    central->setStyleSheet(QStringLiteral("#sfCentral{background:%1;}").arg(Theme::Col::Line));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);
    root->addWidget(buildMenuBar());
    root->addWidget(buildToolbar());
    root->addWidget(buildWorkspace(), 1);
    root->addWidget(buildStatusBar());
    setCentralWidget(central);

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::tick);

    onSeek(0.0);

    // 无头自测：等 main() 完成载入工程后再跑（工程不在构造函数里加载）
    if (QCoreApplication::arguments().contains(QStringLiteral("--selftest")))
        QTimer::singleShot(600, this, &MainWindow::runSelfTest);
}

// ── 启动阶段（供 Splash 分步调用）───────────────────────────────────────────
bool MainWindow::bootLoadProject()
{
    const bool ok = loadDefaultProject();
    onSeek(0.0);
    return ok;
}

void MainWindow::warmupFirstFrame()
{
    if (m_preview)
        m_preview->warmup();
}

// ── 工程载入 ────────────────────────────────────────────────────────────────
bool MainWindow::loadProject(const QString &path)
{
    QString err;
    sf::Project p;
    if (!p.load(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("打开工程失败"), err);
        return false;
    }

    m_project = p;
    m_projectPath = path;
    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    if (m_preview)
        m_preview->setEngine(&m_project, &m_renderer);
    if (m_timeline)
        m_timeline->setProject(&m_project);
    m_undo.clear();
    m_redo.clear();
    m_selShotId.clear();
    m_dirty = false;
    refreshInspector(true);
    updateUndoActions();
    setWindowTitle(QStringLiteral("%1 — ShizukuFlow").arg(m_project.title));
    if (m_fileChip)
        m_fileChip->setText(QFileInfo(m_projectPath).fileName());
    if (m_kernelLabel)
        m_kernelLabel->setText(QStringLiteral("内核已接入 · QPainter 后端 · <span style=\"color:%1\">%2</span>")
                                   .arg(Theme::Col::Ok, QFileInfo(m_project.path).fileName()));
    if (m_resLabel)
        m_resLabel->setText(QStringLiteral("预览 <span style=\"color:%1\">%2×%3</span>（工程画幅）")
                                .arg(Theme::Col::Tx)
                                .arg(m_project.canvas.width)
                                .arg(m_project.canvas.height));
    if (m_fileChip)
        m_fileChip->setText(QStringLiteral("<span style=\"color:%1\">●</span> 工程 "
                                           "<span style=\"color:%2\"><b>%3.sf</b></span>")
                                .arg(Theme::Col::Ok, Theme::Col::Tx,
                                     QFileInfo(m_project.path).completeBaseName()));
    if (m_fmtChip)
        m_fmtChip->setText(QString::asprintf("%dx%d · %dfps", m_project.canvas.width,
                                             m_project.canvas.height, m_project.canvas.fps));
    setWindowTitle(QStringLiteral("ShizukuFlow — %1%2")
                       .arg(m_project.title,
                            m_project.artist.isEmpty()
                                ? QString()
                                : QStringLiteral(" · ") + m_project.artist));
    m_t = 0.0;
    onSeek(0.0);
    updateEngineStats();
    return true;
}

bool MainWindow::loadDefaultProject()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    // 私有工程（含真实歌词，不随仓库分发）优先；找不到就回落到随仓库附带的示例。
    QStringList candidates;
    for (const QString &name : {QStringLiteral("余韻"), QStringLiteral("sample")}) {
        candidates << QStringLiteral("projects/%1.sf").arg(name)
                   << exeDir + QStringLiteral("/../../projects/%1.sf").arg(name)
                   << exeDir + QStringLiteral("/../../../projects/%1.sf").arg(name);
    }
    for (const QString &c : candidates) {
        const QString abs = QFileInfo(c).absoluteFilePath();
        if (QFileInfo::exists(abs))
            return loadProject(abs);
    }
    return false;
}

// ── 菜单栏 32px ──────────────────────────────────────────────────────────────
QWidget *MainWindow::buildMenuBar()
{
    auto *bar = new TitleBar;
    bar->setObjectName(QStringLiteral("sfTitleBar"));
    // 作用域限定到 objectName：裸声明会连带染到子控件，把菜单按钮的 hover 底盖掉
    bar->setStyleSheet(QStringLiteral("#sfTitleBar{background:%1;border-bottom:1px solid %2;}")
                           .arg(Theme::Col::Panel, Theme::Col::Line));

    auto *l = new QHBoxLayout(bar);
    l->setContentsMargins(8, 0, 8, 0);
    l->setSpacing(2);

    l->addWidget(new LogoMark(bar));
    l->addSpacing(2);
    l->addWidget(mkLabel(QStringLiteral("ShizukuFlow"), Theme::F::Body, Theme::Col::Accent,
                         QFont::Bold, 0.3));
    l->addSpacing(10);

    auto addMenu = [&](const QString &name, const std::function<void(QMenu *)> &fill) {
        auto *b = new QPushButton(name);
        b->setProperty("role", QStringLiteral("menu"));
        b->setFixedHeight(24);
        b->setFont(Theme::ui(Theme::F::Body));
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        auto *m = new QMenu(b);
        fill(m);
        b->setMenu(m);
        l->addWidget(b);
    };

    addMenu(QStringLiteral("文件"), [this](QMenu *m) {
        auto add = [m](const QString &text, const QKeySequence &key) {
            auto *a = m->addAction(text);
            if (!key.isEmpty())
                a->setShortcut(key);
            return a;
        };

        connect(add(QStringLiteral("新建工程"), QKeySequence::New), &QAction::triggered, this,
                &MainWindow::newProject);
        connect(add(QStringLiteral("打开工程…"), QKeySequence::Open), &QAction::triggered, this,
                [this] {
                    const QString path = QFileDialog::getOpenFileName(
                        this, QStringLiteral("打开 ShizukuFlow 工程"), QString(),
                        QStringLiteral("ShizukuFlow 工程 (*.sf)"));
                    if (!path.isEmpty()) {
                        togglePlay(false);
                        loadProject(path);
                    }
                });
        connect(add(QStringLiteral("保存工程"), QKeySequence::Save), &QAction::triggered, this,
                &MainWindow::saveProject);
        connect(add(QStringLiteral("另存为…"), QKeySequence::SaveAs), &QAction::triggered, this,
                &MainWindow::saveProjectAs);
        m->addSeparator();
        // 歌词 .lrc 双向：制作流程里歌词几乎都先落在 .lrc
        connect(add(QStringLiteral("导入歌词 (LRC)…"), QKeySequence()), &QAction::triggered, this,
                &MainWindow::importLyricsLrc);
        connect(add(QStringLiteral("导出歌词 (LRC)…"), QKeySequence()), &QAction::triggered, this,
                &MainWindow::exportLyricsLrc);
        m->addSeparator();
        connect(add(QStringLiteral("导出当前帧…"), QKeySequence(QStringLiteral("Ctrl+E"))),
                &QAction::triggered, this, &MainWindow::exportCurrentFrame);
        connect(add(QStringLiteral("导出视频…"), QKeySequence(QStringLiteral("Ctrl+Shift+E"))),
                &QAction::triggered, this, &MainWindow::exportVideo);
        m->addSeparator();
        connect(add(QStringLiteral("打开工程目录"), QKeySequence()), &QAction::triggered, this,
                &MainWindow::openProjectDir);
        connect(add(QStringLiteral("退出"), QKeySequence::Quit), &QAction::triggered, this,
                [this] { close(); });
    });

    addMenu(QStringLiteral("编辑"), [this](QMenu *m) {
        m_undoAct = m->addAction(QStringLiteral("撤销"));
        m_undoAct->setShortcut(QKeySequence::Undo);
        connect(m_undoAct, &QAction::triggered, this, &MainWindow::undoEdit);
        m_redoAct = m->addAction(QStringLiteral("重做"));
        m_redoAct->setShortcut(QKeySequence::Redo);
        connect(m_redoAct, &QAction::triggered, this, &MainWindow::redoEdit);
        m->addSeparator();
        auto *del = m->addAction(QStringLiteral("删除（镜头 / 部件）"));
        del->setShortcut(QKeySequence::Delete);
        connect(del, &QAction::triggered, this, &MainWindow::deleteSelectedShot);
        updateUndoActions();
    });

    addMenu(QStringLiteral("视图"), [this](QMenu *m) {
        // 取景辅助线（预览叠加层，不进导出画面）
        auto guide = [this, m](const QString &text, int mode, const QKeySequence &key) {
            auto *a = m->addAction(text);
            a->setCheckable(true);
            if (!key.isEmpty())
                a->setShortcut(key);
            connect(a, &QAction::triggered, this, [this, mode] {
                if (m_preview)
                    m_preview->setGuideMode(mode);
            });
            return a;
        };
        guide(QStringLiteral("演出画面"), 0, QKeySequence());
        guide(QStringLiteral("安全框"), 1, QKeySequence(QStringLiteral("Ctrl+1")));
        guide(QStringLiteral("三分构图"), 2, QKeySequence(QStringLiteral("Ctrl+2")));
        guide(QStringLiteral("网格"), 3, QKeySequence(QStringLiteral("Ctrl+3")));
        m->addSeparator();

        // 时间轴缩放
        auto *fit = m->addAction(QStringLiteral("适配全片（时间轴）"));
        fit->setShortcut(QKeySequence(QStringLiteral("Shift+F")));
        connect(fit, &QAction::triggered, this, [this] {
            if (m_timeline)
                m_timeline->fitAll();
        });

        auto zoomAct = [this, m](const QString &text, const QKeySequence &key, double f) {
            auto *a = m->addAction(text);
            if (!key.isEmpty())
                a->setShortcut(key);
            connect(a, &QAction::triggered, this, [this, f] {
                if (m_timeline)
                    m_timeline->zoomBy(f);
            });
        };
        zoomAct(QStringLiteral("放大（时间轴）"), QKeySequence(QStringLiteral("Ctrl+=")), 1.5);
        zoomAct(QStringLiteral("缩小（时间轴）"), QKeySequence(QStringLiteral("Ctrl+-")), 1.0 / 1.5);
    });

    addMenu(QStringLiteral("时间轴"), [this](QMenu *m) {
        auto add = [this, m](const QString &text, const QKeySequence &key, auto fn) {
            auto *a = m->addAction(text);
            if (!key.isEmpty())
                a->setShortcut(key);
            connect(a, &QAction::triggered, this, fn);
            return a;
        };
        add(QStringLiteral("播放 / 暂停"), QKeySequence(QStringLiteral("Space")),
            [this] { togglePlay(!m_playing); });
        m->addSeparator();
        add(QStringLiteral("回到开头"), QKeySequence(Qt::Key_Home), [this] { onSeek(0.0); });
        add(QStringLiteral("到末尾"), QKeySequence(Qt::Key_End),
            [this] { onSeek(m_project.duration()); });
        m->addSeparator();
        add(QStringLiteral("上一镜头"), QKeySequence(QStringLiteral("Ctrl+Left")),
            [this] { onSeek(prevShot(m_shotStarts, m_t)); });
        add(QStringLiteral("下一镜头"), QKeySequence(QStringLiteral("Ctrl+Right")),
            [this] { onSeek(nextShot(m_shotStarts, m_t)); });
    });

    addMenu(QStringLiteral("演出"), [this](QMenu *m) {
        auto *roll = m->addAction(QStringLiteral("换一个（重掷种子 + 换背景）"));
        roll->setShortcut(QKeySequence(QStringLiteral("R")));
        connect(roll, &QAction::triggered, this, &MainWindow::rerollShot);
        m->addAction(QStringLiteral("锁定镜头（未接入：Shot 尚无 locked 字段）"))->setEnabled(false);
    });

    addMenu(QStringLiteral("渲染"), [this](QMenu *m) {
        connect(m->addAction(QStringLiteral("导出当前帧…")), &QAction::triggered, this,
                &MainWindow::exportCurrentFrame);
        connect(m->addAction(QStringLiteral("导出视频…")), &QAction::triggered, this,
                &MainWindow::exportVideo);
        m->addAction(QStringLiteral("渲染队列（未接入）"))->setEnabled(false);
    });

    addMenu(QStringLiteral("工具"), [this](QMenu *m) {
        connect(m->addAction(QStringLiteral("打开工程目录")), &QAction::triggered, this,
                &MainWindow::openProjectDir);
        connect(m->addAction(QStringLiteral("检测 BPM")), &QAction::triggered, this,
                &MainWindow::detectBpm);
    });

    addMenu(QStringLiteral("帮助"), [this](QMenu *m) {
        connect(m->addAction(QStringLiteral("快捷键…")), &QAction::triggered, this,
                &MainWindow::showShortcuts);
        connect(m->addAction(QStringLiteral("关于 ShizukuFlow")), &QAction::triggered, this,
                [this] {
                    QMessageBox::about(
                        this, QStringLiteral("关于 ShizukuFlow"),
                        QStringLiteral("ShizukuFlow — Qt 编辑器\n\n"
                                       "Qt 骨架 · C++ 渲染内核 · 命令式编辑层\n"
                                       "设计 token 取自 prototype/gui-v2.html"));
                });
    });

    l->addStretch(1);

    // ── 窗口控制按钮（原型 .menubar .win：38×24，关闭键 hover 转红）
    auto mkWinBtn = [&](const QString &glyph, const char *role, const QString &tip) {
        auto *b = new QPushButton(glyph, bar);
        b->setProperty("role", QString::fromLatin1(role));
        b->setFixedSize(38, 24);
        b->setToolTip(tip);
        b->setCursor(Qt::ArrowCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFont(Theme::ui(Theme::F::Small));
        l->addWidget(b);
        return b;
    };
    m_minBtn = mkWinBtn(QStringLiteral("—"), "wbtn", QStringLiteral("最小化"));
    m_maxBtn = mkWinBtn(QStringLiteral("□"), "wbtn", QStringLiteral("最大化"));
    m_closeBtn = mkWinBtn(QStringLiteral("✕"), "wclose", QStringLiteral("关闭"));
    connect(m_minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(m_maxBtn, &QPushButton::clicked, this, &MainWindow::toggleMaximizedWindow);
    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::close);

    // 拖动分区：空白处 / logo / 标题文字。双击空白 = 最大化切换
    bar->onDragStart = [this](const QPoint &g) { beginWindowDrag(g); };
    bar->onDragMove = [this](const QPoint &g) { dragWindow(g); };
    bar->onDragEnd = [this] { endWindowDrag(); };
    bar->onToggleMaximize = [this] { toggleMaximizedWindow(); };

    return bar;
}

// ── 自绘标题栏：拖动 / 最大化 ───────────────────────────────────────────────
void MainWindow::beginWindowDrag(const QPoint &globalPos)
{
    m_dragging = true;
    if (m_maximized) {
        // 最大化状态下拖标题栏要先把窗口还原，并让光标仍落在原来相对的位置
        const QRect cur = geometry();
        const qreal ratio = cur.width() > 0
                                ? qreal(globalPos.x() - cur.x()) / qreal(cur.width())
                                : 0.5;
        setMaximizedWindow(false);
        const QRect restored = geometry();
        QPoint target(int(globalPos.x() - restored.width() * ratio),
                      int(globalPos.y() - Theme::M::MenuH / 2));
        const QRect avail = screen() ? screen()->availableGeometry()
                                     : QGuiApplication::primaryScreen()->availableGeometry();
        target.setY(std::max(avail.top(), std::min(target.y(), avail.bottom() - 40)));
        move(target);
    }
    m_dragGlobal = globalPos;
    m_dragWindowPos = pos();
}

void MainWindow::dragWindow(const QPoint &globalPos)
{
    if (!m_dragging)
        return;
    move(m_dragWindowPos + (globalPos - m_dragGlobal));
}

void MainWindow::endWindowDrag()
{
    m_dragging = false;
}

void MainWindow::toggleMaximizedWindow()
{
    setMaximizedWindow(!m_maximized);
}

void MainWindow::setMaximizedWindow(bool on)
{
    if (on == m_maximized)
        return;
    if (on) {
        m_normalGeometry = geometry();
        const QScreen *scr = screen() ? screen() : QGuiApplication::primaryScreen();
        // 用 availableGeometry 而不是 showMaximized：无边框窗口铺满全屏会盖住任务栏
        if (scr)
            setGeometry(scr->availableGeometry());
    } else if (m_normalGeometry.isValid()) {
        setGeometry(m_normalGeometry);
    }
    m_maximized = on;
    syncWindowButtons();
}

void MainWindow::syncWindowButtons()
{
    if (!m_maxBtn)
        return;
    m_maxBtn->setText(m_maximized ? QStringLiteral("❐") : QStringLiteral("□"));
    m_maxBtn->setToolTip(m_maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        // 系统层面（Win+↓ 等）改变了状态时，自绘按钮要跟上
        const bool maxed = isMaximized() || isFullScreen();
        if (maxed != m_maximized) {
            m_maximized = maxed;
            if (!maxed && m_normalGeometry.isValid())
                setGeometry(m_normalGeometry);
            syncWindowButtons();
        }
    }
}

// ── 文件拖放：图片走 image/portrait，视频走 video（同一套去重 + 撤销）──────────
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    for (const QUrl &u : event->mimeData()->urls()) {
        const QString p = u.toLocalFile();
        if (!p.isEmpty() && QFileInfo(p).isFile()) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    static const QStringList okSuffix = {
        QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("mp4"), QStringLiteral("mov"),
        QStringLiteral("webm")};

    int added = 0;
    for (const QUrl &u : event->mimeData()->urls()) {
        const QString p = u.toLocalFile();
        if (p.isEmpty() || !QFileInfo(p).isFile())
            continue;
        if (!okSuffix.contains(QFileInfo(p).suffix().toLower()))
            continue;
        insertImagePlate(p); // 内部按扩展名分流，并各自压一条撤销
        ++added;
    }
    if (added > 0)
        event->acceptProposedAction();
    else
        event->ignore();
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // 无边框窗口没有 WS_THICKFRAME，边缘缩放要靠 WM_NCHITTEST 自己报热区。
    // 只报边框，其余一律交回 Qt —— 这样标题栏内的按钮/菜单照常收点击。
    if (eventType == "windows_generic_MSG") {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == WM_NCHITTEST && !m_maximized && !isFullScreen()) {
            RECT wr{};
            if (::GetWindowRect(reinterpret_cast<HWND>(winId()), &wr)) {
                const int bw = std::max(4, int(6 * devicePixelRatioF()));
                const int x = msg->pt.x - wr.left;
                const int y = msg->pt.y - wr.top;
                const int w = wr.right - wr.left;
                const int h = wr.bottom - wr.top;
                const bool left = x < bw, right = x >= w - bw;
                const bool top = y < bw, bottom = y >= h - bw;
                if (top && left)
                    *result = HTTOPLEFT;
                else if (top && right)
                    *result = HTTOPRIGHT;
                else if (bottom && left)
                    *result = HTBOTTOMLEFT;
                else if (bottom && right)
                    *result = HTBOTTOMRIGHT;
                else if (left)
                    *result = HTLEFT;
                else if (right)
                    *result = HTRIGHT;
                else if (top)
                    *result = HTTOP;
                else if (bottom)
                    *result = HTBOTTOM;
                else
                    return QMainWindow::nativeEvent(eventType, message, result);
                return true;
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

// ── 工具栏 42px ─────────────────────────────────────────────────────────────
QWidget *MainWindow::buildToolbar()
{
    auto *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(Theme::M::ToolbarH);
    bar->setStyleSheet(QStringLiteral("background:%1;border-bottom:1px solid %2;")
                           .arg(Theme::Col::Panel2, Theme::Col::Line));

    auto *l = new QHBoxLayout(bar);
    l->setContentsMargins(Theme::M::Gutter, 0, Theme::M::Gutter, 0);
    l->setSpacing(Theme::M::Gap);

    auto addSm = [&](const QString &t, const char *role = "ghost", const QString &tip = {}) {
        auto *b = mkBtn(t, role, Theme::M::BtnSmH);
        b->setFont(Theme::ui(Theme::F::Small));
        if (!tip.isEmpty())
            b->setToolTip(tip);
        l->addWidget(b);
        return b;
    };

    addSm(QStringLiteral("新建"));
    addSm(QStringLiteral("打开"));
    addSm(QStringLiteral("保存"));
    l->addWidget(vsep());

    auto *undo = addSm(QStringLiteral("↶"), "ghost", QStringLiteral("撤销"));
    auto *redo = addSm(QStringLiteral("↷"), "ghost", QStringLiteral("重做"));
    undo->setEnabled(false);
    redo->setEnabled(false);
    l->addWidget(vsep());

    addSm(QStringLiteral("换一个"), "sm", QStringLiteral("重掷随机种子（R）"))->setEnabled(false);
    addSm(QStringLiteral("锁定镜头"), "ghost", QStringLiteral("锁定当前镜头，禁止自动重排"));
    l->addWidget(vsep());

    l->addWidget(mkLabel(QStringLiteral("吸附"), Theme::F::Small, Theme::Col::Tx3));
    auto *snap = mkCombo({QStringLiteral("节拍"), QStringLiteral("小节"), QStringLiteral("逐字"),
                          QStringLiteral("关闭")});
    snap->setFixedWidth(88);
    snap->setFixedHeight(26);
    l->addWidget(snap);
    l->addWidget(vsep());

    auto *bpm = addSm(QStringLiteral("检测 BPM"), "ghost",
                      QStringLiteral("从音轨自相关分析 BPM 与首拍"));
    connect(bpm, &QPushButton::clicked, this, &MainWindow::detectBpm);

    l->addStretch(1);

    // 主题切换 seg（原型是 ☀/🌙/⚙ 三个图标，这里改文字，不用 emoji）
    auto *segBox = new QFrame;
    segBox->setProperty("role", "segbox");
    segBox->setFixedHeight(26);
    auto *sl = new QHBoxLayout(segBox);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(0);
    auto *grp = new QButtonGroup(segBox);
    const QStringList themes = {QStringLiteral("浅"), QStringLiteral("深"),
                                QStringLiteral("自动")};
    for (int i = 0; i < themes.size(); ++i) {
        auto *b = mkBtn(themes[i], "seg", 26);
        b->setCheckable(true);
        b->setChecked(i == 1);
        grp->addButton(b, i);
        sl->addWidget(b);
    }
    l->addWidget(segBox);
    l->addWidget(vsep());

    m_fileChip = mkChip(QStringLiteral("<span style=\"color:%1\">●</span> 工程 "
                                       "<span style=\"color:%2\"><b>—</b></span>")
                            .arg(Theme::Col::Ok, Theme::Col::Tx));
    l->addWidget(m_fileChip);
    m_fmtChip = mkChip(QStringLiteral("—"));
    l->addWidget(m_fmtChip);
    l->addWidget(vsep());

    auto *exp = addSm(QStringLiteral("导出"), "ghost");
    exp->setEnabled(false);
    auto *primary = mkBtn(QStringLiteral("打开工程…"), "primary", Theme::M::BtnSmH);
    primary->setFont(Theme::ui(Theme::F::Small, QFont::DemiBold));
    connect(primary, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("打开 ShizukuFlow 工程"), QString(),
            QStringLiteral("ShizukuFlow 工程 (*.sf)"));
        if (!path.isEmpty()) {
            togglePlay(false);
            loadProject(path);
        }
    });
    l->addWidget(primary);

    return bar;
}

// ── 工作区：左 216 / 中弹性 / 右 268 ────────────────────────────────────────
QWidget *MainWindow::buildWorkspace()
{
    auto *ws = new QWidget;
    auto *l = new QHBoxLayout(ws);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(buildLibrary());
    l->addWidget(buildCenter(), 1);
    l->addWidget(buildInspector());
    return ws;
}

QWidget *MainWindow::buildLibrary()
{
    auto *lib = new QFrame;
    lib->setFixedWidth(Theme::M::LeftW);
    lib->setStyleSheet(QStringLiteral("background:%1;border:none;border-right:1px solid %2;")
                           .arg(Theme::Col::Panel, Theme::Col::Line));

    auto *l = new QVBoxLayout(lib);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);

    l->addWidget(mkPanelHead(QStringLiteral("演出部件库")));

    auto *searchWrap = new QWidget;
    auto *swl = new QVBoxLayout(searchWrap);
    swl->setContentsMargins(8, 8, 8, 8);
    auto *search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("搜索部件…"));
    search->setFixedHeight(Theme::M::InputTallH);
    search->setFont(Theme::ui(Theme::F::Small));
    swl->addWidget(search);
    l->addWidget(searchWrap);

    auto *tagWrap = new QWidget;
    tagWrap->setFixedHeight(52); // 两行 18 + 间距 4 + 下边距 8
    auto *tgl = new QVBoxLayout(tagWrap);
    tgl->setContentsMargins(8, 0, 8, 8);
    tgl->setSpacing(4);
    const QStringList tags = {QStringLiteral("全部"), QStringLiteral("文字PV系"),
                              QStringLiteral("动能"), QStringLiteral("Glitch"),
                              QStringLiteral("和风"), QStringLiteral("Horror")};
    for (int row = 0; row < 2; ++row) {
        auto *rowBox = new QWidget;
        auto *rl = new QHBoxLayout(rowBox);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(4);
        for (int col = 0; col < 3; ++col) {
            const int i = row * 3 + col;
            auto *t = new QPushButton(tags[i]);
            t->setCheckable(true);
            t->setChecked(i == 0);
            t->setFixedHeight(18);
            t->setCursor(Qt::PointingHandCursor);
            t->setFocusPolicy(Qt::NoFocus);
            t->setFont(Theme::ui(Theme::F::Micro));
            t->setStyleSheet(QStringLiteral(
                                 "QPushButton{background:%1;border:none;border-radius:9px;"
                                 "padding:0 8px;color:%2;}"
                                 "QPushButton:checked{background:%3;color:%4;font-weight:600;}")
                                 .arg(Theme::Col::Panel3, Theme::Col::Tx3,
                                      Theme::rgba(Theme::Col::Accent, 0x3c),
                                      Theme::Col::Accent));
            rl->addWidget(t);
        }
        rl->addStretch(1);
        tgl->addWidget(rowBox);
    }
    l->addWidget(tagWrap);

    l->addWidget(buildPartTree(), 1);
    return lib;
}

QTreeWidget *MainWindow::buildPartTree()
{
    struct Cat
    {
        const char *name;
        int count;
        QStringList items;
    };
    // 分类与条目照搬原型 CATS（10 类）
    const QVector<Cat> cats = {
        {"布局 Layout", 10, {"hero.center 居中主字", "split.duo 双分", "column.vertical 竖排",
                             "grid.3x3 九宫", "stagger 阶梯", "frame.rule 规则框", "edge.push 贴边",
                             "radial 放射", "type.wall 字墙", "track.left 左轨"}},
        {"登场 Entrance", 8, {"slide.up 上滑", "cut.hard 硬切", "scale.pop 弹入", "mask.wipe 擦除",
                              "glitch.in 故障入场", "type.write 逐字写入", "blur.in 聚焦",
                              "flip.x 翻转"}},
        {"保持 Hold", 6, {"micro.drift 微漂移", "pulse.beat 节拍脉冲", "flicker 闪烁",
                          "breathe 呼吸", "jitter 抖动", "shake.hold 持续震"}},
        {"退场 Exit", 6, {"cut.hard 硬切", "fade.out 淡出", "slide.down 下滑",
                          "scale.collapse 塌陷", "glitch.out 故障退", "mask.split 分裂"}},
        {"装饰 Decor", 8, {"frame.rule 细线框", "cross.mark 十字", "grid.dot 点阵",
                           "line.flow 流线", "burst.ray 放射线", "blob.organic 有机形",
                           "wave.ocean 海浪", "golden.spiral 黄金螺旋"}},
        {"文字加工 Type", 7, {"outline 描边", "stroke.fill 描边填充", "layer.echo 重影",
                              "glow 辉光", "slice.text 切片", "vertical.sub 竖排副文",
                              "mask.reveal 遮罩"}},
        {"背景 Backdrop", 6, {"solid 纯色", "gradient 渐变", "triangle.grid 三角网格",
                              "checker 棋盘", "noise.film 胶片颗粒", "image.plate 图版"}},
        {"相机 Camera", 7, {"push.in 推近", "pull.out 拉远", "pan.x 横移", "tilt.y 纵移",
                            "roll 滚转", "shake 震动", "zoom.punch 冲击"}},
        {"画面效果 FX", 8, {"grain 颗粒", "chroma 色差", "vignette 暗角", "scanline 扫描线",
                            "glitch.slice 切片故障", "halftone 半调", "invert 反相", "bloom 辉光"}},
        {"转场 Transition", 7, {"cut 硬切", "slice.h 横向切割", "slice.v 纵向切割", "wipe 擦除",
                                "flash 闪白", "glitch.cut 故障切", "push 推移"}},
    };

    auto *tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(false);
    tree->setIndentation(0);
    tree->setFrameShape(QFrame::NoFrame);
    tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->setMouseTracking(true);
    tree->setFocusPolicy(Qt::NoFocus);
    tree->setItemDelegate(new PartDelegate(tree));
    tree->setFont(Theme::ui(Theme::F::Small));

    // ── 真实预设（sf::partPresets）：双击就往对应层插一个「引擎真的能画出来」的部件。
    //    和下面那堆原型占位名的区别是：这些插进去立刻有画面。
    {
        const QVector<sf::PartPreset> presets = sf::partPresets();
        QStringList order;
        QHash<QString, QVector<sf::PartPreset>> byGroup;
        for (const sf::PartPreset &ps : presets) {
            if (!byGroup.contains(ps.group))
                order << ps.group;
            byGroup[ps.group].push_back(ps);
        }
        for (const QString &g : order) {
            auto *gi = new QTreeWidgetItem(tree);
            gi->setText(0, g);
            gi->setData(0, Qt::UserRole, int(byGroup.value(g).size()));
            gi->setFlags(Qt::ItemIsEnabled);
            gi->setExpanded(true);
            for (const sf::PartPreset &ps : byGroup.value(g)) {
                auto *ch = new QTreeWidgetItem(gi);
                ch->setText(0, ps.label);
                // 预设名放 UserRole+1：UserRole 留给「我的素材」的文件路径
                ch->setData(0, Qt::UserRole + 1, ps.label);
                ch->setToolTip(0,
                               QStringLiteral("双击插入 %1（层 %2）").arg(ps.part, ps.layer));
                ch->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            }
        }
    }

    // ── 原型 10 类占位名：引擎并不认识，置灰 + tooltip，别再让人双击后一头雾水
    for (int i = 0; i < cats.size(); ++i) {
        auto *ci = new QTreeWidgetItem(tree);
        ci->setText(0, QString::fromUtf8(cats[i].name));
        ci->setData(0, Qt::UserRole, cats[i].count);
        ci->setFlags(Qt::ItemIsEnabled);
        ci->setToolTip(0, QStringLiteral("未实现：原型里的排版占位分类"));
        for (const QString &it : cats[i].items) {
            auto *ch = new QTreeWidgetItem(ci);
            ch->setText(0, it);
            ch->setFlags(Qt::NoItemFlags);
            ch->setToolTip(0, QStringLiteral("原型占位名：引擎没有这个部件，请用上面的预设"));
        }
        ci->setExpanded(i < 3);
    }

    // ── 我的素材：列真实图片文件，双击插进当前镜头
    {
        auto *mat = new QTreeWidgetItem(tree);
        mat->setText(0, QStringLiteral("我的素材（双击插入）"));
        mat->setFlags(Qt::ItemIsEnabled);

        QStringList files;
        const QStringList dirs = {
            QDir::currentPath() + QStringLiteral("/assets"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/../../assets"),
            QCoreApplication::applicationDirPath() + QStringLiteral("/../../../assets"),
        };
        for (const QString &d : dirs) {
            QDir dd(d);
            if (!dd.exists())
                continue;
            const QFileInfoList list =
                dd.entryInfoList({QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                                  QStringLiteral("*.jpeg"), QStringLiteral("*.webp"),
                                  QStringLiteral("*.mp4"), QStringLiteral("*.mov"),
                                  QStringLiteral("*.webm")},
                                 QDir::Files, QDir::Name);
            for (const QFileInfo &fi : list)
                files << fi.absoluteFilePath();
            if (!files.isEmpty())
                break;
        }
        if (files.isEmpty()) {
            auto *ch = new QTreeWidgetItem(mat);
            ch->setText(0, QStringLiteral("（assets/ 下没有图片或视频）"));
            ch->setFlags(Qt::NoItemFlags);
        } else {
            for (const QString &f : files) {
                auto *ch = new QTreeWidgetItem(mat);
                ch->setText(0, QFileInfo(f).fileName());
                ch->setToolTip(0, f);
                ch->setData(0, Qt::UserRole, f);
                ch->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            }
        }
        mat->setExpanded(true);
    }

    connect(tree, &QTreeWidget::itemClicked, tree, [](QTreeWidgetItem *it, int) {
        if (it && !it->parent())
            it->setExpanded(!it->isExpanded());
    });
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it, int) {
        if (!it || !it->parent())
            return;
        // 预设：往对应层插部件；素材：按扩展名走图片/立绘/视频
        const QString preset = it->data(0, Qt::UserRole + 1).toString();
        if (!preset.isEmpty()) {
            insertPartPreset(preset);
            return;
        }
        const QString path = it->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            insertImagePlate(path);
    });
    // 布局未稳定时视口可能停在非零滚动位，显示一帧后强制回顶
    QTimer::singleShot(0, tree, &QTreeWidget::scrollToTop);

    return tree;
}

QWidget *MainWindow::buildCenter()
{
    auto *c = new QWidget;
    c->setAttribute(Qt::WA_StyledBackground, true);
    c->setStyleSheet(QStringLiteral("background:%1;").arg(Theme::Col::Bg));
    auto *l = new QVBoxLayout(c);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(buildPreviewWrap(), 1);

    m_timeline = new TimelinePane;
    m_timeline->setFixedHeight(Theme::M::TimelineH);
    connect(m_timeline, &TimelinePane::seeked, this, &MainWindow::onSeek);
    connect(m_timeline, &TimelinePane::playToggled, this, &MainWindow::togglePlay);
    connect(m_timeline, &TimelinePane::editRequested, this, &MainWindow::applyEditCmd);
    connect(m_timeline, &TimelinePane::batchEditRequested, this, &MainWindow::applyEditBatch);
    connect(m_timeline, &TimelinePane::selectionChanged, this,
            [this](const QString &id, bool isShot) {
                if (!isShot) {
                    // 取消选中：检查器回落到「播放头所在镜头」
                    if (m_selShotId.isEmpty())
                        return;
                    m_selShotId.clear();
                    refreshInspector(true);
                    return;
                }
                if (id.isEmpty() || m_selShotId == id)
                    return;
                m_selShotId = id;
                refreshInspector(true);
            });
    connect(m_timeline, &TimelinePane::stepShot, this, [this](int dir) {
        onSeek(dir < 0 ? prevShot(m_shotStarts, m_t) : nextShot(m_shotStarts, m_t));
    });
    m_playBtn = m_timeline->playButton();
    m_tcLabel = m_timeline->timeLabel();
    l->addWidget(m_timeline);
    return c;
}

QWidget *MainWindow::buildPreviewWrap()
{
    auto *wrap = new QWidget;
    auto *l = new QVBoxLayout(wrap);
    l->setContentsMargins(Theme::M::PadSection, Theme::M::PadSection, Theme::M::PadSection,
                          Theme::M::PadSection);
    l->setSpacing(8);

    // ── pv-bar
    auto *bar = new QWidget;
    bar->setFixedHeight(22);
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(8);

    auto mkSeg = [&](const QStringList &items, int checkedIdx) {
        auto *box = new QFrame;
        box->setProperty("role", "segbox");
        box->setFixedHeight(22);
        auto *sl = new QHBoxLayout(box);
        sl->setContentsMargins(0, 0, 0, 0);
        sl->setSpacing(0);
        auto *g = new QButtonGroup(box);
        for (int i = 0; i < items.size(); ++i) {
            auto *b = mkBtn(items[i], "seg", 22);
            b->setCheckable(true);
            b->setChecked(i == checkedIdx);
            g->addButton(b, i);
            sl->addWidget(b);
        }
        return box;
    };

    auto *guideBox = mkSeg({QStringLiteral("演出"), QStringLiteral("安全框"),
                            QStringLiteral("三分构图"), QStringLiteral("网格")}, 0);
    auto *guideGroup = qobject_cast<QButtonGroup *>(guideBox->findChild<QButtonGroup *>());
    if (guideGroup) {
        connect(guideGroup, qOverload<int>(&QButtonGroup::idClicked), this,
                [this](int id) { m_preview->setGuideMode(id); });
    }
    bl->addWidget(guideBox);

    auto *holdBox = mkSeg({QStringLiteral("实时"), QStringLiteral("2帧定格"),
                           QStringLiteral("3帧定格")}, 0);
    auto *holdGroup = qobject_cast<QButtonGroup *>(holdBox->findChild<QButtonGroup *>());
    if (holdGroup) {
        connect(holdGroup, qOverload<int>(&QButtonGroup::idClicked), this,
                [this](int id) { m_preview->setHoldFrames(id == 0 ? 1 : id + 1); });
    }
    bl->addWidget(holdBox);
    bl->addStretch(1);

    auto *zoomInfo = new QLabel;
    zoomInfo->setTextFormat(Qt::RichText);
    zoomInfo->setFont(Theme::ui(Theme::F::Small));
    zoomInfo->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(Theme::Col::Tx3));
    zoomInfo->setText(QStringLiteral("缩放 <span style=\"color:%1\">适应</span>")
                          .arg(Theme::Col::Tx));
    bl->addWidget(zoomInfo);

    auto *pipe = new QLabel(QStringLiteral("|"));
    pipe->setFont(Theme::ui(Theme::F::Small));
    pipe->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(Theme::Col::Tx3));
    bl->addWidget(pipe);

    auto *shotInfo = new QLabel;
    shotInfo->setTextFormat(Qt::RichText);
    shotInfo->setFont(Theme::ui(Theme::F::Small));
    shotInfo->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(Theme::Col::Tx3));
    m_shotInfo = shotInfo;
    updateShotInfo();
    bl->addWidget(shotInfo);

    l->addWidget(bar);

    // ── 舞台
    auto *stage = new QFrame;
    stage->setProperty("role", "stage");
    auto *sl = new QVBoxLayout(stage);
    sl->setContentsMargins(0, 0, 0, 0);
    m_preview = new PreviewPane(stage);
    m_preview->onFrameRendered = [this] { updateEngineStats(); };
    sl->addWidget(m_preview);
    l->addWidget(stage, 1);

    return wrap;
}

QWidget *MainWindow::buildInspector()
{
    auto *ins = new QFrame;
    ins->setFixedWidth(Theme::M::RightW);
    ins->setStyleSheet(QStringLiteral("background:%1;border:none;border-left:1px solid %2;")
                           .arg(Theme::Col::Panel, Theme::Col::Line));

    auto *l = new QVBoxLayout(ins);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    {
        QWidget *head = mkPanelHead(QStringLiteral("检查器"));
        m_insHead = head->findChild<QLabel *>();
        l->addWidget(head);
    }
    {
        // 当前镜头区间（真值，随播放头移动）
        auto *row = new QWidget;
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(11, 5, 11, 5);
        rl->setSpacing(8);
        m_insRange = mkLabel(QStringLiteral("—"), Theme::F::Mono, Theme::Col::Tx);
        m_insRange->setFont(Theme::mono(Theme::F::Mono));
        rl->addWidget(m_insRange, 1);
        l->addWidget(row);
    }

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("background:transparent;"));

    auto *body = new QWidget;
    body->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);

    // ── 文字
    {
        Sect s = mkSection(QStringLiteral("文字"));
        s.l->addWidget(rowWithLabel(QStringLiteral("字体"),
                                    mkCombo({QStringLiteral("Noto Serif SC 900"),
                                             QStringLiteral("思源黑体 Heavy"),
                                             QStringLiteral("霞鹜文楷")})));

        // 这些滑块写回的是「当前镜头歌词所用的样式」，不是展示用的假值。
        auto wireStyle = [this](QSlider *sl, const char *field) {
            connect(sl, &QSlider::valueChanged, this, [this, field](int x) {
                if (m_insBusy)
                    return;
                const int i = m_project.shotIndexAt(m_t);
                if (i < 0)
                    return;
                const auto &sh = m_project.shots[i];
                if (sh.lyricRefs.isEmpty())
                    return;
                const sf::Lyric *ly = m_project.lyric(sh.lyricRefs[0]);
                if (!ly)
                    return;
                const QString sid = ly->style;
                const QString f = QString::fromLatin1(field);
                patchProject(QStringLiteral("样式 %1").arg(f), [sid, f, x](sf::Project &p) {
                    for (auto &st : p.styles) {
                        if (st.id != sid)
                            continue;
                        if (f == QLatin1String("size"))
                            st.size = x;
                        else if (f == QLatin1String("letterSpacing"))
                            st.letterSpacing = x / 100.0;
                        else if (f == QLatin1String("lineHeight"))
                            st.lineHeight = x / 100.0;
                        else if (f == QLatin1String("strokeWidth")) {
                            st.strokeWidth = x;
                            st.hasStroke = x > 0;
                        }
                    }
                });
            });
        };

        auto addSlider = [&](const QString &name, int lo, int hi, int val, int kind,
                             const char *field) {
            auto *sl = mkSlider(lo, hi, val);
            auto *v = mkVal();
            bindSlider(sl, v, kind);
            wireStyle(sl, field);
            s.l->addWidget(sliderRow(name, sl, v));
            return sl;
        };
        addSlider(QStringLiteral("字号"), 40, 200, 108, 0, "size");
        addSlider(QStringLiteral("字距"), -100, 100, -8, 3, "letterSpacing");
        addSlider(QStringLiteral("行高"), 80, 200, 112, 2, "lineHeight");
        addSlider(QStringLiteral("描边"), 0, 20, 5, 0, "strokeWidth");

        // 主色
        auto *swRow = new QWidget;
        auto *swl = new QHBoxLayout(swRow);
        swl->setContentsMargins(0, 0, 0, 0);
        swl->setSpacing(8);
        auto *n = mkLabel(QStringLiteral("主色"), Theme::F::Small, Theme::Col::Tx2);
        n->setFixedWidth(56);
        swl->addWidget(n);
        auto *swBox = new QWidget;
        auto *sb = new QHBoxLayout(swBox);
        sb->setContentsMargins(0, 0, 0, 0);
        sb->setSpacing(5);
        const QStringList swatches = {"#ffffff", "#ffe08a", "#ff6b9d", "#4dd0e1", "#8be08b"};
        auto *sg = new QButtonGroup(swBox);
        for (int i = 0; i < swatches.size(); ++i) {
            auto *sw = new QPushButton;
            sw->setCheckable(true);
            sw->setChecked(i == 0);
            sw->setFixedSize(22, 22);
            sw->setCursor(Qt::PointingHandCursor);
            sw->setFocusPolicy(Qt::NoFocus);
            sw->setStyleSheet(QStringLiteral(
                                  "QPushButton{background:%1;border:2px solid transparent;"
                                  "border-radius:4px;}QPushButton:checked{border-color:%2;}")
                                  .arg(swatches[i], Theme::Col::Tx));
            sg->addButton(sw, i);
            sb->addWidget(sw);
        }
        sb->addStretch(1);
        swl->addWidget(swBox, 1);
        s.l->addWidget(swRow);
        bl->addWidget(s.f);
    }

    // ── 入场 / 相机（真改工程）
    {
        Sect s = mkSection(QStringLiteral("入场 / 相机"));

        m_insFromInt = mkCombo({QStringLiteral("单字上浮 popUp"),
                                QStringLiteral("横排推入 trackIn"),
                                QStringLiteral("斜飞入 flyIn"),
                                QStringLiteral("无入场")});
        s.l->addWidget(rowWithLabel(QStringLiteral("入场"), m_insFromInt));
        connect(m_insFromInt, &QComboBox::currentTextChanged, this, [this](const QString &) {
            if (m_insBusy)
                return;
            const int i = m_project.shotIndexAt(m_t);
            if (i < 0)
                return;
            const QString id = m_project.shots[i].id;
            const int idx = m_insFromInt->currentIndex();
            patchProject(QStringLiteral("入场 %1").arg(id), [id, idx](sf::Project &p) {
                for (auto &sh : p.shots) {
                    if (sh.id != id)
                        continue;
                    if (idx == 3) {
                        sh.hasEntrance = false;
                        continue;
                    }
                    sh.hasEntrance = true;
                    sh.entrance.part = QStringLiteral("lyricEntrance");
                    sh.entrance.valid = true;
                    sh.entrance.params.insert(
                        QStringLiteral("style"),
                        idx == 0 ? QStringLiteral("popUp")
                        : idx == 1 ? QStringLiteral("trackIn")
                                   : QStringLiteral("flyIn"));
                    if (!sh.entrance.params.contains(QStringLiteral("dur")))
                        sh.entrance.params.insert(QStringLiteral("dur"), 0.32);
                }
            });
        });

        auto wireZoom = [this](QSlider *sl, bool isEnd) {
            connect(sl, &QSlider::valueChanged, this, [this, isEnd](int x) {
                if (m_insBusy)
                    return;
                const int i = m_project.shotIndexAt(m_t);
                if (i < 0)
                    return;
                const QString id = m_project.shots[i].id;
                patchProject(QStringLiteral("相机缩放 %1").arg(id), [id, isEnd, x](sf::Project &p) {
                    for (auto &sh : p.shots) {
                        if (sh.id != id)
                            continue;
                        if (!sh.hasCamera) {
                            sh.hasCamera = true;
                            sh.camera.part = QStringLiteral("camera");
                            sh.camera.valid = true;
                        }
                        const QJsonArray za =
                            sh.camera.params.value(QStringLiteral("zoom")).toArray();
                        double a = za.size() > 0 ? za.at(0).toDouble() : 1.0;
                        double b = za.size() > 1 ? za.at(1).toDouble() : 1.0;
                        if (isEnd)
                            b = x / 100.0;
                        else
                            a = x / 100.0;
                        QJsonArray out;
                        out.append(a);
                        out.append(b);
                        sh.camera.params.insert(QStringLiteral("zoom"), out);
                    }
                });
            });
        };

        m_insZoomA = mkSlider(100, 200, 100);
        auto *va = mkVal();
        bindSlider(m_insZoomA, va, 2);
        wireZoom(m_insZoomA, false);
        s.l->addWidget(sliderRow(QStringLiteral("缩放起"), m_insZoomA, va));

        m_insZoomB = mkSlider(100, 200, 100);
        auto *vb = mkVal();
        bindSlider(m_insZoomB, vb, 2);
        wireZoom(m_insZoomB, true);
        s.l->addWidget(sliderRow(QStringLiteral("缩放止"), m_insZoomB, vb));

        m_insPunch = mkSlider(0, 40, 0);
        auto *vp = mkVal();
        bindSlider(m_insPunch, vp, 2);
        connect(m_insPunch, &QSlider::valueChanged, this, [this](int x) {
            if (m_insBusy)
                return;
            const int i = m_project.shotIndexAt(m_t);
            if (i < 0)
                return;
            const QString id = m_project.shots[i].id;
            patchProject(QStringLiteral("节拍冲击 %1").arg(id), [id, x](sf::Project &p) {
                for (auto &sh : p.shots) {
                    if (sh.id != id)
                        continue;
                    if (!sh.hasCamera) {
                        sh.hasCamera = true;
                        sh.camera.part = QStringLiteral("camera");
                        sh.camera.valid = true;
                    }
                    sh.camera.params.insert(QStringLiteral("punch"), x / 100.0);
                }
            });
        });
        s.l->addWidget(sliderRow(QStringLiteral("节拍冲击"), m_insPunch, vp));

        // ── 运镜关键帧（camera.params.keys）：{t, zoom, x, y}，t 是绝对秒，
        //    x/y 是「要落到画面正中的那个点」（0..1）。≥2 个点时接管缩放与对焦。
        m_insKeyHint = mkLabel(QStringLiteral("运镜关键帧：无"), Theme::F::Micro, Theme::Col::Tx3);
        m_insKeyHint->setWordWrap(true);
        s.l->addWidget(m_insKeyHint);

        m_insKeySel = mkCombo({});
        s.l->addWidget(rowWithLabel(QStringLiteral("关键帧"), m_insKeySel));
        connect(m_insKeySel, &QComboBox::currentIndexChanged, this, [this](int idx) {
            if (m_insBusy || idx < 0)
                return;
            m_insKeyIndex = idx;
            refreshInspector(true); // 切点：把滑杆同步成这个点的值
        });

        auto addKeySlider = [&](const QString &name, int lo, int hi, const char *field) {
            auto *sl = mkSlider(lo, hi, lo);
            auto *v = mkVal();
            bindSlider(sl, v, 2);
            const QString f = QString::fromLatin1(field);
            connect(sl, &QSlider::sliderPressed, this,
                    [this, name] { beginParamDrag(QStringLiteral("运镜 %1").arg(name)); });
            connect(sl, &QSlider::sliderReleased, this, [this] { endParamDrag(); });
            connect(sl, &QSlider::valueChanged, this, [this, f](int x) {
                if (m_insBusy)
                    return;
                applyCameraKeyParam(f, x / 100.0);
            });
            s.l->addWidget(sliderRow(name, sl, v));
            return sl;
        };
        m_insKeyT = addKeySlider(QStringLiteral("时间 t"), 0, 6000, "t");
        m_insKeyZoom = addKeySlider(QStringLiteral("缩放"), 100, 200, "zoom");
        m_insKeyX = addKeySlider(QStringLiteral("中心 x"), 0, 100, "x");
        m_insKeyY = addKeySlider(QStringLiteral("中心 y"), 0, 100, "y");

        auto *keyBtnRow = new QWidget;
        auto *kbl = new QHBoxLayout(keyBtnRow);
        kbl->setContentsMargins(0, 0, 0, 0);
        kbl->setSpacing(6);
        m_insKeyAdd = mkBtn(QStringLiteral("＋ 加点"), "ghost", Theme::M::InputH);
        m_insKeyAdd->setFont(Theme::ui(Theme::F::Small));
        connect(m_insKeyAdd, &QPushButton::clicked, this, &MainWindow::addCameraKeyAtPlayhead);
        kbl->addWidget(m_insKeyAdd);
        m_insKeyDel = mkBtn(QStringLiteral("－ 删点"), "ghost", Theme::M::InputH);
        m_insKeyDel->setFont(Theme::ui(Theme::F::Small));
        connect(m_insKeyDel, &QPushButton::clicked, this, &MainWindow::removeCurrentCameraKey);
        kbl->addWidget(m_insKeyDel);
        kbl->addStretch(1);
        s.l->addWidget(keyBtnRow);

        bl->addWidget(s.f);
    }

    // ── 背景 / 效果（真改工程）
    {
        Sect s = mkSection(QStringLiteral("背景 / 效果"));

        m_insBgKind = mkCombo({QStringLiteral("solid 纯色"),   QStringLiteral("gradient 渐变"),
                               QStringLiteral("aurora 极光"),  QStringLiteral("bokeh 散景"),
                               QStringLiteral("grid 网格"),    QStringLiteral("rays 放射"),
                               QStringLiteral("stripes 斜条"), QStringLiteral("halftone 半调"),
                               QStringLiteral("scanlines 扫描线"), QStringLiteral("noise 噪点"),
                               QStringLiteral("paper 纸面")});
        s.l->addWidget(rowWithLabel(QStringLiteral("背景"), m_insBgKind));
        connect(m_insBgKind, &QComboBox::currentTextChanged, this, [this](const QString &text) {
            if (m_insBusy)
                return;
            const int i = m_project.shotIndexAt(m_t);
            if (i < 0)
                return;
            const QString id = m_project.shots[i].id;
            const QString kind = text.section(QLatin1Char(' '), 0, 0);
            patchProject(QStringLiteral("背景 → %1").arg(kind), [id, kind](sf::Project &p) {
                for (auto &sh : p.shots) {
                    if (sh.id != id)
                        continue;
                    backgroundParamsOf(sh)->insert(QStringLiteral("kind"), kind);
                }
            });
        });

        auto wireOverlay = [this](QSlider *sl, const QString &kind) {
            connect(sl, &QSlider::valueChanged, this, [this, kind](int x) {
                if (m_insBusy)
                    return;
                const int i = m_project.shotIndexAt(m_t);
                if (i < 0)
                    return;
                const QString id = m_project.shots[i].id;
                const double v = x / 100.0;
                patchProject(QStringLiteral("%1 %2").arg(kind, id), [id, kind, v](sf::Project &p) {
                    for (auto &sh : p.shots) {
                        if (sh.id != id)
                            continue;
                        bool found = false;
                        for (auto &ov : sh.overlay) {
                            if (ov.params.value(QStringLiteral("kind")).toString() != kind)
                                continue;
                            found = true;
                            if (kind == QLatin1String("grain"))
                                ov.params.insert(QStringLiteral("alpha"), v * 0.5);
                            else
                                ov.params.insert(QStringLiteral("color"),
                                                 QStringLiteral("rgba(0,0,0,%1)").arg(v * 0.8));
                        }
                        if (found)
                            continue;
                        sf::PartRef pr;
                        pr.part = QStringLiteral("overlay");
                        pr.valid = true;
                        pr.params.insert(QStringLiteral("kind"), kind);
                        if (kind == QLatin1String("grain")) {
                            pr.params.insert(QStringLiteral("alpha"), v * 0.5);
                            pr.params.insert(QStringLiteral("amount"), 420);
                        } else {
                            pr.params.insert(QStringLiteral("color"),
                                             QStringLiteral("rgba(0,0,0,%1)").arg(v * 0.8));
                            pr.params.insert(QStringLiteral("inner"), QStringLiteral("rgba(0,0,0,0)"));
                        }
                        sh.overlay.push_back(pr);
                    }
                });
            });
        };

        auto *grain = mkSlider(0, 100, 8);
        auto *gv = mkVal();
        bindSlider(grain, gv, 2);
        wireOverlay(grain, QStringLiteral("grain"));
        s.l->addWidget(sliderRow(QStringLiteral("颗粒"), grain, gv));

        auto *vig = mkSlider(0, 100, 30);
        auto *vv = mkVal();
        bindSlider(vig, vv, 2);
        wireOverlay(vig, QStringLiteral("vignette"));
        s.l->addWidget(sliderRow(QStringLiteral("暗角"), vig, vv));

        bl->addWidget(s.f);
    }

    // ── 立绘 / 场景图（background 混合数组，按 part 名定位）
    {
        Sect s = mkSection(QStringLiteral("立绘 / 场景图"));

        m_insPtName = mkLabel(QStringLiteral("—"), Theme::F::Small, Theme::Col::Tx2);
        s.l->addWidget(rowWithLabel(QStringLiteral("立绘"), m_insPtName));

        // 一个滑块 = 一条命令：按下记底，拖动中只改工程，松开才压撤销栈
        auto addPtSlider = [&](const QString &name, int lo, int hi, const char *key) {
            auto *sl = mkSlider(lo, hi, lo);
            auto *v = mkVal();
            bindSlider(sl, v, 2); // 值 = 滑块 / 100，两位小数
            const QString k = QString::fromLatin1(key);
            connect(sl, &QSlider::sliderPressed, this,
                    [this, name] { beginParamDrag(QStringLiteral("立绘 %1").arg(name)); });
            connect(sl, &QSlider::sliderReleased, this, [this] { endParamDrag(); });
            connect(sl, &QSlider::valueChanged, this, [this, k](int x) {
                if (m_insBusy)
                    return;
                applyPortraitParam(k, x / 100.0);
            });
            s.l->addWidget(sliderRow(name, sl, v));
            return sl;
        };

        m_insPtX = addPtSlider(QStringLiteral("水平"), 0, 100, "x");
        m_insPtY = addPtSlider(QStringLiteral("底边"), 50, 130, "y");
        m_insPtH = addPtSlider(QStringLiteral("身高"), 40, 140, "h");
        m_insPtAlpha = addPtSlider(QStringLiteral("透明"), 0, 100, "alpha");
        m_insPtGlow = addPtSlider(QStringLiteral("光晕"), 0, 100, "glow");

        m_insPtFlip = mkCombo({QStringLiteral("正常"), QStringLiteral("水平翻转")});
        s.l->addWidget(rowWithLabel(QStringLiteral("镜像"), m_insPtFlip));
        connect(m_insPtFlip, &QComboBox::currentIndexChanged, this, [this](int idx) {
            if (m_insBusy)
                return;
            applyPortraitParam(QStringLiteral("flip"), idx == 1 ? 1.0 : 0.0);
        });

        // at/outAt 是绝对秒，而且是「组级」的：同一个人跨 N 个镜头共用一组，
        // 改一处同步整组，切镜时人才不会跳。
        m_insPtAt = addPtSlider(QStringLiteral("进场 at"), 0, 100, "at");
        m_insPtOut = addPtSlider(QStringLiteral("退场 outAt"), 0, 100, "outAt");

        auto *ptHint =
            mkLabel(QStringLiteral("at/outAt 为绝对秒：同一立绘跨镜头共用，0 表示不进场/不退场"),
                    Theme::F::Micro, Theme::Col::Tx3);
        ptHint->setWordWrap(true);
        s.l->addWidget(ptHint);

        auto *delRow = new QWidget;
        auto *dl = new QHBoxLayout(delRow);
        dl->setContentsMargins(0, 0, 0, 0);
        dl->setSpacing(6);
        m_insPtDel = mkBtn(QStringLiteral("删除立绘"), "ghost", Theme::M::InputH);
        m_insPtDel->setFont(Theme::ui(Theme::F::Small));
        connect(m_insPtDel, &QPushButton::clicked, this,
                [this] { removeBackgroundPart(QStringLiteral("portrait")); });
        dl->addWidget(m_insPtDel);
        m_insPlateDel = mkBtn(QStringLiteral("删除场景图"), "ghost", Theme::M::InputH);
        m_insPlateDel->setFont(Theme::ui(Theme::F::Small));
        connect(m_insPlateDel, &QPushButton::clicked, this,
                [this] { removeBackgroundPart(QStringLiteral("image")); });
        dl->addWidget(m_insPlateDel);
        dl->addStretch(1);
        s.l->addWidget(delRow);

        m_insPlateName = mkLabel(QStringLiteral("—"), Theme::F::Micro, Theme::Col::Tx3);
        m_insPlateName->setWordWrap(true);
        s.l->addWidget(m_insPlateName);

        bl->addWidget(s.f);
    }

    // ── 视频 / 旋转（video 参数是 image 的超集；rotate 四类部件都认）
    {
        Sect s = mkSection(QStringLiteral("视频 / 图层微调"));

        m_insVideoName = mkLabel(QStringLiteral("—"), Theme::F::Small, Theme::Col::Tx2);
        s.l->addWidget(rowWithLabel(QStringLiteral("视频"), m_insVideoName));

        auto addVidSlider = [&](const QString &name, int lo, int hi, const char *key) {
            auto *sl = mkSlider(lo, hi, lo);
            auto *v = mkVal();
            bindSlider(sl, v, 2);
            const QString k = QString::fromLatin1(key);
            connect(sl, &QSlider::sliderPressed, this,
                    [this, name] { beginParamDrag(QStringLiteral("视频 %1").arg(name)); });
            connect(sl, &QSlider::sliderReleased, this, [this] { endParamDrag(); });
            connect(sl, &QSlider::valueChanged, this, [this, k](int x) {
                if (m_insBusy)
                    return;
                applyVideoParam(k, x / 100.0);
            });
            s.l->addWidget(sliderRow(name, sl, v));
            return sl;
        };
        m_insVidIn = addVidSlider(QStringLiteral("源起点"), 0, 6000, "in");
        m_insVidRate = addVidSlider(QStringLiteral("倍速"), 10, 200, "rate");

        m_insVidLoop = mkCombo({QStringLiteral("循环"), QStringLiteral("只播一次")});
        s.l->addWidget(rowWithLabel(QStringLiteral("循环"), m_insVidLoop));
        connect(m_insVidLoop, &QComboBox::currentIndexChanged, this, [this](int idx) {
            if (m_insBusy)
                return;
            applyVideoParam(QStringLiteral("loop"), idx == 0 ? 1.0 : 0.0);
        });

        // rotate：先选部件再转（一个镜头里可能同时有底图/立绘/视频/文字）
        m_insRotTarget = mkCombo({});
        s.l->addWidget(rowWithLabel(QStringLiteral("旋转目标"), m_insRotTarget));
        connect(m_insRotTarget, &QComboBox::currentIndexChanged, this, [this](int) {
            if (m_insBusy)
                return;
            refreshInspector(true); // 换目标：把滑杆同步成那个部件当前的旋转
        });
        m_insRotate = mkSlider(-45, 45, 0);
        auto *rv = mkVal();
        bindSlider(m_insRotate, rv, 0);
        connect(m_insRotate, &QSlider::sliderPressed, this,
                [this] { beginParamDrag(QStringLiteral("旋转")); });
        connect(m_insRotate, &QSlider::sliderReleased, this, [this] { endParamDrag(); });
        connect(m_insRotate, &QSlider::valueChanged, this, [this](int deg) {
            if (m_insBusy)
                return;
            applyLayerRotate(deg);
        });
        s.l->addWidget(sliderRow(QStringLiteral("旋转"), m_insRotate, rv));

        // tint + tintAmount：图片/立绘染色（色板给几个常用调，够点两下看出效果）
        m_insTintSel = mkCombo({QStringLiteral("不染色"), QStringLiteral("冷蓝"),
                                QStringLiteral("暖金"), QStringLiteral("品红"),
                                QStringLiteral("青绿")});
        s.l->addWidget(rowWithLabel(QStringLiteral("染色"), m_insTintSel));
        m_insTintAmt = mkSlider(0, 100, 0);
        auto *tv = mkVal();
        bindSlider(m_insTintAmt, tv, 2);
        s.l->addWidget(sliderRow(QStringLiteral("染色量"), m_insTintAmt, tv));
        connect(m_insTintSel, &QComboBox::currentIndexChanged, this, [this](int) {
            if (m_insBusy)
                return;
            applyLayerTint(tintSwatches().value(m_insTintSel->currentIndex()),
                           m_insTintAmt->value() / 100.0);
        });
        connect(m_insTintAmt, &QSlider::sliderPressed, this,
                [this] { beginParamDrag(QStringLiteral("染色量")); });
        connect(m_insTintAmt, &QSlider::sliderReleased, this, [this] { endParamDrag(); });
        connect(m_insTintAmt, &QSlider::valueChanged, this, [this](int) {
            if (m_insBusy)
                return;
            applyLayerTint(tintSwatches().value(m_insTintSel->currentIndex()),
                           m_insTintAmt->value() / 100.0);
        });

        bl->addWidget(s.f);
    }

    // ── 随机种子
    {
        Sect s = mkSection(QStringLiteral("随机种子"));
        auto *row = new QWidget;
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(5);
        m_insSeed = new QLineEdit(QStringLiteral("0"));
        m_insSeed->setFixedHeight(Theme::M::InputH);
        m_insSeed->setFont(Theme::mono(Theme::F::Mono));
        rl->addWidget(m_insSeed, 1);
        connect(m_insSeed, &QLineEdit::editingFinished, this, [this] {
            if (m_insBusy)
                return;
            const int i = m_project.shotIndexAt(m_t);
            if (i < 0)
                return;
            const QString id = m_project.shots[i].id;
            const int v = m_insSeed->text().toInt();
            patchProject(QStringLiteral("种子 %1").arg(id), [id, v](sf::Project &p) {
                for (auto &sh : p.shots)
                    if (sh.id == id)
                        sh.seed = v;
            });
        });
        auto *roll = mkBtn(QStringLiteral("↻"), "ghost", Theme::M::InputH);
        roll->setFixedWidth(28);
        rl->addWidget(roll);
        s.l->addWidget(row);

        auto *hint = mkLabel(QStringLiteral("种子固化进 .sf，保证任何机器上重渲结果一致"),
                             Theme::F::Micro, Theme::Col::Tx3);
        hint->setWordWrap(true);
        s.l->addWidget(hint);
        bl->addWidget(s.f);
    }

    // ── 工程（真实数据，随刷新更新）
    {
        Sect s = mkSection(QStringLiteral("工程"));
        m_insMeta = mkLabel(QStringLiteral("—"), Theme::F::Small, Theme::Col::Tx2);
        m_insMeta->setWordWrap(true);
        m_insMeta->setTextFormat(Qt::RichText);
        s.l->addWidget(m_insMeta);
        bl->addWidget(s.f);
    }

    bl->addStretch(1);
    scroll->setWidget(body);
    l->addWidget(scroll, 1);
    return ins;
}

// ── 无头自测：证明检查器控件真的写进了工程，而不是只动数字 ─────────────────
void MainWindow::runSelfTest()
{
    int failed = 0;
    auto check = [&failed](bool ok, const char *what) {
        fprintf(stdout, "%s  %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++failed;
    };

    if (m_project.shots.isEmpty()) {
        fprintf(stdout, "FAIL  工程为空\n");
        QCoreApplication::exit(1);
        return;
    }

    const QString id = m_project.shots.at(0).id;
    m_selShotId = id;
    refreshInspector(true);
    check(m_insHead && m_insHead->text().contains(id), "检查器绑定到选中镜头");

    // 1) 相机缩放止：setValue 必须触发 valueChanged → patchProject
    m_insZoomB->setValue(150);
    {
        const QJsonArray za =
            m_project.shots.at(0).camera.params.value(QStringLiteral("zoom")).toArray();
        const bool ok = za.size() > 1 && qAbs(za.at(1).toDouble() - 1.5) < 1e-6;
        check(ok, "缩放止 150 → camera.params.zoom[1] == 1.5");
    }

    // 2) 撤销必须把它退回去
    undoEdit();
    {
        bool ok = true;
        if (m_project.shots.at(0).hasCamera) {
            const QJsonArray za =
                m_project.shots.at(0).camera.params.value(QStringLiteral("zoom")).toArray();
            ok = za.size() < 2 || qAbs(za.at(1).toDouble() - 1.5) > 1e-6;
        }
        check(ok, "撤销后缩放止不是 1.5");
    }

    // 3) 种子真正落到 shot.seed
    m_insSeed->setText(QStringLiteral("424242"));
    QMetaObject::invokeMethod(m_insSeed, "editingFinished");
    check(m_project.shots.at(0).seed == 424242, "种子输入 → shot.seed == 424242");

    // 4) 背景类型真正落到 params.kind（background 是混合数组，按 part 名取，别按下标）
    m_insBgKind->setCurrentIndex(m_insBgKind->findText(QStringLiteral("paper"), Qt::MatchStartsWith));
    check(backgroundKindOf(m_project.shots.at(0)) == QLatin1String("paper"),
          "背景下拉 → background 部件的 params.kind == paper");

    // 5) 渲染层确实吃到了改动（换背景后同一时刻像素必须不同）
    {
        QImage a = m_renderer.render(m_t, 320, 180);
        m_insBgKind->setCurrentIndex(
            m_insBgKind->findText(QStringLiteral("grid"), Qt::MatchStartsWith));
        QImage b = m_renderer.render(m_t, 320, 180);
        check(!a.isNull() && !b.isNull() && a != b, "换背景后渲染结果不同");
    }

    // 6) 插入素材：background 多一个 image 部件，且渲染结果真的变了
    {
        // 素材按「CWD → exe 目录 → 各往上 4 层」找：在 build 目录里跑 selftest 也能拿到
        const QString plate = findRepoAsset(QStringLiteral("assets/bg_rain_window.png"));
        if (!plate.isEmpty()) {
            const int before = m_project.shots.at(0).background.size();
            const QImage a = m_renderer.render(m_t, 320, 180);
            insertImagePlate(plate);
            const int after = m_project.shots.at(0).background.size();
            const QImage b = m_renderer.render(m_t, 320, 180);
            check(after == before + 1, "插入素材 → background 多一项");
            check(!a.isNull() && !b.isNull() && a != b, "插入素材后渲染结果不同");

            // 删除场景图也要连数组位置一起撤销（顺序即绘制顺序）
            if (after == before + 1) {
                m_selShotId = m_project.shots.at(0).id;
                refreshInspector(true);
                QStringList seq0;
                for (const auto &pr : m_project.shots.at(0).background)
                    seq0 << pr.part;
                removeBackgroundPart(QStringLiteral("image"));
                check(int(m_project.shots.at(0).background.size()) == after - 1,
                      "删除场景图 → background 少一项");
                undoEdit();
                QStringList seq1;
                for (const auto &pr : m_project.shots.at(0).background)
                    seq1 << pr.part;
                check(seq1 == seq0, "撤销删除场景图 → 回到原数组位置");
            }
        } else {
            fprintf(stdout, "SKIP  插入素材（找不到 %s）\n", qUtf8Printable(plate));
        }
    }

    // 7) 立绘：面板绑定 → 参数写回 → 一次拖动一条撤销 → at 组级同步 → 删除可撤销
    {
        int pi = -1;
        for (int i = 0; i < m_project.shots.size(); ++i) {
            if (findBackgroundPart(m_project.shots.at(i), QStringLiteral("portrait"))) {
                pi = i;
                break;
            }
        }
        if (pi < 0) {
            fprintf(stdout, "SKIP  立绘（当前工程没有 portrait 部件）\n");
        } else {
            m_selShotId = m_project.shots.at(pi).id;
            m_t = m_project.shots.at(pi).start + 0.01;
            refreshInspector(true);
            const QString path = m_insPortraitPath;
            check(m_insPtName && !path.isEmpty() && m_insPtX->isEnabled(),
                  "检查器绑定到镜头里的立绘（按 path 认人）");

            sf::PartRef *pt =
                findBackgroundPart(m_project.shots[pi], QStringLiteral("portrait"), path);
            check(pt != nullptr, "立绘部件按 part+path 定位（不是 background[0]）");
            const double x0 = pt ? pt->params.value(QStringLiteral("x")).toDouble() : 0.0;

            // 一次拖动 = 一条命令：press → 多步 valueChanged → release
            const int depth0 = int(m_undo.size());
            m_insPtX->sliderPressed();
            m_insPtX->setValue(50);
            m_insPtX->setValue(44);
            m_insPtX->setValue(40);
            m_insPtX->sliderReleased();
            pt = findBackgroundPart(m_project.shots[pi], QStringLiteral("portrait"), path);
            check(int(m_undo.size()) == depth0 + 1, "一次拖动只压一条撤销项");
            check(pt && qAbs(pt->params.value(QStringLiteral("x")).toDouble() - 0.40) < 1e-6,
                  "立绘「水平」→ params.x == 0.40");
            undoEdit();
            pt = findBackgroundPart(m_project.shots[pi], QStringLiteral("portrait"), path);
            check(pt && qAbs(pt->params.value(QStringLiteral("x")).toDouble() - x0) < 1e-6,
                  "撤销后立绘 x 回到原值");

            // 镜像开关
            const int flip0 = m_insPtFlip->currentIndex();
            m_insPtFlip->setCurrentIndex(flip0 == 1 ? 0 : 1);
            pt = findBackgroundPart(m_project.shots[pi], QStringLiteral("portrait"), path);
            check(pt && pt->params.value(QStringLiteral("flip")).toBool() == (flip0 != 1),
                  "镜像开关 → params.flip");
            undoEdit();

            // at 是组级的：同一个人跨 N 个镜头共用一组进场时间
            const QVector<int> grp = portraitGroup(m_project, pi, path);
            if (grp.size() < 2) {
                fprintf(stdout, "SKIP  at 组级同步（这个立绘只在一个镜头里）\n");
            } else {
                QVector<double> before;
                for (int i : grp) {
                    before.push_back(findBackgroundPart(m_project.shots.at(i),
                                                        QStringLiteral("portrait"), path)
                                         ->params.value(QStringLiteral("at"))
                                         .toDouble());
                }
                const int at0 = m_insPtAt->value();
                const int target = (at0 + 7 <= m_insPtAt->maximum()) ? at0 + 7 : at0 - 7;
                m_insPtAt->sliderPressed();
                m_insPtAt->setValue(target);
                m_insPtAt->sliderReleased();

                double atFirst = 0;
                bool synced = true;
                for (int k = 0; k < grp.size(); ++k) {
                    const double v = findBackgroundPart(m_project.shots.at(grp.at(k)),
                                                        QStringLiteral("portrait"), path)
                                         ->params.value(QStringLiteral("at"))
                                         .toDouble();
                    if (k == 0)
                        atFirst = v;
                    if (qAbs(v - atFirst) > 1e-6)
                        synced = false;
                }
                check(synced && qAbs(atFirst - target / 100.0) < 0.02,
                      "改 at 同步整组立绘，且落到绝对秒");

                undoEdit();
                bool back = true;
                for (int k = 0; k < grp.size(); ++k) {
                    const double v = findBackgroundPart(m_project.shots.at(grp.at(k)),
                                                        QStringLiteral("portrait"), path)
                                         ->params.value(QStringLiteral("at"))
                                         .toDouble();
                    if (qAbs(v - before.at(k)) > 1e-6)
                        back = false;
                }
                check(back, "一次撤销把整组 at 一起回滚");
            }

            // 删除立绘：数组位置/顺序必须能完整撤销
            QStringList parts0;
            for (const auto &pr : m_project.shots.at(pi).background)
                parts0 << pr.part;
            removeBackgroundPart(QStringLiteral("portrait"));
            QStringList parts1;
            for (const auto &pr : m_project.shots.at(pi).background)
                parts1 << pr.part;
            check(parts1.size() == parts0.size() - 1, "删除立绘 → background 少一项");
            undoEdit();
            QStringList parts2;
            for (const auto &pr : m_project.shots.at(pi).background)
                parts2 << pr.part;
            check(parts2 == parts0, "撤销删除 → 立绘回到原数组位置（顺序即绘制顺序）");

            // 有立绘/底图挡在前面时，背景下拉仍要改到 background 部件
            m_insBgKind->setCurrentIndex(
                m_insBgKind->findText(QStringLiteral("grid"), Qt::MatchStartsWith));
            check(backgroundKindOf(m_project.shots.at(pi)) == QLatin1String("grid"),
                  "立绘排在前面时背景下拉仍落到 background 部件");
        }
    }

    // 8) 部件库预设（sf::partPresets）：每一条都能真的插进它声明的层
    {
        const QVector<sf::PartPreset> ps = sf::partPresets();
        QStringList groups;
        for (const sf::PartPreset &p : ps) {
            if (!groups.contains(p.group))
                groups << p.group;
        }
        std::printf("  预设 %d 条，分组：%s\n", int(ps.size()),
                    groups.join(QStringLiteral(" / ")).toUtf8().constData());
        check(ps.size() >= 20, "部件库预设表非空且数量充足（sf::partPresets）");
        check(groups.size() >= 4, "预设覆盖 背景 / 装饰 / 覆盖 / 文字 四组");

        sf::PartPreset pick;
        for (const sf::PartPreset &p : ps) {
            if (p.layer == QLatin1String("decor")) {
                pick = p;
                break;
            }
        }
        if (pick.label.isEmpty()) {
            fprintf(stdout, "SKIP  预设插入（没有 decor 层预设）\n");
        } else {
            m_selShotId = m_project.shots.at(0).id;
            const int before = m_project.shots.at(0).decor.size();
            insertPartPreset(pick.label);
            const int after = m_project.shots.at(0).decor.size();
            check(after == before + 1, "双击预设 → 声明的层（decor）多一项");
            check(!m_project.shots.at(0).decor.isEmpty()
                      && m_project.shots.at(0).decor.last().part == pick.part,
                  "预设的 part 原样落进对应层");
            undoEdit();
            check(int(m_project.shots.at(0).decor.size()) == before, "撤销后装饰层回到原状");
        }
    }

    // 9) 视频：按扩展名插 video 部件（参数是 image 的超集）+ 面板 in/rate/loop
    {
        m_selShotId = m_project.shots.at(0).id;
        refreshInspector(true);
        const QString real = findRepoAsset(QStringLiteral("output/_t1.mp4"));
        const QString clip = real.isEmpty() ? QStringLiteral("/__no_such__/clip.mp4") : real;

        const int before = m_project.shots.at(0).background.size();
        insertImagePlate(clip);
        const sf::PartRef *vd = findVideoPart(m_project.shots.at(0));
        check(vd != nullptr && int(m_project.shots.at(0).background.size()) == before + 1,
              "插 .mp4 → background 多一个 video 部件");
        check(vd
                  && vd->params.value(QStringLiteral("fit")).toString() == QLatin1String("cover")
                  && qAbs(vd->params.value(QStringLiteral("alpha")).toDouble() - 1.0) < 1e-9,
              "视频默认 fit=cover / alpha=1（铺满画幅不透明）");
        check(vd && vd->params.value(QStringLiteral("loop")).toBool(false),
              "视频默认 loop=true");

        refreshInspector(true);
        check(m_insVidRate && m_insVidRate->isEnabled(), "检查器绑定到视频部件");
        m_insVidRate->sliderPressed();
        m_insVidRate->setValue(150);
        m_insVidRate->sliderReleased();
        vd = findVideoPart(m_project.shots.at(0));
        check(vd && qAbs(vd->params.value(QStringLiteral("rate")).toDouble() - 1.5) < 1e-6,
              "倍速滑杆 → params.rate == 1.5");
        undoEdit();
        vd = findVideoPart(m_project.shots.at(0));
        check(vd && qAbs(vd->params.value(QStringLiteral("rate")).toDouble() - 1.0) < 1e-6,
              "撤销后倍速回到 1.0");
    }

    // 10) 旋转：任意可旋转部件都能写 rotate，0 度即删键
    {
        refreshInspector(true);
        check(m_insRotTarget && m_insRotTarget->count() > 0, "旋转目标下拉列出可旋转部件");

        const auto readRotate = [this]() -> double {
            const QStringList sl =
                m_insRotTarget->currentData().toString().split(QLatin1Char(':'));
            if (sl.size() != 2)
                return 0;
            const QVector<sf::PartRef> *v = layerParts(m_project.shots.at(0), sl.at(0).toInt());
            const int k = sl.at(1).toInt();
            if (!v || k < 0 || k >= v->size())
                return 0;
            return v->at(k).params.value(QStringLiteral("rotate")).toDouble(0);
        };

        m_insRotate->sliderPressed();
        m_insRotate->setValue(-12);
        m_insRotate->sliderReleased();
        check(qAbs(readRotate() + 12.0) < 1e-6, "旋转滑杆 → 目标部件 params.rotate = -12");
        undoEdit();
        check(qAbs(readRotate()) < 1e-6, "撤销后 rotate 回到原值");
    }

    // 11) 运镜关键帧（camera.params.keys）+ 场景图必须压在最底（background 渐变是文字衬底）
    {
        m_selShotId = m_project.shots.at(0).id;
        m_t = m_project.shots.at(0).start + 0.5;
        refreshInspector(true);

        const QString plate = findRepoAsset(QStringLiteral("assets/bg_city.png"));
        if (!plate.isEmpty()) {
            insertImagePlate(plate);
            const auto &bg = m_project.shots.at(0).background;
            const bool imgFirst =
                !bg.isEmpty() && bg.first().part == QLatin1String("image")
                && bg.first().params.value(QStringLiteral("path")).toString().contains(
                    QStringLiteral("bg_city"));
            check(imgFirst, "插入场景图 → 排在 background 数组最前（压在文字衬底之下）");
            undoEdit();
        } else {
            fprintf(stdout, "SKIP  场景图层序（找不到 assets/bg_city.png）\n");
        }

        // 染色：tint + tintAmount 落在图片/立绘上，撤销要一起消失。
        // 目标镜头不一定有图片（整片渐变也很正常），先全工程找，找不到就 SKIP 而不是 FAIL。
        int tintShot = -1;
        for (int si = 0; si < m_project.shots.size() && tintShot < 0; ++si) {
            for (const sf::PartRef &pr : m_project.shots.at(si).background) {
                if (pr.part == QLatin1String("image") || pr.part == QLatin1String("portrait")) {
                    tintShot = si;
                    break;
                }
            }
        }
        if (tintShot < 0) {
            fprintf(stdout, "SKIP  染色（工程里没有图片/立绘）\n");
        } else {
            m_selShotId = m_project.shots.at(tintShot).id;
            m_t = m_project.shots.at(tintShot).start + 0.01;
            refreshInspector(true);
            int imgItem = -1;
            for (int k = 0; k < m_insRotTarget->count(); ++k) {
                const QStringList sp =
                    m_insRotTarget->itemData(k).toString().split(QLatin1Char(':'));
                if (sp.size() != 2)
                    continue;
                const QVector<sf::PartRef> *v =
                    layerParts(m_project.shots.at(tintShot), sp.at(0).toInt());
                const int vi = sp.at(1).toInt();
                if (v && vi >= 0 && vi < v->size()) {
                    const QString part = v->at(vi).part;
                    if (part == QLatin1String("image") || part == QLatin1String("portrait")) {
                        imgItem = k;
                        break;
                    }
                }
            }
            check(imgItem >= 0, "旋转/染色目标里能找到图片或立绘");
            if (imgItem >= 0) {
                m_insRotTarget->setCurrentIndex(imgItem);
                m_insTintSel->setCurrentIndex(1); // 冷蓝（染色量还是 0，应当不产生改动）
                m_insTintAmt->sliderPressed();
                m_insTintAmt->setValue(60);
                m_insTintAmt->sliderReleased();
                const QStringList sp =
                    m_insRotTarget->currentData().toString().split(QLatin1Char(':'));
                const QVector<sf::PartRef> *v =
                    layerParts(m_project.shots.at(tintShot), sp.at(0).toInt());
                const int vi = sp.at(1).toInt();
                const QJsonObject prm =
                    (v && vi >= 0 && vi < v->size()) ? v->at(vi).params : QJsonObject();
                check(prm.value(QStringLiteral("tint")).toString() == QLatin1String("#9fd8ff")
                          && qAbs(prm.value(QStringLiteral("tintAmount")).toDouble(0) - 0.6)
                                 < 1e-6,
                      "染色滑杆 → params.tint / tintAmount");
                undoEdit();
                const QVector<sf::PartRef> *v2 =
                    layerParts(m_project.shots.at(tintShot), sp.at(0).toInt());
                check(v2 && vi >= 0 && vi < v2->size()
                          && !v2->at(vi).params.contains(QStringLiteral("tint")),
                      "撤销后 tint / tintAmount 一起消失");
            }
        }

        // 后面的关键帧断言按 shot 0 读数，把选择切回去
        m_selShotId = m_project.shots.at(0).id;
        m_t = m_project.shots.at(0).start + 0.5;
        refreshInspector(true);

        addCameraKeyAtPlayhead();
        const int n1 =
            int(m_project.shots.at(0).camera.params.value(QStringLiteral("keys")).toArray().size());
        m_t += 1.0;
        refreshInspector(true);
        addCameraKeyAtPlayhead();
        const QJsonArray ks =
            m_project.shots.at(0).camera.params.value(QStringLiteral("keys")).toArray();
        check(n1 >= 1 && int(ks.size()) == n1 + 1, "加运镜关键帧 → camera.params.keys 多一个点");

        refreshInspector(true); // addCameraKey 已把选中项设成刚加的那个点
        if (m_insKeyZoom && m_insKeyZoom->isEnabled()) {
            m_insKeyZoom->sliderPressed();
            m_insKeyZoom->setValue(130);
            m_insKeyZoom->sliderReleased();
            const QJsonArray k2 =
                m_project.shots.at(0).camera.params.value(QStringLiteral("keys")).toArray();
            check(qAbs(k2.last().toObject().value(QStringLiteral("zoom")).toDouble(0) - 1.3) < 1e-6,
                  "关键帧「缩放」滑杆 → keys[last].zoom == 1.3");
            undoEdit();
            const QJsonArray k3 =
                m_project.shots.at(0).camera.params.value(QStringLiteral("keys")).toArray();
            check(qAbs(k3.last().toObject().value(QStringLiteral("zoom")).toDouble(0) - 1.0) < 1e-6,
                  "撤销后关键帧 zoom 回到 1.0");
        } else {
            fprintf(stdout, "SKIP  关键帧滑杆（面板未绑定到关键帧）\n");
        }
    }

    // 12) BPM 检测：内核自相关算完写回 bpm/offset（走同一条无对话框路径）
    {
        const int undoBefore = int(m_undo.size());
        QString berr;
        if (applyDetectedBpm(&berr)) {
            std::printf("  BPM 检测结果：%.2f BPM / 首拍 %.3fs\n", m_project.bpm, m_project.offset);
            check(m_project.bpm > 40 && m_project.bpm < 220, "BPM 检测 → 结果落在合理区间");
            check(int(m_undo.size()) == undoBefore + 1, "BPM 写回只压一条撤销命令");
            undoEdit();
        } else {
            fprintf(stdout, "SKIP  BPM 检测（%s）\n", qUtf8Printable(berr));
        }
    }

    // 13) 歌词 .lrc 往返：导出 → 重新导入，行数/文本一致、行起点误差 ≤5ms
    {
        if (m_project.lyrics.isEmpty()) {
            fprintf(stdout, "SKIP  LRC 往返（工程没有歌词）\n");
        } else {
            const QString tmp = QDir::tempPath() + QStringLiteral("/sf_selftest.lrc");
            QString lerr;
            const bool exported = sf::exportLrc(m_project, tmp, &lerr);
            sf::Project probe;
            const bool imported = exported && sf::importLrc(tmp, probe, &lerr);
            check(imported && probe.lyrics.size() == m_project.lyrics.size(),
                  "LRC 往返：行数一致");

            const int n = std::min(int(m_project.lyrics.size()), int(probe.lyrics.size()));
            double maxErr = 0;
            bool textSame = n > 0;
            for (int i = 0; i < n; ++i) {
                const sf::Lyric &a = m_project.lyrics.at(i);
                const sf::Lyric &b = probe.lyrics.at(i);
                maxErr = std::max(maxErr, std::abs(a.start - b.start));
                if (a.text != b.text)
                    textSame = false;
            }
            check(textSame, "LRC 往返：文本逐行一致");
            std::printf("  LRC 行起点最大误差 %.1f ms（%d 行）\n", maxErr * 1000.0, n);
            // LRC 只有厘秒精度（mm:ss.xx），量化误差上界就是 5ms，再留一点浮点余量
            check(maxErr <= 0.0055, "LRC 往返：行起点误差 ≤5ms（厘秒量化上界）");

            // 增强格式 <mm:ss.xx> 会被拆成逐字，符号起点也要对得上（比较公共前缀）
            int sylCmp = 0;
            double sylErr = 0;
            for (int i = 0; i < n; ++i) {
                const sf::Lyric &a = m_project.lyrics.at(i);
                const sf::Lyric &b = probe.lyrics.at(i);
                const int sn = std::min(int(a.syllables.size()), int(b.syllables.size()));
                for (int k = 0; k < sn; ++k) {
                    sylErr = std::max(sylErr, std::abs(a.syllables.at(k).start
                                                       - b.syllables.at(k).start));
                    ++sylCmp;
                }
            }
            std::printf("  LRC 逐字起点最大误差 %.1f ms（%d 个字）\n", sylErr * 1000.0, sylCmp);
            check(sylCmp == 0 || sylErr <= 0.0055, "LRC 往返：逐字起点误差 ≤5ms（厘秒量化上界）");
            QFile::remove(tmp);
        }
    }

    // 14) 存盘往返：改过的工程写盘后重新读回来，改动必须还在
    {
        const QString tmp = QDir::tempPath() + QStringLiteral("/sf_selftest.sf");
        QString err;
        const bool saved = m_project.save(tmp, &err);
        sf::Project re;
        const bool reloaded = saved && re.load(tmp, &err);

        bool same = reloaded && re.shots.size() == m_project.shots.size();
        if (same && !re.shots.isEmpty()) {
            const auto &a = m_project.shots.at(0);
            const auto &b = re.shots.at(0);
            same = a.id == b.id && qAbs(a.start - b.start) < 1e-9
                   && qAbs(a.end - b.end) < 1e-9 && a.seed == b.seed;
            // 同样按 part 名比，不猜下标：底图/立绘可能排在 gradient 前面
            same = same && backgroundKindOf(a) == backgroundKindOf(b)
                   && a.background.size() == b.background.size();
        }
        check(same, "save → load 往返：镜头数 / 区间 / 种子 / 背景类型一致");

        // 撤销栈里那些改动也应随工程一起落盘（这里只验数量级：歌词与样式没丢）
        check(reloaded && re.lyrics.size() == m_project.lyrics.size()
                  && re.styles.size() == m_project.styles.size(),
              "往返后歌词与样式数量一致");

        QFile::remove(tmp);
    }

    fprintf(stdout, "\n%s (%d failed)\n", failed == 0 ? "RESULT: PASS" : "RESULT: FAIL", failed);
    QCoreApplication::exit(failed == 0 ? 0 : 1);
}

// ── 菜单动作 ───────────────────────────────────────────────────────────────

int MainWindow::currentShotIndex() const
{
    if (m_project.shots.isEmpty())
        return -1;
    if (!m_selShotId.isEmpty()) {
        for (int i = 0; i < m_project.shots.size(); ++i)
            if (m_project.shots.at(i).id == m_selShotId)
                return i;
    }
    return m_project.shotIndexAt(m_t);
}

void MainWindow::newProject()
{
    togglePlay(false);

    sf::Project p;
    p.title = QStringLiteral("未命名");
    p.canvas.width = 1280;
    p.canvas.height = 720;
    p.canvas.fps = 30;
    p.canvas.background = QColor(QStringLiteral("#050508"));
    p.bpm = 120;

    sf::StyleDef st;
    st.id = QStringLiteral("main");
    p.styles.push_back(st);

    // 给一个空镜头：没有镜头的时间轴什么也干不了，连"删除"都没得删
    sf::Shot s;
    s.id = QStringLiteral("S001");
    s.start = 0;
    s.end = 4;
    s.track = 0;
    s.seed = 1;
    sf::PartRef bg;
    bg.part = QStringLiteral("background");
    bg.valid = true;
    bg.params.insert(QStringLiteral("kind"), QStringLiteral("solid"));
    s.background.push_back(bg);
    p.shots.push_back(s);

    m_project = p;
    m_projectPath.clear();
    m_selShotId.clear();
    m_insShotId.clear();
    m_t = 0;

    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    m_undo.clear();
    m_redo.clear();
    if (m_preview) {
        m_preview->setEngine(&m_project, &m_renderer);
        m_preview->invalidateFrame();
    }
    if (m_timeline) {
        m_timeline->setProject(&m_project);
        m_timeline->reloadProjectView(); // 连开两个新工程时路径都是空，得显式重置
        m_timeline->setTime(0);
    }
    refreshTimeCode();
    refreshInspector(true);
    updateUndoActions();
    updateShotInfo();
    markDirty();
}

void MainWindow::markDirty()
{
    m_dirty = true;
    const QString base = m_projectPath.isEmpty() ? m_project.title : m_project.title;
    setWindowTitle(QStringLiteral("%1%2 — ShizukuFlow").arg(base, QStringLiteral("*")));
    if (m_fileChip)
        m_fileChip->setText(QStringLiteral("%1%2").arg(
            m_projectPath.isEmpty() ? QStringLiteral("未保存") : QFileInfo(m_projectPath).fileName(),
            QStringLiteral(" *")));
}

void MainWindow::saveProject()
{
    if (m_projectPath.isEmpty()) {
        saveProjectAs();
        return;
    }
    QString err;
    if (!m_project.save(m_projectPath, &err)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             err.isEmpty() ? QStringLiteral("未知错误") : err);
        return;
    }
    m_dirty = false;
    setWindowTitle(QStringLiteral("%1 — ShizukuFlow").arg(m_project.title));
    if (m_fileChip)
        m_fileChip->setText(QFileInfo(m_projectPath).fileName());
}

void MainWindow::saveProjectAs()
{
    const QString suggest =
        m_projectPath.isEmpty() ? (m_project.title + QStringLiteral(".sf")) : m_projectPath;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("另存为"), suggest,
                                                QStringLiteral("ShizukuFlow 工程 (*.sf)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".sf"), Qt::CaseInsensitive))
        path += QStringLiteral(".sf");

    QString err;
    if (!m_project.save(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             err.isEmpty() ? QStringLiteral("未知错误") : err);
        return;
    }
    m_projectPath = path;
    m_dirty = false;
    setWindowTitle(QStringLiteral("%1 — ShizukuFlow").arg(m_project.title));
    if (m_fileChip)
        m_fileChip->setText(QFileInfo(path).fileName());
}

void MainWindow::deleteSelectedShot()
{
    // 窗口级 Delete 快捷键会先于时间轴的 keyPressEvent 命中，
    // 所以「删部件还是删镜头」必须先问时间轴：有部件选择就删部件（一条撤销）。
    if (m_timeline && m_timeline->deleteSelection())
        return;
    const int i = currentShotIndex();
    if (i < 0)
        return;
    if (m_project.shots.size() <= 1) {
        QMessageBox::information(this, QStringLiteral("删除镜头"),
                                 QStringLiteral("至少保留一个镜头。"));
        return;
    }
    sf::EditCmd c;
    c.kind = sf::EditKind::ShotRemove;
    c.id = m_project.shots.at(i).id;
    applyEditCmd(c);
    m_selShotId.clear();
}

void MainWindow::rerollShot()
{
    const int i = currentShotIndex();
    if (i < 0)
        return;
    const QString id = m_project.shots.at(i).id;
    const int seed = QRandomGenerator::global()->bounded(99999);
    // 不能是 static：lambda 按值捕获 static 变量会触发
    // "capture of variable with non-automatic storage duration"
    const QStringList kinds = {
        QStringLiteral("solid"),   QStringLiteral("gradient"), QStringLiteral("aurora"),
        QStringLiteral("bokeh"),   QStringLiteral("grid"),     QStringLiteral("rays"),
        QStringLiteral("stripes"), QStringLiteral("halftone"), QStringLiteral("scanlines"),
        QStringLiteral("noise"),   QStringLiteral("paper")};
    patchProject(QStringLiteral("换一个 %1").arg(id), [id, seed, kinds](sf::Project &p) {
        for (auto &s : p.shots) {
            if (s.id != id)
                continue;
            s.seed = seed;
            QJsonObject *bg = backgroundParamsOf(s);
            bg->insert(QStringLiteral("kind"), kinds.at(seed % kinds.size()));
            bg->insert(QStringLiteral("seed"), seed);
        }
    });
}

void MainWindow::insertImagePlate(const QString &absPath)
{
    const int i = currentShotIndex();
    if (i < 0) {
        QMessageBox::information(this, QStringLiteral("插入图片"),
                                 QStringLiteral("先把播放头移到一个镜头上。"));
        return;
    }
    const QString id = m_project.shots.at(i).id;

    // 存相对 .sf 目录的路径：工程连同 assets/ 一起搬走仍然找得到
    const QString baseDir = m_projectPath.isEmpty() ? QDir::currentPath()
                                                    : QFileInfo(m_projectPath).absolutePath();
    const QString rel = QDir(baseDir).relativeFilePath(absPath);
    const QString fileName = QFileInfo(absPath).fileName();
    // 三类素材走三种部件：
    //   .mp4/.mov/.webm → video（渲染层抽帧缓存，参数是 image 的超集）
    //   chara_*          → portrait（烤好的立绘：站画框底、靠右、自带呼吸）
    //   其它             → image（铺满画幅的照片底图）
    const QString suffix = QFileInfo(absPath).suffix().toLower();
    const bool asVideo =
        suffix == QLatin1String("mp4") || suffix == QLatin1String("mov")
        || suffix == QLatin1String("webm");
    const bool asPortrait = !asVideo && fileName.startsWith(QLatin1String("chara_"));
    const QString partName = asVideo ? QStringLiteral("video")
                                     : (asPortrait ? QStringLiteral("portrait")
                                                   : QStringLiteral("image"));

    // 同一张图重复插会叠成两层：画面看不出差别，却白吃一遍缩放绘制
    for (const auto &pr : m_project.shots.at(i).background) {
        if (pr.part == partName
            && pr.params.value(QStringLiteral("path")).toString() == rel) {
            QMessageBox::information(this, QStringLiteral("插入图片"),
                                     QStringLiteral("这个镜头已经用过这张图了。"));
            return;
        }
    }

    patchProject(QStringLiteral("插入%1 %2")
                     .arg(asVideo ? QStringLiteral("视频")
                                  : (asPortrait ? QStringLiteral("立绘") : QStringLiteral("图片")),
                          fileName),
                 [id, rel, asPortrait, asVideo, fileName](sf::Project &p) {
                     for (auto &s : p.shots) {
                         if (s.id != id)
                             continue;
                         sf::PartRef pr;
                         pr.valid = true;
                         pr.params.insert(QStringLiteral("path"), rel);
                         if (asVideo) {
                             // 视频 = image 的超集：默认铺满画幅、cover、不透明、循环、原速
                             pr.part = QStringLiteral("video");
                             pr.params.insert(QStringLiteral("fit"), QStringLiteral("cover"));
                             pr.params.insert(QStringLiteral("x"), 0.0);
                             pr.params.insert(QStringLiteral("y"), 0.0);
                             pr.params.insert(QStringLiteral("w"), 1.0);
                             pr.params.insert(QStringLiteral("h"), 1.0);
                             pr.params.insert(QStringLiteral("alpha"), 1.0);
                             pr.params.insert(QStringLiteral("in"), 0.0);
                             pr.params.insert(QStringLiteral("rate"), 1.0);
                             pr.params.insert(QStringLiteral("loop"), true);
                         } else if (asPortrait) {
                             pr.part = QStringLiteral("portrait");
                             pr.params.insert(QStringLiteral("x"), 0.78);
                             pr.params.insert(QStringLiteral("y"), 1.0);
                             pr.params.insert(QStringLiteral("h"), 0.95);
                             pr.params.insert(QStringLiteral("alpha"), 1.0);
                             pr.params.insert(QStringLiteral("sway"), 0.005);
                             pr.params.insert(QStringLiteral("swaySpeed"), 0.55);
                             pr.params.insert(QStringLiteral("beat"), 0.012);
                             pr.params.insert(QStringLiteral("glow"), 0.25);
                             pr.params.insert(QStringLiteral("glowColor"),
                                              QStringLiteral("#8fd8ff"));
                             // 每张立绘一个固定相位，编辑时预览和成片呼吸节奏一致
                             pr.params.insert(QStringLiteral("seed"),
                                              double(qHash(fileName) % 628) / 100.0);
                         } else {
                             pr.part = QStringLiteral("image");
                             pr.params.insert(QStringLiteral("fit"), QStringLiteral("cover"));
                             pr.params.insert(QStringLiteral("w"), 1.06);
                             pr.params.insert(QStringLiteral("h"), 1.06);
                             pr.params.insert(QStringLiteral("alpha"), 0.5);
                         }
                         // 数组顺序 = 绘制顺序。场景图/视频是铺满画幅的底，必须插到最前；
                         // 立绘压在它上面，追加到末尾。注意 background 渐变现在是「文字衬底」
                         // （排在场景图之后），所以底图不能 push_back —— 那会盖住衬底。
                         if (asPortrait)
                             s.background.push_back(pr);
                         else
                             s.background.insert(0, pr);
                     }
                 });
}

void MainWindow::exportCurrentFrame()
{
    const QString name = QStringLiteral("frame_%1.png")
                             .arg(int(m_t * m_project.canvas.fps), 6, 10, QLatin1Char('0'));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出当前帧"), name,
                                                      QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty())
        return;
    const QImage img = m_renderer.render(m_t);
    if (img.isNull() || !img.save(path))
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入：%1").arg(path));
}

void MainWindow::exportVideo()
{
    // 并行分片：内核按帧区间把整片切成 N 段，各自渲染再拼接（N=1 就是原来的串行路径）。
    // 全片 6402 帧实测 串行 71.5s → 4 路 58.3s，所以给个默认 1、上限 8 的旋钮。
    int jobs = 1;
    bool useCache = false;
    QString cacheDir;
    // 默认缓存落在工程目录下的 .sfc_render（纯临时产物，已进 .gitignore）
    const QString projDir = m_projectPath.isEmpty() ? QDir::currentPath()
                                                   : QFileInfo(m_projectPath).absolutePath();
    const QString defaultCacheDir = QDir(projDir).absoluteFilePath(QStringLiteral(".sfc_render"));
    {
        QDialog opt(this);
        opt.setWindowTitle(QStringLiteral("导出设置"));
        auto *lay = new QVBoxLayout(&opt);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("并行分片")));
        auto *spin = new QSpinBox;
        spin->setRange(1, 8);
        spin->setValue(1);
        spin->setSuffix(QStringLiteral(" 路"));
        row->addWidget(spin);
        row->addStretch(1);
        lay->addLayout(row);
        auto *hint = new QLabel(
            QStringLiteral("多核机器 4 路通常快 1.3–1.8 倍；1 = 串行（分片只对 MP4 生效）"));
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::Col::Tx3));
        lay->addWidget(hint);

        // 增量缓存：分片按「落在该时间范围内的镜头 + 歌词」指纹缓存，只重渲改动过的段。
        // 只在并行路径生效，所以勾上就强制 ≥2 路。
        auto *cacheChk = new QCheckBox(QStringLiteral("增量缓存（只重渲改动过的分片）"));
        cacheChk->setToolTip(QStringLiteral("勾选后强制至少 2 路：缓存只在并行路径生效"));
        lay->addWidget(cacheChk);

        auto *cacheRow = new QHBoxLayout;
        auto *cacheEdit = new QLineEdit(defaultCacheDir);
        cacheEdit->setEnabled(false);
        cacheEdit->setFont(Theme::mono(Theme::F::Small));
        cacheRow->addWidget(cacheEdit, 1);
        auto *cacheBtn = new QPushButton(QStringLiteral("…"));
        cacheBtn->setFixedWidth(28);
        cacheBtn->setEnabled(false);
        cacheRow->addWidget(cacheBtn);
        lay->addLayout(cacheRow);

        connect(cacheBtn, &QPushButton::clicked, &opt, [&opt, cacheEdit] {
            const QString d = QFileDialog::getExistingDirectory(
                &opt, QStringLiteral("增量缓存目录"), cacheEdit->text());
            if (!d.isEmpty())
                cacheEdit->setText(d);
        });
        connect(cacheChk, &QCheckBox::toggled, &opt, [spin, cacheEdit, cacheBtn](bool on) {
            cacheEdit->setEnabled(on);
            cacheBtn->setEnabled(on);
            if (on && spin->value() < 2)
                spin->setValue(2);
        });

        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(bb, &QDialogButtonBox::accepted, &opt, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &opt, &QDialog::reject);
        lay->addWidget(bb);
        if (opt.exec() != QDialog::Accepted)
            return;
        jobs = spin->value();
        useCache = cacheChk->isChecked();
        const QString typed = cacheEdit->text().trimmed();
        cacheDir = typed.isEmpty() ? defaultCacheDir : typed;
    }

    // 格式由扩展名决定（与 sf_dump 的 exportKindOf 同一套规则）：
    //   .mp4/.mov → H.264+AAC；.gif → 调色板动图；.webp → 动画 WebP；目录 → PNG 序列
    QString outPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出"), m_project.title + QStringLiteral(".mp4"),
        QStringLiteral("视频 MP4 (*.mp4);;动图 GIF (*.gif);;动画 WebP (*.webp);;"
                       "PNG 序列（下一步选目录）"));
    if (outPath.isEmpty())
        return;

    // 没扩展名 = 选了「PNG 序列」：这一步要的是一个目录
    if (QFileInfo(outPath).suffix().isEmpty()) {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择 PNG 序列输出目录"), QFileInfo(outPath).absolutePath());
        if (dir.isEmpty())
            return;
        outPath = dir;
    }

    const QString expSuffix = QFileInfo(outPath).suffix().toLower();
    const bool isMp4 = expSuffix == QLatin1String("mp4") || expSuffix == QLatin1String("mov")
                       || expSuffix == QLatin1String("mkv");
    const bool isGif = expSuffix == QLatin1String("gif");
    const bool isWebp = expSuffix == QLatin1String("webp");
    const bool pngSeq = !isMp4 && !isGif && !isWebp;
    const QString kindName = pngSeq ? QStringLiteral("PNG 序列")
                                    : (isGif ? QStringLiteral("GIF")
                                             : (isWebp ? QStringLiteral("WebP")
                                                       : QStringLiteral("MP4")));

    const QString exe = QCoreApplication::applicationDirPath() + QStringLiteral("/sf_dump.exe");
    if (!QFileInfo::exists(exe)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("找不到同目录下的 sf_dump.exe"));
        return;
    }

    // 导出读的是磁盘上的工程：先把当前内存状态落成临时 .sf，
    // 这样"所见即所导出"，也不会因为没保存过工程就导不了。
    const QString tmpSf = QDir::tempPath() + QStringLiteral("/sf_export_") + QString::number(quintptr(this))
                          + QStringLiteral(".sf");
    QString err;
    if (!m_project.save(tmpSf, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("临时工程写入失败：%1").arg(err));
        return;
    }

    auto *dlg = new QProgressDialog(
        isMp4 ? QStringLiteral("准备中…")
              : QStringLiteral("准备中…（%1 不带音轨，音轨请在下游合成）").arg(kindName),
        QStringLiteral("取消"), 0, 100, this);
    dlg->setWindowTitle(QStringLiteral("导出 %1").arg(kindName));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setValue(0);

    auto *proc = new QProcess(this);
    proc->setProgram(exe);
    QStringList expArgs{tmpSf, QStringLiteral("--export"), outPath, QStringLiteral("--size"),
                        QStringLiteral("%1x%2")
                            .arg(m_project.canvas.width)
                            .arg(m_project.canvas.height)};
    // GIF / WebP / PNG 序列没有音轨概念：显式掐掉，别等 sf_dump 改口径
    if (!isMp4)
        expArgs << QStringLiteral("--no-audio");
    // 增量缓存只在并行路径生效：勾了缓存就必须 ≥2 路
    if (useCache && jobs < 2)
        jobs = 2;
    // 分片只对 MP4 有意义（内核自己也会忽略），>1 才传，串行路径的 argv 保持原样
    if (jobs > 1)
        expArgs << QStringLiteral("--jobs") << QString::number(jobs);
    if (useCache && !cacheDir.isEmpty())
        expArgs << QStringLiteral("--cache") << cacheDir;
    proc->setArguments(expArgs);

    auto buf = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyReadStandardOutput, this, [proc, buf, dlg] {
        buf->append(proc->readAllStandardOutput());
        // sf_dump 每 10% 打一行 "  N/M 帧  (NN%)"
        int idx = buf->lastIndexOf(')');
        if (idx < 0)
            return;
        int open = buf->lastIndexOf('(', idx);
        if (open < 0 || idx - open < 2)
            return;
        const int pct = buf->mid(open + 1, idx - open - 1).trimmed().toInt();
        if (pct >= 0 && pct <= 100)
            dlg->setValue(pct);
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, proc, dlg, tmpSf, outPath, pngSeq, kindName](int code, QProcess::ExitStatus) {
                dlg->setValue(100);
                dlg->close();
                dlg->deleteLater();
                const QString tail = QString::fromLocal8Bit(proc->readAllStandardError()).right(400);
                proc->deleteLater();
                QFile::remove(tmpSf);
                if (code == 0 && QFileInfo::exists(outPath)) {
                    QString info;
                    if (pngSeq) {
                        const int frames = int(QDir(outPath)
                                                   .entryList({QStringLiteral("*.png")},
                                                              QDir::Files)
                                                   .size());
                        info = QStringLiteral("%1\n%2 帧 PNG（目录）").arg(outPath).arg(frames);
                    } else {
                        info = QStringLiteral("%1\n%2 MB")
                                   .arg(outPath,
                                        QString::number(QFileInfo(outPath).size() / 1048576.0,
                                                        'f', 1));
                    }
                    QMessageBox::information(
                        this, QStringLiteral("导出完成（%1）").arg(kindName), info);
                } else {
                    QMessageBox::warning(this, QStringLiteral("导出失败"),
                                         tail.isEmpty() ? QStringLiteral("退出码 %1").arg(code)
                                                        : tail);
                }
            });

    connect(dlg, &QProgressDialog::canceled, this, [proc] {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });

    proc->start();
    if (!proc->waitForStarted(5000)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法启动 sf_dump.exe"));
        proc->deleteLater();
        dlg->deleteLater();
    }
}

void MainWindow::openProjectDir()
{
    const QString dir = m_projectPath.isEmpty() ? QCoreApplication::applicationDirPath()
                                                : QFileInfo(m_projectPath).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::showShortcuts()
{
    QMessageBox::information(
        this, QStringLiteral("快捷键"),
        QStringLiteral("空格            播放 / 暂停\n"
                       "Home / End      回到开头 / 末尾\n"
                       "← / →           上一帧 / 下一帧\n"
                       "Ctrl+← / →      上一镜 / 下一镜\n"
                       "Ctrl+Z          撤销\n"
                       "Ctrl+Shift+Z    重做\n"
                       "Delete          删除选中（有部件选择先删部件）\n"
                       "R               换一个（重掷种子 + 换背景）\n"
                       "\n时间轴：\n"
                       "滚轮            横向平移（预览长时间轴）\n"
                       "Ctrl+滚轮       以光标为锚缩放\n"
                       "Shift+F         适配全片\n"
                       "拖块中间 / 边缘  平移镜头 / 裁剪时长\n"
                       "上下拖部件块     换层（一条撤销）\n"
                       "空白处拖框       框选部件（Shift 追加）\n"
                       "Shift 点块       加选 / 减选\n"
                       "Ctrl 点部件块    选中该层全部镜头\n"
                       "Esc             取消拖动 / 清空选择"));
}

// ── 检查器 / 编辑 ───────────────────────────────────────────────────────────

void MainWindow::patchProject(const QString &label, const std::function<void(sf::Project &)> &fn)
{
    m_undo.push_back(m_project);
    while (m_undo.size() > kUndoLimit)
        m_undo.removeFirst();
    m_redo.clear();

    fn(m_project);

    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    if (m_preview)
        m_preview->invalidateFrame();
    if (m_timeline) {
        m_timeline->setProject(&m_project);
        m_timeline->setTime(m_t);
    }
    refreshInspector(true);
    updateUndoActions();
    markDirty();
    Q_UNUSED(label)
}

// ── 拖动合并（立绘等高频参数：一次拖动一条撤销项）────────────────────────────

void MainWindow::beginParamDrag(const QString &label)
{
    if (m_dragActive)
        return;
    m_dragActive = true;
    m_dragDirty = false;
    m_dragBefore = m_project; // 这一份就是松开时要压进撤销栈的状态
    Q_UNUSED(label)           // 标签目前只用于将来在撤销菜单里展示单步名
}

void MainWindow::endParamDrag()
{
    if (!m_dragActive)
        return;
    m_dragActive = false;
    if (m_dragDirty) {
        m_undo.push_back(m_dragBefore);
        while (m_undo.size() > kUndoLimit)
            m_undo.removeFirst();
        m_redo.clear();
        markDirty();
    }
    m_dragDirty = false;
    refreshInspector(true);
    updateUndoActions();
}

void MainWindow::patchProjectDrag(const QString &label,
                                  const std::function<void(sf::Project &)> &fn)
{
    beginParamDrag(label);
    fn(m_project);
    m_dragDirty = true;

    // 注意：这里**不**刷检查器 —— 拖动中重建控件会把用户手里的滑块拽回去
    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    if (m_preview)
        m_preview->invalidateFrame();
    if (m_timeline) {
        m_timeline->setProject(&m_project);
        m_timeline->setTime(m_t);
    }
}

void MainWindow::applyPortraitParam(const QString &key, double value)
{
    if (m_insPortraitPath.isEmpty())
        return;
    const int anchor = currentShotIndex();
    if (anchor < 0 || anchor >= m_project.shots.size())
        return;

    // 键盘一格格改、点滑槽跳值都不会有 sliderPressed：单步也要自成一条命令
    const bool solo = !m_dragActive;
    const QString path = m_insPortraitPath;
    const QString label = QStringLiteral("立绘 %1").arg(key);
    beginParamDrag(label);

    // at/outAt 是组级：改一处，同一个人所在的整段镜头一起改
    const bool groupWide =
        key == QLatin1String("at") || key == QLatin1String("outAt");
    patchProjectDrag(label, [key, value, anchor, path, groupWide](sf::Project &p) {
        QVector<int> idx;
        idx.push_back(anchor);
        const QVector<int> grp = groupWide ? portraitGroup(p, anchor, path) : QVector<int>();
        if (!grp.isEmpty())
            idx = grp;

        for (int i : idx) {
            if (i < 0 || i >= p.shots.size())
                continue;
            sf::PartRef *pr = findBackgroundPart(p.shots[i], QStringLiteral("portrait"), path);
            if (!pr)
                continue;
            if (key == QLatin1String("flip"))
                pr->params.insert(key, value > 0.5);
            else if (value <= 0.0001 && groupWide)
                pr->params.remove(key); // 0 = 关：整段常显，别写个 at=0 让渲染层算淡入
            else
                pr->params.insert(key, value);
        }
    });

    if (solo)
        endParamDrag();
}

void MainWindow::removeBackgroundPart(const QString &part)
{
    const int i = currentShotIndex();
    if (i < 0)
        return;
    const QString id = m_project.shots.at(i).id;
    const QString path =
        part == QLatin1String("portrait") ? m_insPortraitPath : m_insPlatePath;
    const QString label = part == QLatin1String("portrait") ? QStringLiteral("删除立绘")
                                                           : QStringLiteral("删除场景图");

    patchProject(label, [id, part, path](sf::Project &p) {
        for (auto &s : p.shots) {
            if (s.id != id)
                continue;
            for (int k = 0; k < s.background.size(); ++k) {
                const sf::PartRef &pr = s.background.at(k);
                if (pr.part != part)
                    continue;
                if (!path.isEmpty()
                    && pr.params.value(QStringLiteral("path")).toString() != path)
                    continue;
                // 只删一个；数组位置由快照撤销原样放回去（顺序即绘制顺序）
                s.background.remove(k);
                break;
            }
        }
    });
}

void MainWindow::applyDragEdit(const QString &label,
                               const std::function<void(sf::Project &)> &fn)
{
    // 拖动中：只改工程；单步（键盘/点滑槽）：自己开合，自成一条
    const bool solo = !m_dragActive;
    beginParamDrag(label);
    patchProjectDrag(label, fn);
    if (solo)
        endParamDrag();
}

void MainWindow::applyVideoParam(const QString &key, double value)
{
    const int i = currentShotIndex();
    if (i < 0 || i >= m_project.shots.size())
        return;
    if (!findVideoPart(m_project.shots.at(i)))
        return;
    const QString id = m_project.shots.at(i).id;
    applyDragEdit(QStringLiteral("视频 %1").arg(key), [id, key, value](sf::Project &p) {
        for (auto &s : p.shots) {
            if (s.id != id)
                continue;
            for (QVector<sf::PartRef> *v : {&s.background, &s.decor}) {
                for (auto &pr : *v) {
                    if (pr.part != QLatin1String("video"))
                        continue;
                    // loop 必须是 bool：渲染层用 toBool(true)，写成 1.0 会被当成"没给"
                    if (key == QLatin1String("loop"))
                        pr.params.insert(key, value > 0.5);
                    else
                        pr.params.insert(key, value);
                }
            }
        }
    });
}

void MainWindow::applyLayerRotate(double deg)
{
    if (!m_insRotTarget)
        return;
    const QStringList sel = m_insRotTarget->currentData().toString().split(QLatin1Char(':'));
    if (sel.size() != 2)
        return;
    const int layer = sel.at(0).toInt();
    const int idx = sel.at(1).toInt();
    const int i = currentShotIndex();
    if (i < 0)
        return;
    const QString id = m_project.shots.at(i).id;
    applyDragEdit(QStringLiteral("旋转 %1°").arg(deg, 0, 'f', 0),
                  [id, layer, idx, deg](sf::Project &p) {
                      for (auto &s : p.shots) {
                          if (s.id != id)
                              continue;
                          QVector<sf::PartRef> *v = layerParts(s, layer);
                          if (!v || idx < 0 || idx >= v->size())
                              continue;
                          // 0 度就把键删掉，别在 .sf 里留一堆 rotate:0
                          if (qAbs(deg) < 0.01)
                              (*v)[idx].params.remove(QStringLiteral("rotate"));
                          else
                              (*v)[idx].params.insert(QStringLiteral("rotate"), deg);
                      }
                  });
}

void MainWindow::applyCameraKeyParam(const QString &field, double value)
{
    const int i = currentShotIndex();
    if (i < 0 || i >= m_project.shots.size())
        return;
    const int keyIdx = m_insKeyIndex;
    const QString id = m_project.shots.at(i).id;
    applyDragEdit(QStringLiteral("运镜 %1").arg(field),
                  [id, field, value, keyIdx](sf::Project &p) {
                      for (auto &s : p.shots) {
                          if (s.id != id)
                              continue;
                          if (!s.hasCamera) {
                              s.hasCamera = true;
                              s.camera.part = QStringLiteral("camera");
                              s.camera.valid = true;
                          }
                          QJsonArray keys =
                              s.camera.params.value(QStringLiteral("keys")).toArray();
                          if (keyIdx < 0 || keyIdx >= keys.size())
                              continue;
                          QJsonObject o = keys.at(keyIdx).toObject();
                          o.insert(field, value);
                          keys.replace(keyIdx, o); // 顺序不动：渲染层自己按 t 排序
                          s.camera.params.insert(QStringLiteral("keys"), keys);
                      }
                  });
}

void MainWindow::addCameraKeyAtPlayhead()
{
    const int i = currentShotIndex();
    if (i < 0 || i >= m_project.shots.size())
        return;
    const sf::Shot &sh = m_project.shots.at(i);
    const QString id = sh.id;
    const double t = std::min(std::max(m_t, sh.start), sh.end);
    const int at = int(sh.camera.params.value(QStringLiteral("keys")).toArray().size());
    patchProject(QStringLiteral("加运镜关键帧"), [id, t](sf::Project &p) {
        for (auto &s : p.shots) {
            if (s.id != id)
                continue;
            if (!s.hasCamera) {
                s.hasCamera = true;
                s.camera.part = QStringLiteral("camera");
                s.camera.valid = true;
            }
            QJsonArray keys = s.camera.params.value(QStringLiteral("keys")).toArray();
            QJsonObject o;
            o.insert(QStringLiteral("t"), t);
            o.insert(QStringLiteral("zoom"), 1.0);
            o.insert(QStringLiteral("x"), 0.5);
            o.insert(QStringLiteral("y"), 0.5);
            keys.append(o);
            s.camera.params.insert(QStringLiteral("keys"), keys);
            // keys ≥ 2 时会接管缩放，两段式 zoom 归一到 1，免得新旧两套叠乘
            QJsonArray z;
            z.append(1.0);
            z.append(1.0);
            s.camera.params.insert(QStringLiteral("zoom"), z);
        }
    });
    m_insKeyIndex = at; // 选中刚加的那个
    refreshInspector(true);
}

void MainWindow::removeCurrentCameraKey()
{
    const int i = currentShotIndex();
    if (i < 0 || i >= m_project.shots.size())
        return;
    const int keyIdx = m_insKeyIndex;
    if (keyIdx < 0)
        return;
    const QString id = m_project.shots.at(i).id;
    patchProject(QStringLiteral("删运镜关键帧"), [id, keyIdx](sf::Project &p) {
        for (auto &s : p.shots) {
            if (s.id != id)
                continue;
            QJsonArray keys = s.camera.params.value(QStringLiteral("keys")).toArray();
            if (keyIdx >= keys.size())
                continue;
            keys.removeAt(keyIdx);
            s.camera.params.insert(QStringLiteral("keys"), keys);
        }
    });
    m_insKeyIndex = std::max(0, keyIdx - 1);
    refreshInspector(true);
}

void MainWindow::applyLayerTint(const QString &hex, double amount)
{
    if (!m_insRotTarget)
        return;
    const QStringList sel = m_insRotTarget->currentData().toString().split(QLatin1Char(':'));
    if (sel.size() != 2)
        return;
    const int layer = sel.at(0).toInt();
    const int idx = sel.at(1).toInt();
    const int i = currentShotIndex();
    if (i < 0)
        return;
    const QString id = m_project.shots.at(i).id;

    // 目标当前状态：值没变就别压一条「什么也没做」的撤销项
    // （染色量为 0 时改颜色是常见操作，不该在历史里留噪声）
    const QString wantTint = (amount <= 0.001) ? QString() : hex;
    const double wantAmt = (amount <= 0.001) ? 0.0 : amount;
    {
        const QVector<sf::PartRef> *cur = layerParts(m_project.shots.at(i), layer);
        if (!cur || idx < 0 || idx >= cur->size())
            return;
        const QJsonObject &prm = cur->at(idx).params;
        if (prm.value(QStringLiteral("tint")).toString() == wantTint
            && qAbs(prm.value(QStringLiteral("tintAmount")).toDouble(0) - wantAmt) < 1e-9) {
            return;
        }
    }

    applyDragEdit(QStringLiteral("染色"), [id, layer, idx, wantTint, wantAmt](sf::Project &p) {
        for (auto &s : p.shots) {
            if (s.id != id)
                continue;
            QVector<sf::PartRef> *v = layerParts(s, layer);
            if (!v || idx < 0 || idx >= v->size())
                continue;
            sf::PartRef &pr = (*v)[idx];
            if (wantTint.isEmpty() || wantAmt <= 0.001) {
                pr.params.remove(QStringLiteral("tint"));
                pr.params.remove(QStringLiteral("tintAmount"));
            } else {
                pr.params.insert(QStringLiteral("tint"), wantTint);
                pr.params.insert(QStringLiteral("tintAmount"), wantAmt);
            }
        }
    });
}

void MainWindow::importLyricsLrc()
{
    const QString baseDir = m_projectPath.isEmpty() ? QDir::currentPath()
                                                    : QFileInfo(m_projectPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入歌词 (LRC)"), baseDir,
        QStringLiteral("LRC 歌词 (*.lrc);;文本 (*.txt);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    // 先在副本上解析：解析不出来就一个字段都不动，不往工程里塞半截歌词
    sf::Project probe;
    QString err;
    if (!sf::importLrc(path, probe, &err)) {
        QMessageBox::warning(this, QStringLiteral("导入歌词失败"),
                             err.isEmpty() ? QStringLiteral("无法解析这个 .lrc") : err);
        return;
    }
    int withSyllables = 0;
    for (const sf::Lyric &l : probe.lyrics)
        if (l.syllables.size() > 1)
            ++withSyllables;
    const int lines = int(probe.lyrics.size());

    patchProject(QStringLiteral("导入歌词"), [path](sf::Project &p) {
        QString e;
        if (!sf::importLrc(path, p, &e))
            qWarning("[lrc] 导入失败：%s", qUtf8Printable(e));
    });

    // patchProject 已经重设了渲染器/时间轴，但歌词轨是时间轴内部缓存的东西，
    // 这里再显式刷一遍 setProject + setTime，确保新歌词立刻画出来
    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    if (m_preview) {
        m_preview->setEngine(&m_project, &m_renderer);
        m_preview->invalidateFrame();
    }
    if (m_timeline) {
        m_timeline->setProject(&m_project);
        m_timeline->setTime(m_t);
    }
    refreshInspector(true);

    QMessageBox::information(
        this, QStringLiteral("导入歌词"),
        QStringLiteral("已导入 %1 行歌词（其中 %2 行带逐字时间）\nCtrl+Z 可撤销")
            .arg(lines)
            .arg(withSyllables));
}

void MainWindow::exportLyricsLrc()
{
    if (m_project.lyrics.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出歌词"),
                                 QStringLiteral("这个工程还没有歌词。"));
        return;
    }
    const QString baseDir = m_projectPath.isEmpty() ? QDir::currentPath()
                                                    : QFileInfo(m_projectPath).absolutePath();
    const QString stem = m_project.title.isEmpty() ? QStringLiteral("lyrics") : m_project.title;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出歌词 (LRC)"), baseDir + QLatin1Char('/') + stem + QStringLiteral(".lrc"),
        QStringLiteral("LRC 歌词 (*.lrc)"));
    if (path.isEmpty())
        return;

    QString err;
    if (!sf::exportLrc(m_project, path, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出歌词失败"),
                             err.isEmpty() ? QStringLiteral("写入失败") : err);
        return;
    }
    QMessageBox::information(this, QStringLiteral("导出歌词"),
                             QStringLiteral("已导出 %1 行歌词\n%2")
                                 .arg(m_project.lyrics.size())
                                 .arg(path));
}

bool MainWindow::applyDetectedBpm(QString *error)
{
    const QString audio = resolveProjectAudio(m_project);
    if (audio.isEmpty()) {
        if (error)
            *error = QStringLiteral("工程没有可用的音轨文件");
        return false;
    }
    if (m_ffmpegPath.isEmpty())
        m_ffmpegPath = resolveFfmpeg();

    QString perr;
    const sf::AudioPeaks peaks = sf::computePeaks(audio, m_ffmpegPath, &perr);
    if (peaks.isEmpty()) {
        if (error)
            *error = perr.isEmpty() ? QStringLiteral("音轨解码失败") : perr;
        return false;
    }
    const sf::BeatInfo beat = sf::detectBeats(peaks);
    if (beat.bpm <= 0) {
        if (error)
            *error = QStringLiteral("测不出稳定节拍（音轨太短或太弱）");
        return false;
    }

    const double bpm = beat.bpm;
    const double offset = beat.offset;
    const double conf = beat.confidence;
    patchProject(QStringLiteral("检测 BPM"), [bpm, offset](sf::Project &p) {
        p.bpm = bpm;
        p.offset = offset; // 首拍偏移：节拍吸附/节拍冲击都按它对齐
    });

    if (m_bpmLabel) {
        m_bpmLabel->setText(QStringLiteral("BPM %1 · 首拍 %2s · 置信 %3")
                                .arg(bpm, 0, 'f', 2)
                                .arg(offset, 0, 'f', 3)
                                .arg(conf, 0, 'f', 2));
    }
    return true;
}

void MainWindow::detectBpm()
{
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    const bool ok = applyDetectedBpm(&err);
    QGuiApplication::restoreOverrideCursor();

    if (!ok) {
        if (m_bpmLabel)
            m_bpmLabel->setText(QStringLiteral("BPM —"));
        QMessageBox::warning(this, QStringLiteral("检测 BPM"),
                             err.isEmpty() ? QStringLiteral("分析失败") : err);
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("检测 BPM"),
        QStringLiteral("BPM %1 · 首拍 %2s\n已写入工程（Ctrl+Z 可撤销）")
            .arg(m_project.bpm, 0, 'f', 2)
            .arg(m_project.offset, 0, 'f', 3));
}

void MainWindow::insertPartPreset(const QString &label)
{
    const sf::PartPreset ps = sf::presetByLabel(label);
    if (ps.label.isEmpty())
        return; // 未命中（不是预设条目）
    const int i = currentShotIndex();
    if (i < 0) {
        QMessageBox::information(this, QStringLiteral("插入部件"),
                                 QStringLiteral("先把播放头移到一个镜头上。"));
        return;
    }
    const QString id = m_project.shots.at(i).id;
    patchProject(QStringLiteral("插入 %1").arg(ps.label), [id, ps](sf::Project &p) {
        for (auto &s : p.shots) {
            if (s.id != id)
                continue;
            sf::PartRef pr;
            pr.part = ps.part;
            pr.valid = true;
            pr.params = ps.params; // 预设自带一套能立刻看出效果的参数
            // 层由预设决定：插错层等于把字盖住，所以不能一律塞 background
            if (ps.layer == QLatin1String("text"))
                s.text.push_back(pr);
            else if (ps.layer == QLatin1String("decor"))
                s.decor.push_back(pr);
            else if (ps.layer == QLatin1String("overlay"))
                s.overlay.push_back(pr);
            else
                s.background.push_back(pr);
        }
    });
}

void MainWindow::applyEditCmd(const sf::EditCmd &cmd)
{
    // 只借用 ProjectEditor 的「合法性 + clamp」逻辑，不使用它内部的撤销栈：
    // 撤销入口必须唯一，否则 Ctrl+Z 会出现两套历史互相打架。
    sf::ProjectEditor ed(&m_project);
    const sf::Project before = m_project;
    const sf::EditResult r = ed.apply(cmd);
    if (!r.ok) {
        // 失败时工程必须原样恢复：半截改动比不改更糟
        m_project = before;
        qWarning("[edit] %s",
                 qUtf8Printable(r.note.isEmpty() ? QStringLiteral("位置冲突") : r.note));
        return;
    }

    afterProjectEdit(before);
}

void MainWindow::applyEditBatch(const QVector<sf::EditCmd> &cmds, const QString &label)
{
    if (cmds.isEmpty())
        return;
    // 与单条命令同一套快照撤销：整批要么全成、要么工程原样不动
    sf::ProjectEditor ed(&m_project);
    const sf::Project before = m_project;
    if (!ed.applyBatch(cmds, label)) {
        m_project = before;
        qWarning("[edit] %s", qUtf8Printable(label.isEmpty() ? QStringLiteral("批量编辑失败")
                                                            : label));
        return;
    }
    afterProjectEdit(before);
}

void MainWindow::afterProjectEdit(const sf::Project &before)
{
    m_undo.push_back(before);
    while (m_undo.size() > kUndoLimit)
        m_undo.removeFirst();
    m_redo.clear();

    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    if (m_preview)
        m_preview->invalidateFrame();
    if (m_timeline) {
        m_timeline->setProject(&m_project);
        m_timeline->setTime(m_t);
    }
    refreshTimeCode();
    refreshInspector(true);
    updateUndoActions();
    markDirty();
}

void MainWindow::undoEdit()
{
    if (m_undo.isEmpty())
        return;
    m_redo.push_back(m_project);
    m_project = m_undo.takeLast();
    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    if (m_preview) {
        m_preview->setEngine(&m_project, &m_renderer);
        m_preview->invalidateFrame();
    }
    if (m_timeline) {
        m_timeline->setProject(&m_project);
        m_timeline->setTime(m_t);
    }
    refreshInspector(true);
    updateUndoActions();
}

void MainWindow::redoEdit()
{
    if (m_redo.isEmpty())
        return;
    m_undo.push_back(m_project);
    m_project = m_redo.takeLast();
    m_shotStarts = m_project.shotStarts();
    m_renderer.setProject(&m_project);
    if (m_preview) {
        m_preview->setEngine(&m_project, &m_renderer);
        m_preview->invalidateFrame();
    }
    if (m_timeline) {
        m_timeline->setProject(&m_project);
        m_timeline->setTime(m_t);
    }
    refreshInspector(true);
    updateUndoActions();
}

void MainWindow::updateUndoActions()
{
    if (m_undoAct) {
        m_undoAct->setEnabled(!m_undo.isEmpty());
        m_undoAct->setText(m_undo.isEmpty() ? QStringLiteral("撤销")
                                            : QStringLiteral("撤销（%1 步）").arg(m_undo.size()));
    }
    if (m_redoAct)
        m_redoAct->setEnabled(!m_redo.isEmpty());
}

void MainWindow::refreshInspector(bool force)
{
    if (m_project.shots.isEmpty()) {
        m_insShotId.clear();
        if (m_insRange)
            m_insRange->setText(QStringLiteral("—"));
        return;
    }
    // 时间轴上有选中时，检查器跟着选中走；没选中才回落到播放头所在镜头。
    // 否则「点中一个镜头却改不动它」—— 非编里选中即编辑目标，这是肌肉记忆。
    int i = -1;
    if (!m_selShotId.isEmpty()) {
        for (int k = 0; k < m_project.shots.size(); ++k) {
            if (m_project.shots.at(k).id == m_selShotId) {
                i = k;
                break;
            }
        }
    }
    if (i < 0)
        i = m_project.shotIndexAt(m_t);
    if (i < 0 || i >= m_project.shots.size())
        return;
    const sf::Shot &sh = m_project.shots.at(i);

    if (m_insRange)
        m_insRange->setText(QStringLiteral("%1 → %2   时长 %3s")
                                .arg(sh.start, 0, 'f', 2)
                                .arg(sh.end, 0, 'f', 2)
                                .arg(sh.end - sh.start, 0, 'f', 2));

    // 播放时每帧都会调到这里：镜头没变就只更新区间，不重设控件
    if (!force && m_insShotId == sh.id)
        return;
    m_insShotId = sh.id;

    if (m_insHead)
        m_insHead->setText(QStringLiteral("检查器 · %1").arg(sh.id));

    m_insBusy = true;

    if (m_insBgKind) {
        const QString kind = backgroundKindOf(sh);
        const int idx = m_insBgKind->findText(kind, Qt::MatchStartsWith);
        if (idx >= 0)
            m_insBgKind->setCurrentIndex(idx);
    }

    if (m_insFromInt) {
        int idx = 3;
        if (sh.hasEntrance) {
            const QString st = sh.entrance.params.value(QStringLiteral("style")).toString();
            idx = st == QLatin1String("popUp") ? 0 : st == QLatin1String("trackIn") ? 1 : 2;
        }
        m_insFromInt->setCurrentIndex(idx);
    }

    double za = 1.0;
    double zb = 1.0;
    double punch = 0.0;
    if (sh.hasCamera) {
        const QJsonArray arr = sh.camera.params.value(QStringLiteral("zoom")).toArray();
        if (arr.size() > 0)
            za = arr.at(0).toDouble();
        if (arr.size() > 1)
            zb = arr.at(1).toDouble();
        punch = sh.camera.params.value(QStringLiteral("punch")).toDouble();
    }
    if (m_insZoomA)
        m_insZoomA->setValue(int(std::lround(za * 100)));
    if (m_insZoomB)
        m_insZoomB->setValue(int(std::lround(zb * 100)));
    if (m_insPunch)
        m_insPunch->setValue(int(std::lround(punch * 100)));

    // ── 运镜关键帧：绝对秒 + 「要落到画面中心的点」
    const QJsonArray keys =
        sh.hasCamera ? sh.camera.params.value(QStringLiteral("keys")).toArray() : QJsonArray();
    if (m_insKeyHint) {
        m_insKeyHint->setText(
            keys.isEmpty()
                ? QStringLiteral("运镜关键帧：无（至少 2 个点才会接管缩放/对焦）")
                : QStringLiteral("运镜关键帧：%1 个 · 时间轴上的运镜编辑可后置").arg(keys.size()));
    }
    if (m_insKeySel) {
        m_insKeySel->clear();
        for (int k = 0; k < keys.size(); ++k) {
            const QJsonObject o = keys.at(k).toObject();
            m_insKeySel->addItem(QStringLiteral("%1 · %2s")
                                     .arg(k + 1)
                                     .arg(o.value(QStringLiteral("t")).toDouble(), 0, 'f', 2));
        }
        if (m_insKeyIndex >= keys.size())
            m_insKeyIndex = std::max(0, int(keys.size()) - 1);
        if (!keys.isEmpty())
            m_insKeySel->setCurrentIndex(m_insKeyIndex);
        m_insKeySel->setEnabled(!keys.isEmpty());
    }
    const bool hasKey = m_insKeyIndex >= 0 && m_insKeyIndex < keys.size();
    const QJsonObject keyObj = hasKey ? keys.at(m_insKeyIndex).toObject() : QJsonObject();
    const int keySpan = std::max(100, int(std::lround(m_project.duration() * 100)));
    if (m_insKeyT)
        m_insKeyT->setRange(0, keySpan);
    if (m_insKeyT) {
        m_insKeyT->setEnabled(hasKey);
        if (hasKey)
            m_insKeyT->setValue(
                int(std::lround(keyObj.value(QStringLiteral("t")).toDouble(0) * 100)));
    }
    if (m_insKeyZoom) {
        m_insKeyZoom->setEnabled(hasKey);
        if (hasKey)
            m_insKeyZoom->setValue(
                int(std::lround(keyObj.value(QStringLiteral("zoom")).toDouble(1.0) * 100)));
    }
    if (m_insKeyX) {
        m_insKeyX->setEnabled(hasKey);
        if (hasKey)
            m_insKeyX->setValue(
                int(std::lround(keyObj.value(QStringLiteral("x")).toDouble(0.5) * 100)));
    }
    if (m_insKeyY) {
        m_insKeyY->setEnabled(hasKey);
        if (hasKey)
            m_insKeyY->setValue(
                int(std::lround(keyObj.value(QStringLiteral("y")).toDouble(0.5) * 100)));
    }
    if (m_insKeyDel)
        m_insKeyDel->setEnabled(hasKey);

    if (m_insSeed)
        m_insSeed->setText(QString::number(sh.seed));

    // ── 立绘 / 场景图：都从 background 混合数组里按 part 名取（不是 background[0]）
    const sf::PartRef *pt = findBackgroundPart(sh, QStringLiteral("portrait"));
    const sf::PartRef *plate = findBackgroundPart(sh, QStringLiteral("image"));
    m_insPortraitPath = pt ? pt->params.value(QStringLiteral("path")).toString() : QString();
    m_insPlatePath = plate ? plate->params.value(QStringLiteral("path")).toString() : QString();
    const bool hasPt = pt != nullptr;

    if (m_insPtName) {
        m_insPtName->setText(hasPt ? QFileInfo(m_insPortraitPath).fileName()
                                   : QStringLiteral("本镜头没有立绘"));
    }
    if (m_insPlateName) {
        m_insPlateName->setText(plate ? QStringLiteral("场景图：%1")
                                            .arg(QFileInfo(m_insPlatePath).fileName())
                                      : QStringLiteral("场景图：无"));
    }
    if (m_insPtDel)
        m_insPtDel->setEnabled(hasPt);
    if (m_insPlateDel)
        m_insPlateDel->setEnabled(plate != nullptr);

    // at/outAt 是绝对秒，滑块走厘秒，量程跟着工程时长走
    const int span = std::max(100, int(std::lround(m_project.duration() * 100)));
    if (m_insPtAt)
        m_insPtAt->setRange(0, span);
    if (m_insPtOut)
        m_insPtOut->setRange(0, span);

    const auto ptNum = [pt](const char *key, double dflt) {
        if (!pt)
            return int(std::lround(dflt * 100));
        const QString k = QString::fromLatin1(key);
        return int(std::lround(pt->params.value(k).toDouble(dflt) * 100));
    };
    if (m_insPtX)
        m_insPtX->setValue(ptNum("x", 0.78));
    if (m_insPtY)
        m_insPtY->setValue(ptNum("y", 1.0));
    if (m_insPtH)
        m_insPtH->setValue(ptNum("h", 0.95));
    if (m_insPtAlpha)
        m_insPtAlpha->setValue(ptNum("alpha", 1.0));
    if (m_insPtGlow)
        m_insPtGlow->setValue(ptNum("glow", 0.0));
    if (m_insPtFlip)
        m_insPtFlip->setCurrentIndex(
            hasPt && pt->params.value(QStringLiteral("flip")).toBool() ? 1 : 0);

    // 没有 at/outAt 这个键 = 关（滑块 0），不是"从 0 秒进场"
    const auto ptAt = [pt](const char *key) {
        if (!pt)
            return 0;
        const QString k = QString::fromLatin1(key);
        return pt->params.contains(k) ? int(std::lround(pt->params.value(k).toDouble() * 100)) : 0;
    };
    if (m_insPtAt)
        m_insPtAt->setValue(ptAt("at"));
    if (m_insPtOut)
        m_insPtOut->setValue(ptAt("outAt"));

    if (m_insPtX)
        m_insPtX->setEnabled(hasPt);
    if (m_insPtY)
        m_insPtY->setEnabled(hasPt);
    if (m_insPtH)
        m_insPtH->setEnabled(hasPt);
    if (m_insPtAlpha)
        m_insPtAlpha->setEnabled(hasPt);
    if (m_insPtGlow)
        m_insPtGlow->setEnabled(hasPt);
    if (m_insPtFlip)
        m_insPtFlip->setEnabled(hasPt);
    if (m_insPtAt)
        m_insPtAt->setEnabled(hasPt);
    if (m_insPtOut)
        m_insPtOut->setEnabled(hasPt);

    // ── 视频：in / rate / loop（默认从所在镜头起点开演，见 sf_render 的注释）
    const sf::PartRef *vid = findVideoPart(sh);
    const bool hasVid = vid != nullptr;
    m_insVideoPath = hasVid ? vid->params.value(QStringLiteral("path")).toString() : QString();
    if (m_insVideoName) {
        m_insVideoName->setText(hasVid ? QFileInfo(m_insVideoPath).fileName()
                                       : QStringLiteral("本镜头没有视频"));
    }
    const auto vidNum = [vid](const char *key, double dflt) {
        if (!vid)
            return int(std::lround(dflt * 100));
        return int(std::lround(
            vid->params.value(QString::fromLatin1(key)).toDouble(dflt) * 100));
    };
    if (m_insVidIn)
        m_insVidIn->setValue(vidNum("in", 0.0));
    if (m_insVidRate)
        m_insVidRate->setValue(vidNum("rate", 1.0));
    if (m_insVidLoop)
        m_insVidLoop->setCurrentIndex(
            hasVid && !vid->params.value(QStringLiteral("loop")).toBool(true) ? 1 : 0);
    if (m_insVidIn)
        m_insVidIn->setEnabled(hasVid);
    if (m_insVidRate)
        m_insVidRate->setEnabled(hasVid);
    if (m_insVidLoop)
        m_insVidLoop->setEnabled(hasVid);

    // ── 旋转：列出本镜头所有可旋转部件，滑杆同步到当前目标
    if (m_insRotTarget) {
        const QString keep = m_insRotTarget->currentData().toString();
        m_insRotTarget->clear();
        auto addLayer = [this](int layer, const QString &tag, const QVector<sf::PartRef> &v) {
            for (int k = 0; k < v.size(); ++k) {
                const QString part = v.at(k).part;
                if (part != QLatin1String("text") && part != QLatin1String("image")
                    && part != QLatin1String("video") && part != QLatin1String("portrait"))
                    continue;
                QString name = part;
                const QString p = v.at(k).params.value(QStringLiteral("path")).toString();
                if (!p.isEmpty())
                    name = QFileInfo(p).fileName();
                else if (part == QLatin1String("text"))
                    name = v.at(k).params.value(QStringLiteral("text")).toString().left(10);
                m_insRotTarget->addItem(QStringLiteral("%1 · %2").arg(tag, name),
                                        QStringLiteral("%1:%2").arg(layer).arg(k));
            }
        };
        addLayer(0, QStringLiteral("背景"), sh.background);
        addLayer(1, QStringLiteral("文字"), sh.text);
        addLayer(2, QStringLiteral("装饰"), sh.decor);
        addLayer(3, QStringLiteral("覆盖"), sh.overlay);

        int sel = keep.isEmpty() ? -1 : m_insRotTarget->findData(keep);
        if (sel < 0 && m_insRotTarget->count() > 0)
            sel = 0;
        if (sel >= 0)
            m_insRotTarget->setCurrentIndex(sel);

        const bool hasTarget = m_insRotTarget->count() > 0;
        m_insRotTarget->setEnabled(hasTarget);
        if (m_insRotate) {
            m_insRotate->setEnabled(hasTarget);
            double rot = 0;
            if (hasTarget) {
                const QStringList sl =
                    m_insRotTarget->currentData().toString().split(QLatin1Char(':'));
                if (sl.size() == 2) {
                    const QVector<sf::PartRef> *v = layerParts(sh, sl.at(0).toInt());
                    const int k = sl.at(1).toInt();
                    if (v && k >= 0 && k < v->size())
                        rot = v->at(k).params.value(QStringLiteral("rotate")).toDouble(0);
                }
            }
            m_insRotate->setValue(int(std::lround(rot)));
        }
    }

    // ── 染色（tint / tintAmount）：跟旋转共用同一个目标部件
    if (m_insTintSel && m_insTintAmt) {
        QString tint;
        double amt = 0;
        const bool hasTarget = m_insRotTarget && m_insRotTarget->count() > 0;
        if (hasTarget) {
            const QStringList sl =
                m_insRotTarget->currentData().toString().split(QLatin1Char(':'));
            if (sl.size() == 2) {
                const QVector<sf::PartRef> *v = layerParts(sh, sl.at(0).toInt());
                const int k = sl.at(1).toInt();
                if (v && k >= 0 && k < v->size()) {
                    tint = v->at(k).params.value(QStringLiteral("tint")).toString();
                    amt = v->at(k).params.value(QStringLiteral("tintAmount")).toDouble(0);
                }
            }
        }
        int ci = 0;
        if (!tint.isEmpty()) {
            const int found = tintSwatches().indexOf(tint);
            ci = found > 0 ? found : 1; // 工程里是色板外的颜色：落到第一档，改一下就统一了
        }
        m_insTintSel->setCurrentIndex(ci);
        m_insTintAmt->setValue(int(std::lround(amt * 100)));
        m_insTintSel->setEnabled(hasTarget);
        m_insTintAmt->setEnabled(hasTarget);
    }

    if (m_insMeta) {
        const double d = m_project.duration();
        const int mm = int(d) / 60;
        const int ss = int(d) % 60;
        m_insMeta->setText(
            QStringLiteral("%1 · %2<br>%3:%4 · 镜头 %5 · 歌词 %6 行")
                .arg(m_project.title, m_project.artist)
                .arg(mm)
                .arg(QStringLiteral("%1").arg(ss, 2, 10, QLatin1Char('0')))
                .arg(m_project.shots.size())
                .arg(m_project.lyrics.size()));
    }

    m_insBusy = false;
}

// ── 状态栏 24px ─────────────────────────────────────────────────────────────
QWidget *MainWindow::buildStatusBar()
{
    auto *sb = new QWidget;
    sb->setAttribute(Qt::WA_StyledBackground, true);
    sb->setFixedHeight(Theme::M::StatusH);
    sb->setStyleSheet(QStringLiteral("background:%1;border-top:1px solid %2;")
                          .arg(Theme::Col::Panel2, Theme::Col::Line));

    auto *l = new QHBoxLayout(sb);
    l->setContentsMargins(11, 0, 11, 0);
    l->setSpacing(14);

    auto mono = [](const QString &html) {
        auto *e = new QLabel(html);
        e->setTextFormat(Qt::RichText);
        e->setFont(Theme::mono(Theme::F::Mono));
        e->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(Theme::Col::Tx3));
        return e;
    };

    m_kernelLabel = mono(QStringLiteral("内核未接入"));
    l->addWidget(m_kernelLabel);
    l->addWidget(mono(QStringLiteral("|")));

    m_frameLabel = new QLabel;
    m_frameLabel->setTextFormat(Qt::RichText);
    m_frameLabel->setFont(Theme::mono(Theme::F::Mono));
    m_frameLabel->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                                    .arg(Theme::Col::Tx3));
    l->addWidget(m_frameLabel);
    l->addWidget(mono(QStringLiteral("|")));

    m_resLabel = mono(QStringLiteral("预览 —"));
    l->addWidget(m_resLabel);
    l->addWidget(mono(QStringLiteral("|")));

    // 引擎计量：一帧多少毫秒、字形缓存命中率。优化前先有数
    m_engineLabel = mono(QStringLiteral("引擎 —"));
    l->addWidget(m_engineLabel);
    l->addWidget(mono(QStringLiteral("|")));

    // BPM：点「检测 BPM」后这里显示结果与置信度（自相关主峰/均值，>2 算可信）
    m_bpmLabel = mono(QStringLiteral("BPM —"));
    l->addWidget(m_bpmLabel);
    l->addWidget(mono(QStringLiteral("|")));

    l->addWidget(mono(QStringLiteral("<span style=\"color:%1\">●</span> ffmpeg 已就绪")
                          .arg(Theme::Col::Ok)));

    l->addStretch(1);
    l->addWidget(mono(QStringLiteral("自动保存 已开启")));
    l->addWidget(mono(QStringLiteral("|")));
    l->addWidget(mono(QStringLiteral("渲染队列 空闲")));

    return sb;
}

// ═════════════════════════════════════════════════════════════════════════════
// 播放
// ═════════════════════════════════════════════════════════════════════════════
void MainWindow::tick()
{
    const int fps = m_project.canvas.fps > 0 ? m_project.canvas.fps : 30;
    m_t += 1.0 / fps;
    const double dur = m_project.duration();
    if (dur > 0 && m_t > dur)
        m_t = 0.0;
    onSeek(m_t);
}

void MainWindow::onSeek(double t)
{
    m_t = t;
    if (m_preview)
        m_preview->setTime(t);
    if (m_timeline)
        m_timeline->setTime(t);
    refreshTimeCode();
    refreshInspector();
}

void MainWindow::togglePlay(bool on)
{
    m_playing = on;
    if (on)
        m_timer->start();
    else
        m_timer->stop();
    if (m_playBtn)
        m_playBtn->setText(on ? QStringLiteral("暂停") : QStringLiteral("播放"));
    if (m_timeline)
        m_timeline->setPlaying(on);
}

void MainWindow::refreshTimeCode()
{
    const int totalMs = int(m_t * 1000.0);
    const int mm = totalMs / 60000;
    const int ss = (totalMs / 1000) % 60;
    const int ms = totalMs % 1000;
    if (m_tcLabel)
        m_tcLabel->setText(QString::asprintf("%02d:%02d.%03d", mm, ss, ms));
    if (m_frameLabel) {
        const int fps = m_project.canvas.fps > 0 ? m_project.canvas.fps : 30;
        const int frame = int(m_t * fps);
        const int total = m_project.frameCount();
        m_frameLabel->setText(QStringLiteral("帧 <span style=\"color:%1\">%2</span> / %3")
                                  .arg(Theme::Col::Tx)
                                  .arg(frame)
                                  .arg(total > 0 ? total : 0));
    }
    updateShotInfo();
}

void MainWindow::updateEngineStats()
{
    if (!m_engineLabel || !m_preview)
        return;
    const EngineStats &s = m_preview->engineStats();
    if (s.frames == 0) {
        m_engineLabel->setText(QStringLiteral("引擎 <span style=\"color:%1\">—</span>")
                                   .arg(QString::fromLatin1(Theme::Col::Tx3)));
        return;
    }

    // 30fps 的预算是 33.3ms；≤16.7 说明够跑 60fps 的余量
    const char *msColor = s.lastMs > 33.4 ? Theme::Col::Warn
                          : s.lastMs <= 16.7 ? Theme::Col::Ok
                                             : Theme::Col::Tx;

    QString text = QStringLiteral("引擎 <span style=\"color:%1\">%2</span>ms")
                       .arg(QString::fromLatin1(msColor),
                            QString::number(s.lastMs, 'f', 1));
    text += QStringLiteral(" · 均 %1 · p95 %2")
                .arg(s.avgMs, 0, 'f', 1)
                .arg(s.p95Ms, 0, 'f', 1);

    if (s.glyphHitRate >= 0) {
        // lume.h 把「命中率 > 90%」写成验收线，低于就标黄
        const char *hitColor = s.glyphHitRate >= 0.9 ? Theme::Col::Ok : Theme::Col::Warn;
        text += QStringLiteral(" · 字形 <span style=\"color:%1\">%2</span>")
                    .arg(QString::fromLatin1(hitColor),
                         QString::number(s.glyphHitRate * 100.0, 'f', 0) + QStringLiteral("%"));
    }
    if (s.bgBakes > 0)
        text += QStringLiteral(" · 背景烘焙 %1").arg(s.bgBakes);

    m_engineLabel->setText(text);
}

void MainWindow::updateShotInfo()
{
    if (!m_shotInfo)
        return;
    if (m_project.shots.isEmpty()) {
        m_shotInfo->setText(QStringLiteral("镜头 —"));
        return;
    }
    const int idx = m_project.shotIndexAt(m_t);
    m_shotInfo->setText(QStringLiteral("镜头 <span style=\"color:%1\">%2</span> / %3")
                            .arg(Theme::Col::Accent,
                                 idx >= 0 ? m_project.shots[idx].id : QStringLiteral("—"))
                            .arg(m_project.shots.size()));
}

void MainWindow::autoplay()
{
    // 截图为“播放中的一帧”：先跳到有卡拉OK高亮的位置，再让时钟走起来
    // 跳到第一行歌词的中点（有卡拉OK高亮、画面信息最丰富的位置）
    if (!m_project.lyrics.isEmpty()) {
        const auto &l = m_project.lyrics.first();
        onSeek(l.start + (l.end - l.start) * 0.45);
    } else {
        onSeek(0.0);
    }
    QTimer::singleShot(900, this, [this] { togglePlay(true); });
}
