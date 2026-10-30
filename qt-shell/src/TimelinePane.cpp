#include "TimelinePane.h"

#include "core/sf_audio.h"
#include "core/sf_edit.h"
#include "core/sf_project.h"
#include "theme.h"

#include <QAction>
#include <QBrush>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QMenu>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QImage>
#include <QLinearGradient>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace {

constexpr double PPS = 46.0;     // 每秒 46px（原型 zoom × 46），同时是视图缩放的默认值
constexpr double PPS_MIN = 2.0;  // 缩放下限：213s 的歌也要能「适配全片」铺满窗口
constexpr double PPS_MAX = 400.0; // Ctrl+滚轮放大的上限
constexpr double PAD = 12.0;      // 左内边距

constexpr double EDGE_HOT = 6.0;  // 块左右边缘热区宽度（px）
constexpr double DRAG_THR = 3.0;  // 拖动阈值：小于它算点击
constexpr double MIN_LEN = 0.05;  // 裁剪后块的最短时长（秒）

// 安全的区间夹取：lo > hi 时退化为 lo（避免 std::clamp 前置条件不成立）
double clampSafe(double v, double lo, double hi)
{
    return hi < lo ? lo : std::clamp(v, lo, hi);
}

// 把图片按 cover 方式裁出目标矩形的长宽比（不拉伸变形）
QRectF coverSource(const QImage &img, const QRectF &dst)
{
    const double dar = dst.width() / std::max(1.0, dst.height());
    const double iw = img.width(), ih = img.height();
    if (iw <= 0 || ih <= 0)
        return QRectF();
    if (iw / ih > dar) {
        const double w = ih * dar;
        return QRectF((iw - w) / 2.0, 0, w, ih);
    }
    const double h = iw / dar;
    return QRectF(0, (ih - h) / 2.0, iw, h);
}

// 轨道纵向布局：合计 214 = 270（.timeline）− 32（head）− 24（foot）
constexpr int ROW_BEAT = 0, H_BEAT = 28;
// 胶片条：紧贴时间尺下方（sf_dump --thumbs 出的整片缩略图）
constexpr int ROW_FILM = 28, H_FILM = 28;
constexpr int ROW_LYRIC = 56, H_LYRIC = 28;
constexpr int ROW_SHOT = 84, H_SHOT = 28;
constexpr int ROW_WAVE = 112, H_WAVE = 46;
// 部件轨：波形之下，四个层各占一条子道（上→下 = background/decor/text/overlay，
// 与渲染的绘制顺序一致）。每层一条 11px 道：够画缩略图和一行 8px 标签。
constexpr int ROW_IMAGE = 158, H_IMAGE = 44;
constexpr int LAYER_N = 4;
constexpr int H_LAYER = H_IMAGE / LAYER_N; // 11
constexpr int ROW_FX = 202, H_FX = 28;
constexpr int ROWS_H = 230; // 轨道区总高

constexpr double FILM_MIN_CELL = 6.0; // 每格缩略图在屏幕上的最小宽度（更小就只留纯色条）

constexpr double THUMB_MIN_W = 12.0; // 块宽小于它只画色块，不画缩略图

QPushButton *tlBtn(const QString &text, const char *role, int h)
{
    auto *b = new QPushButton(text);
    b->setProperty("role", QString::fromLatin1(role));
    b->setFont(Theme::ui(Theme::F::Small));
    b->setFixedHeight(h);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

/** 工程音轨的绝对路径（工程里可能存相对 .sf 的路径）。解析不出返回空串。 */
QString projectAudioPath(const sf::Project &p)
{
    if (p.audioPath.isEmpty())
        return QString();
    const QFileInfo fi(p.audioPath);
    if (fi.isAbsolute())
        return fi.exists() ? fi.absoluteFilePath() : QString();
    const QString base = p.path.isEmpty() ? QDir::currentPath() : QFileInfo(p.path).absolutePath();
    const QString cand = QDir(base).absoluteFilePath(p.audioPath);
    return cand;
}

/**
 * ffmpeg：显式环境变量 → exe 旁边/仓库里的 tools/ → PATH。
 * （与 MainWindow 里导出用的那套找法同源；这里只给波形分析用。）
 */
QString findFfmpeg()
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
    return QString(); // 空 = 让内核自己去 PATH 上找
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// 轨道画布
// ═════════════════════════════════════════════════════════════════════════════
class TimeCanvas : public QWidget
{
public:
    explicit TimeCanvas(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("tlCanvas"));
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus); // 键盘编辑：Delete / 空格
        setCursor(Qt::CrossCursor);
        // fixed 而不是 minimum：minimum 允许布局把它压扁，最后两行（图片/特效）
        // 会被裁掉。轨道区高度是确定的，不该由布局协商。
        setFixedHeight(ROWS_H);
    }

    std::function<void(double)> onSeek;
    std::function<void(const sf::EditCmd &)> onEdit;
    std::function<void(const QVector<sf::EditCmd> &, const QString &)> onBatch;
    std::function<void(const QString &, bool)> onSelect;
    std::function<void(const QString &)> onStatus;
    std::function<void()> onPlayToggle;
    std::function<void(double)> onZoom; // 缩放变了（滚轮/滑块/适配）→ 外层同步缩放控件
    bool deleteCurrent();               // Del 的统一入口：先删部件，再删镜头

    const sf::Project *m_project = nullptr;
    double m_t = 0.0;
    double m_viewStart = 0.0;
    double m_pps = PPS; // 每秒像素（Ctrl+滚轮改变）

    sf::AudioPeaks m_peaks;  // 真实波形（后台算好后由 TimelinePane 塞进来）
    bool m_showWave = true;  // 波形开关（头部按钮，默认开）
    QVector<QPair<double, QImage>> m_frames; // 胶片条缩略图，按时间升序
    bool m_showFilm = true;

    double duration() const { return m_project ? m_project->duration() : 0.0; }

    void setTime(double t)
    {
        // 只在时间真的变了才自动跟随：编辑后外层会重设同一个 t，
        // 若每次都跟随，用户刚滚轮平移出去的视图会被弹回播放头。
        const bool moved = std::fabs(t - m_t) > 1e-9;
        m_t = t;
        if (moved) {
            const double visible = (width() - 2 * PAD) / m_pps;
            if (duration() <= visible)
                m_viewStart = 0.0; // 全片都看得见，不需要跟
            else if (m_t < m_viewStart || m_t > m_viewStart + visible * 0.85)
                m_viewStart = std::max(0.0, m_t - visible * 0.45);
        }
        update();
    }

    /** 换工程时静默清掉选中（不发 selectionChanged，避免加载过程中抖动）。 */
    void clearSelection()
    {
        m_sel = Hit();
        m_hover = Hit();
        m_selGroup.clear();
        m_partSel.clear();
        m_marquee = false;
        reportStatus();
        update();
    }

    /** 底部状态条：没选中时展示部件轨手势提示，选中时展示选的是什么。 */
    void reportStatus()
    {
        if (!onStatus)
            return;
        if (!m_partSel.isEmpty()) {
            onStatus(m_partSel.size() == 1 ? QStringLiteral("部件 1 组")
                                           : QStringLiteral("部件 %1 组").arg(m_partSel.size()));
        } else if (m_sel.valid) {
            onStatus(m_sel.isShot ? QStringLiteral("镜头 %1").arg(m_sel.id)
                                  : QStringLiteral("歌词 %1").arg(m_sel.id));
        } else {
            onStatus(QString());
        }
    }

    /** 全片铺满：pps 由「可视宽度 / 时长」求得，仍受 PPS_MIN/MAX 约束。 */
    void fitAll()
    {
        const double d = duration();
        const double w = std::max(1.0, double(width()) - 2 * PAD);
        m_viewStart = 0.0;
        if (d > 1e-6)
            m_pps = clampSafe(w / d, PPS_MIN, PPS_MAX);
        m_hover = Hit(); // 缩放后旧的边缘热区坐标已失效
        if (onZoom)
            onZoom(m_pps);
        update();
    }

    /**
     * 换工程后适配全片。启动阶段窗口还没显示、控件宽度是布局中间值，
     * 这时候 fit 出来的 pps 会偏小（全片缩在左边半屏），所以等 showEvent
     * 里把这一帧事件跑完再 fit。
     */
    void requestFitAll()
    {
        m_fitPending = true;
        if (isVisible() && width() > 80) {
            m_fitPending = false;
            fitAll();
        }
    }

    /** 以播放头为锚缩放：保持播放头的屏幕横坐标不变（同 Ctrl+滚轮的锚点算法）。 */
    void zoomAtPlayhead(double factor)
    {
        if (factor <= 0.0)
            return;
        const double anchorX = xAt(m_t); // 缩放前的播放头横坐标
        const double np = std::clamp(m_pps * factor, PPS_MIN, PPS_MAX);
        if (std::fabs(np - m_pps) < 1e-9)
            return;
        m_pps = np;
        m_viewStart = std::max(0.0, m_t - (anchorX - PAD) / m_pps);
        m_hover = Hit(); // 缩放后旧的边缘热区坐标已失效
        if (onZoom)
            onZoom(m_pps);
        update();
    }

    /** 滑块/外部设定绝对缩放：锚在播放头，行为与 Ctrl+滚轮一致。 */
    void setPps(double pps)
    {
        const double np = clampSafe(pps, PPS_MIN, PPS_MAX);
        if (std::fabs(np - m_pps) < 1e-9)
            return;
        const double anchorX = xAt(m_t);
        m_pps = np;
        m_viewStart = std::max(0.0, m_t - (anchorX - PAD) / m_pps);
        m_hover = Hit();
        if (onZoom)
            onZoom(m_pps);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void leaveEvent(QEvent *) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    /** 命中的轨道。图片轨也归属镜头（图片没有自己的 id），只是落点不同。 */
    enum class Row
    {
        None,
        Lyric,
        Shot,
        Image
    };

    /** 命中结果：落在哪条轨、第几个块、是否贴着左右边缘。 */
    struct Hit
    {
        bool valid = false;
        bool isShot = false;
        Row row = Row::None;
        int index = -1;
        QString id;
        double start = 0;
        double end = 0;
        bool edgeIn = false;  // 贴左边缘（改入点）
        bool edgeOut = false; // 贴右边缘（改出点）
        int layer = -1;       // 部件轨命中的层（0..3）；-1 = 不在部件轨
        QString groupKey;     // 该层里第一个立绘的角色组键；空 = 不成组
    };

    /** 部件选中的一个单位：哪个镜头的哪一层（同层多部件视为一块，合并显示）。 */
    struct PartAddr
    {
        int shot = -1;
        int layer = 0;
    };

    enum class Drag
    {
        None,
        Seek,
        Move,
        TrimIn,
        TrimOut,
        PartLayer, // 部件块上下拖 = 换层（整组走批量命令）
        Marquee    // 部件轨空白处起拖 = 橡皮筋框选
    };

    double xAt(double t) const { return PAD + (t - m_viewStart) * m_pps; }
    double tAt(double x) const { return (x - PAD) / m_pps + m_viewStart; }
    void emitSeek(double x)
    {
        const double t = std::clamp(tAt(x), 0.0, duration());
        if (onSeek)
            onSeek(t);
    }

    /** 场景层部件（铺满画幅的背景素材）：静帧 image/imagePlate，以及视频 video
        （video 的参数是 image 的超集，时间轴上和底图同一条道）。 */
    static bool isBackdropPart(const sf::PartRef &part)
    {
        const QString &n = part.part;
        return n == QLatin1String("image") || n == QLatin1String("imagePlate") ||
               n == QLatin1String("video");
    }

    /** 视频图层：只画记号，不去解码帧做缩略图。 */
    static bool isVideoPart(const sf::PartRef &part)
    {
        return part.part == QLatin1String("video");
    }

    /** 角色立绘部件（站在画里的人）。 */
    static bool isPortraitPart(const sf::PartRef &part)
    {
        return part.part == QLatin1String("portrait");
    }

    /**
     * 遍历一个镜头四层里的全部部件（background → decor → text → overlay）。
     * 部件放哪层由渲染侧决定，时间轴这边不能只认 background：
     * 图片/立绘/视频挪到 decor 时照样要出现在轨道上。
     */
    template <typename Fn>
    static void forEachPart(const sf::Shot &s, Fn &&fn)
    {
        for (const auto &p : s.background)
            fn(p);
        for (const auto &p : s.decor)
            fn(p);
        for (const auto &p : s.text)
            fn(p);
        for (const auto &p : s.overlay)
            fn(p);
    }

    // ── 四层：名字 / 层色 / 取数组（颜色只从既有 token 里取，不新增常量） ─────────
    static QString layerName(int layer)
    {
        switch (layer) {
        case 0: return QStringLiteral("背景");
        case 1: return QStringLiteral("装饰");
        case 2: return QStringLiteral("文字");
        default: return QStringLiteral("覆盖");
        }
    }

    static const char *layerColorHex(int layer)
    {
        switch (layer) {
        case 0: return Theme::Col::Accent;
        case 1: return Theme::Col::Beat;
        case 2: return Theme::Col::Ok;
        default: return Theme::Col::Warn;
        }
    }

    static QColor layerColor(int layer, int alpha = 255)
    {
        return Theme::c(layerColorHex(layer), alpha);
    }

    /** layer: 0 background / 1 decor / 2 text / 3 overlay（顺序 = 绘制顺序）。 */
    static const QVector<sf::PartRef> &partsOf(const sf::Shot &s, int layer)
    {
        static const QVector<sf::PartRef> kEmpty;
        switch (layer) {
        case 0: return s.background;
        case 1: return s.decor;
        case 2: return s.text;
        case 3: return s.overlay;
        default: return kEmpty;
        }
    }

    /** 层号 → 命令里的层名（与渲染/工程 JSON 的键一致）。 */
    static QString layerKey(int layer)
    {
        switch (layer) {
        case 0: return QStringLiteral("background");
        case 1: return QStringLiteral("decor");
        case 2: return QStringLiteral("text");
        default: return QStringLiteral("overlay");
        }
    }

    static int layerAtY(double y)
    {
        if (y < ROW_IMAGE || y >= ROW_IMAGE + H_IMAGE)
            return -1;
        return std::clamp(int((y - ROW_IMAGE) / H_LAYER), 0, LAYER_N - 1);
    }

    static bool isPartRow(double y)
    {
        return y >= ROW_IMAGE && y < ROW_IMAGE + H_IMAGE;
    }

    /** 部件块的几何：某镜头某层在屏幕上的矩形（绘制与命中共用一套）。 */
    QRectF partBlockRect(int shotIndex, int layer) const
    {
        const auto &s = m_project->shots.at(shotIndex);
        const double x0 = xAt(s.start), x1 = xAt(s.end);
        return QRectF(x0 + 1.0, ROW_IMAGE + layer * H_LAYER + 0.5, std::max(2.0, x1 - x0 - 2.0),
                      H_LAYER - 1.0);
    }

    bool isPartSelected(int shot, int layer) const
    {
        for (const PartAddr &a : m_partSel) {
            if (a.shot == shot && a.layer == layer)
                return true;
        }
        return false;
    }

    void clearPartSelection()
    {
        if (m_partSel.isEmpty() && m_selGroup.isEmpty())
            return;
        m_partSel.clear();
        m_selGroup.clear();
        update();
    }

    /** 按层全选：该项目里所有在这一层有部件的镜头。 */
    void selectLayerEverywhere(int layer)
    {
        m_partSel.clear();
        m_selGroup.clear();
        if (m_project) {
            for (int i = 0; i < m_project->shots.size(); ++i) {
                if (!partsOf(m_project->shots.at(i), layer).isEmpty())
                    m_partSel.append(PartAddr{i, layer});
            }
        }
        reportStatus();
        update();
    }

    /** 轨道行顶 y（部件轨按整行进；子道由 layer 决定）。 */
    static int rowTop(Row r)
    {
        switch (r) {
        case Row::Lyric:
            return ROW_LYRIC;
        case Row::Image:
            return ROW_IMAGE;
        case Row::Shot:
        default:
            return ROW_SHOT;
        }
    }

    static int rowHeight(Row r)
    {
        switch (r) {
        case Row::Lyric:
            return H_LYRIC;
        case Row::Image:
            return H_IMAGE;
        case Row::Shot:
        default:
            return H_SHOT;
        }
    }

    /** 立绘的「角色组」键：解析后的绝对路径（同一角色跨镜头共用同一张 png）。 */
    QString portraitKey(const sf::PartRef &part) const
    {
        if (!isPortraitPart(part))
            return QString();
        const QString p = resolveImagePath(part.params);
        if (!p.isEmpty())
            return p;
        return part.params.value(QLatin1String("asset")).toString();
    }

    /** 某镜头里出现的全部角色组键（去重保序）。 */
    QStringList shotPortraitKeys(int index) const
    {
        QStringList out;
        if (!m_project || index < 0 || index >= m_project->shots.size())
            return out;
        forEachPart(m_project->shots[index], [&out, this](const sf::PartRef &part) {
            const QString k = portraitKey(part);
            if (!k.isEmpty() && !out.contains(k))
                out.append(k);
        });
        return out;
    }

    /** 该镜头是否属于当前选中的角色组（一组 = 一次进场，是立绘的实际编辑单位）。 */
    bool inSelectedGroup(int index) const
    {
        return !m_selGroup.isEmpty() && shotPortraitKeys(index).contains(m_selGroup);
    }

    /**
     * 一个角色组一种颜色。只从既有 token 里取（不新增颜色常量），
     * 用组键的哈希稳定选一个 —— 同一组在每次重绘、每次选中都是同色。
     */
    static QColor groupColor(const QString &key, int alpha = 235)
    {
        static const char *const palette[] = {Theme::Col::Accent, Theme::Col::Beat,
                                              Theme::Col::Ok, Theme::Col::Warn};
        const int n = int(sizeof(palette) / sizeof(palette[0]));
        // 自己滚一遍 FNV：qHash(QString) 在 Qt6 里带每进程随机种子，换次启动同一组会换色
        uint h = 2166136261u;
        for (const QChar ch : key)
            h = (h ^ uint(ch.unicode())) * 16777619u;
        return Theme::c(palette[int(h % uint(n))], alpha);
    }

    /** 立绘显示名：chara_akito.png → akito（去掉烤图前缀与扩展名）。 */
    static QString portraitLabel(const QString &absPath)
    {
        if (absPath.isEmpty())
            return QStringLiteral("立绘");
        QString n = QFileInfo(absPath).completeBaseName();
        if (n.startsWith(QLatin1String("chara_")))
            n = n.mid(6);
        return n;
    }

    /** params.path（相对 .sf 目录）或 params.asset（资源表 id）→ 绝对路径；解析不出返回空串。 */
    QString resolveImagePath(const QJsonObject &params) const
    {
        QString p = params.value(QLatin1String("path")).toString();
        if (p.isEmpty()) {
            const QString aid = params.value(QLatin1String("asset")).toString();
            if (!aid.isEmpty() && m_project)
                p = m_project->assetPath(aid);
        }
        if (p.isEmpty())
            return QString();
        if (!QFileInfo(p).isAbsolute() && m_project)
            p = QDir(QFileInfo(m_project->path).absolutePath()).absoluteFilePath(p);
        return p;
    }

    /**
     * 按绝对路径缓存的缩略图。
     * 缓存命中直接返回（QImage 隐式共享，拷贝无代价）；加载失败也缓存空图，
     * 否则时间轴每次随播放头重绘都会重新摸一遍磁盘。
     */
    QImage thumbFor(const QString &absPath)
    {
        auto it = m_thumbs.constFind(absPath);
        if (it != m_thumbs.constEnd())
            return it.value();
        QImage img(absPath);
        // 时间轴这一行只有 19px 高，原图按 256 压一次足够清晰且省内存
        if (!img.isNull() && img.height() > 256)
            img = img.scaledToHeight(256, Qt::SmoothTransformation);
        m_thumbs.insert(absPath, img);
        return img;
    }

    /** 部件块上的短标签：第一个部件的名字，多件合成一块时带 ×N。 */
    QString partCaption(int shotIndex, int layer) const;
    Hit hitTest(const QPointF &pos) const;
    void setSelection(const Hit &h);
    void selectPartsInRect(const QRectF &rect, bool add);
    void togglePart(int shot, int layer);
    void updateCursor();
    void updateDragPreview(double x);
    void commitDrag();
    void commitPartDrag();

    Hit m_sel;
    Hit m_hover;
    Hit m_press;             // 按下时命中的块（拖动全程以它为基准）
    QString m_selGroup;      // 选中的角色组键（空 = 单选镜头，不整组）
    int m_selGroupLayer = 1; // 角色组所在层（连接线画在这一道）
    QVector<PartAddr> m_partSel; // 部件选择：跨镜头保留
    Drag m_drag = Drag::None;
    bool m_pressed = false;  // 已按在块上，尚未越过拖动阈值
    QPointF m_pressPos;
    double m_pressT = 0.0;
    bool m_hasPreview = false;
    double m_prevStart = 0.0;
    double m_prevEnd = 0.0;

    bool m_marquee = false;   // 橡皮筋框选进行中
    bool m_marqueeAdd = false; // Shift：追加到已有选择
    QPointF m_marqueeFrom, m_marqueeTo;
    int m_dropLayer = -1; // 部件块拖动时的目标层（-1 = 没在拖动）
    bool m_fitPending = false; // 换工程后等控件有了真实宽度再「适配全片」一次

    QHash<QString, QImage> m_thumbs; // 绝对路径 → 缩略图（空图 = 加载失败，也缓存）
};

// 命中测试：歌词轨 / 分镜轨按块命中；部件轨按「子道 + 块」命中
auto TimeCanvas::hitTest(const QPointF &pos) const -> Hit
{
    Hit h;
    if (!m_project)
        return h;
    const double x = pos.x();

    const bool lyricRow = (pos.y() >= ROW_LYRIC && pos.y() < ROW_LYRIC + H_LYRIC);
    const bool shotRow = (pos.y() >= ROW_SHOT && pos.y() < ROW_SHOT + H_SHOT);
    const bool imageRow = isPartRow(pos.y());
    if (!lyricRow && !shotRow && !imageRow)
        return h;

    auto probe = [&](int i, const QString &id, double s, double e) {
        const double x0 = xAt(s), x1 = xAt(e);
        if (x < x0 || x > x1)
            return false;
        h.valid = true;
        h.index = i;
        h.id = id;
        h.start = s;
        h.end = e;
        h.edgeIn = (x - x0) <= EDGE_HOT;
        h.edgeOut = (x1 - x) <= EDGE_HOT;
        if (h.edgeIn && h.edgeOut) { // 块比两个热区还窄时取更近的一边
            if ((x - x0) <= (x1 - x))
                h.edgeOut = false;
            else
                h.edgeIn = false;
        }
        return true;
    };

    if (lyricRow) {
        for (int i = 0; i < m_project->lyrics.size(); ++i) {
            const auto &L = m_project->lyrics[i];
            if (probe(i, L.id, L.start, L.end)) {
                h.row = Row::Lyric;
                break;
            }
        }
        return h;
    }

    if (imageRow) {
        // 16px 一条子道，先定层再看该层有没有部件：空道 = 空白（可以起框选）
        const int layer = layerAtY(pos.y());
        // 倒序 = 绘制顺序的逆序：多轨层叠时命中最上面那一块
        for (int i = int(m_project->shots.size()) - 1; i >= 0; --i) {
            const auto &s = m_project->shots[i];
            if (partsOf(s, layer).isEmpty())
                continue;
            if (probe(i, s.id, s.start, s.end)) {
                h.row = Row::Image;
                h.layer = layer;
                // 部件块里带立绘 → 带上角色组键，点它就是选中整组（主轨同步高亮）
                for (const sf::PartRef &part : partsOf(s, layer)) {
                    if (isPortraitPart(part)) {
                        h.groupKey = portraitKey(part);
                        if (!h.groupKey.isEmpty())
                            break;
                    }
                }
                break;
            }
        }
        // 部件块永远不裁剪镜头：边缘热区在这里没有意义（拖动 = 换层）
        h.edgeIn = false;
        h.edgeOut = false;
        h.isShot = h.valid;
        return h;
    }

    for (int i = int(m_project->shots.size()) - 1; i >= 0; --i) {
        const auto &s = m_project->shots[i];
        if (probe(i, s.id, s.start, s.end)) {
            h.row = Row::Shot;
            break;
        }
    }
    h.isShot = h.valid;
    return h;
}

void TimeCanvas::setSelection(const Hit &h)
{
    // 点立绘块 = 选中整个角色组（只影响高亮，部件级操作看 m_partSel）
    const QString group = (h.valid && h.row == Row::Image) ? h.groupKey : QString();
    const bool same = (m_sel.valid == h.valid) && (m_selGroup == group) &&
                      (!h.valid || (m_sel.isShot == h.isShot && m_sel.index == h.index));
    m_sel = h;
    m_selGroup = group;
    // 点到部件轨以外（镜头/歌词/空白）就放弃部件选择：两种选择不该同时"活着"
    if (h.row != Row::Image)
        m_partSel.clear();
    if (h.valid && h.layer >= 0)
        m_selGroupLayer = h.layer;
    if (!same && onSelect)
        onSelect(h.valid ? h.id : QString(), h.isShot);
    reportStatus();
    updateCursor();
    update();
}

/** 部件块上的短标签：只取第一个能读的部件名，多件合并时补 ×N。 */
QString TimeCanvas::partCaption(int shotIndex, int layer) const
{
    if (!m_project || shotIndex < 0 || shotIndex >= m_project->shots.size())
        return QString();
    const QVector<sf::PartRef> &v = partsOf(m_project->shots.at(shotIndex), layer);
    if (v.isEmpty())
        return QString();

    QString name;
    for (const sf::PartRef &part : v) {
        if (isVideoPart(part)) {
            name = QStringLiteral("视频");
        } else if (isBackdropPart(part)) {
            const QString path = resolveImagePath(part.params);
            name = path.isEmpty() ? QStringLiteral("底图") : QFileInfo(path).completeBaseName();
        } else if (isPortraitPart(part)) {
            name = portraitLabel(resolveImagePath(part.params));
        } else {
            name = part.part;
            const QString kind = part.params.value(QLatin1String("kind")).toString();
            if (!kind.isEmpty() && kind != part.part)
                name = kind;
        }
        if (!name.isEmpty())
            break;
    }
    if (name.isEmpty())
        name = QStringLiteral("部件");
    if (v.size() > 1)
        name += QStringLiteral(" ×%1").arg(v.size());
    return name;
}

/** Shift+点击：把一块加入/移出选择（整组操作的基本手势）。 */
void TimeCanvas::togglePart(int shot, int layer)
{
    for (int i = 0; i < m_partSel.size(); ++i) {
        if (m_partSel.at(i).shot == shot && m_partSel.at(i).layer == layer) {
            m_partSel.remove(i);
            reportStatus();
            update();
            return;
        }
    }
    m_partSel.append(PartAddr{shot, layer});
    reportStatus();
    update();
}

/** 框选：与框相交的部件块全部选上；add = 保留原有选择。 */
void TimeCanvas::selectPartsInRect(const QRectF &rect, bool add)
{
    if (!m_project)
        return;
    if (!add)
        m_partSel.clear();
    const QRectF r = rect.normalized();
    for (int i = 0; i < m_project->shots.size(); ++i) {
        for (int layer = 0; layer < LAYER_N; ++layer) {
            if (partsOf(m_project->shots.at(i), layer).isEmpty())
                continue;
            if (!partBlockRect(i, layer).intersects(r))
                continue;
            if (!isPartSelected(i, layer))
                m_partSel.append(PartAddr{i, layer});
        }
    }
    reportStatus();
    update();
}

/**
 * 部件块换层：把选中的每一块（镜头 + 层）整体搬到目标层。
 * 同一块 = 该层全部部件，按原顺序追加到目标层末尾；一条批次 = 一条撤销。
 * 源层与目标层相同的块跳过（拖回原地什么都不做）。
 */
void TimeCanvas::commitPartDrag()
{
    if (!m_project || !onBatch)
        return;
    const int target = m_dropLayer;
    if (target < 0 || target >= LAYER_N)
        return;

    QVector<PartAddr> sel = m_partSel;
    if (sel.isEmpty() && m_press.valid && m_press.layer >= 0)
        sel.append(PartAddr{m_press.index, m_press.layer});

    QVector<sf::EditCmd> cmds;
    QHash<QString, int> nextIndex; // "镜头下标:目标层" → 下一个插入位置
    QVector<PartAddr> moved;
    for (const PartAddr &addr : sel) {
        if (addr.shot < 0 || addr.shot >= m_project->shots.size())
            continue;
        if (addr.layer == target)
            continue;
        const sf::Shot &s = m_project->shots.at(addr.shot);
        const QVector<sf::PartRef> &src = partsOf(s, addr.layer);
        if (src.isEmpty())
            continue;

        const QString key = QStringLiteral("%1:%2").arg(addr.shot).arg(target);
        int base = nextIndex.contains(key) ? nextIndex.value(key) : partsOf(s, target).size();

        // 先倒序删源层，再按原顺序追加到目标层末尾：顺序即绘制顺序，不能被打乱
        for (int k = int(src.size()) - 1; k >= 0; --k) {
            sf::EditCmd rm;
            rm.kind = sf::EditKind::PartRemove;
            rm.id = s.id;
            rm.layer = layerKey(addr.layer);
            rm.a = k;
            cmds.append(rm);
        }
        for (const sf::PartRef &part : src) {
            sf::EditCmd ins;
            ins.kind = sf::EditKind::PartInsert;
            ins.id = s.id;
            ins.layer = layerKey(target);
            ins.a = base++;
            ins.payload.insert(QStringLiteral("part"), part.part);
            ins.payload.insert(QStringLiteral("params"), part.params);
            ins.payload.insert(QStringLiteral("valid"), part.valid);
            cmds.append(ins);
        }
        nextIndex.insert(key, base);
        moved.append(PartAddr{addr.shot, target});
    }

    if (cmds.isEmpty()) {
        update();
        return;
    }
    onBatch(cmds, QStringLiteral("部件换到%1层").arg(layerName(target)));

    // 命令已提交：选择跟着部件落到新层，紧接着还能继续操作
    m_partSel = moved;
    m_selGroup.clear();
    reportStatus();
    update();
}

void TimeCanvas::updateCursor()
{
    if (m_drag == Drag::Move || m_drag == Drag::PartLayer) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_drag == Drag::Marquee) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (m_drag == Drag::TrimIn || m_drag == Drag::TrimOut) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    if (m_drag == Drag::PartLayer) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    const Hit &ref = m_pressed ? m_press : m_hover;
    if (ref.valid && ref.row == Row::Image)
        setCursor(Qt::OpenHandCursor); // 部件块只能上下拖（换层），抓手比十字箭头更诚实
    else if (ref.valid && (ref.edgeIn || ref.edgeOut))
        setCursor(Qt::SizeHorCursor);
    else if (ref.valid)
        setCursor(Qt::SizeAllCursor);
    else
        setCursor(Qt::CrossCursor);
}

void TimeCanvas::updateDragPreview(double x)
{
    if (!m_press.valid)
        return;
    const double origStart = m_press.start;
    const double origEnd = m_press.end;
    double s = origStart, e = origEnd;

    if (m_drag == Drag::Move) {
        double delta = tAt(x) - m_pressT;
        delta = clampSafe(delta, -origStart, duration() - origEnd);
        s = origStart + delta;
        e = origEnd + delta;
    } else if (m_drag == Drag::TrimIn) {
        s = clampSafe(tAt(x), 0.0, origEnd - MIN_LEN);
        e = origEnd;
    } else if (m_drag == Drag::TrimOut) {
        s = origStart;
        e = clampSafe(tAt(x), origStart + MIN_LEN, duration());
    } else {
        return;
    }

    m_prevStart = s;
    m_prevEnd = e;
    m_hasPreview = true;
    update();
}

void TimeCanvas::commitDrag()
{
    if (!m_hasPreview || !m_press.valid || !onEdit)
        return;
    constexpr double EPS = 1e-6;
    const bool shot = m_press.isShot;

    sf::EditCmd cmd;
    cmd.id = m_press.id;
    if (m_drag == Drag::Move) {
        if (std::fabs(m_prevStart - m_press.start) < EPS)
            return;
        cmd.kind = shot ? sf::EditKind::ShotMove : sf::EditKind::LyricMove;
        cmd.a = m_prevStart;
    } else if (m_drag == Drag::TrimIn) {
        if (std::fabs(m_prevStart - m_press.start) < EPS)
            return;
        cmd.kind = shot ? sf::EditKind::ShotTrimIn : sf::EditKind::LyricTrimIn;
        cmd.a = m_prevStart;
    } else if (m_drag == Drag::TrimOut) {
        if (std::fabs(m_prevEnd - m_press.end) < EPS)
            return;
        cmd.kind = shot ? sf::EditKind::ShotTrimOut : sf::EditKind::LyricTrimOut;
        cmd.b = m_prevEnd;
    } else {
        return;
    }
    onEdit(cmd);
}

/**
 * Del / Backspace 的统一实现：有部件选择优先删部件（每块倒序删索引，整批一条撤销），
 * 否则删选中的镜头。返回 false = 没有可删的。
 * 主窗口的「删除」QAction 也走这里：窗口级快捷键会先于控件的 keyPressEvent 命中，
 * 不接这一条的话，时间轴上按 Del 永远只会删镜头。
 */
bool TimeCanvas::deleteCurrent()
{
    if (!m_partSel.isEmpty() && onBatch) {
        QVector<sf::EditCmd> cmds;
        for (const PartAddr &addr : m_partSel) {
            if (!m_project || addr.shot < 0 || addr.shot >= m_project->shots.size())
                continue;
            const sf::Shot &s = m_project->shots.at(addr.shot);
            const int n = int(partsOf(s, addr.layer).size());
            for (int k = n - 1; k >= 0; --k) {
                sf::EditCmd cmd;
                cmd.kind = sf::EditKind::PartRemove;
                cmd.id = s.id;
                cmd.layer = layerKey(addr.layer);
                cmd.a = k;
                cmds.append(cmd);
            }
        }
        if (!cmds.isEmpty())
            onBatch(cmds, QStringLiteral("删除选中部件（%1 组）").arg(m_partSel.size()));
        m_partSel.clear();
        m_selGroup.clear();
        reportStatus();
        update();
        return true;
    }
    // 镜头：工程至少留一个（与菜单删除同一条规则，最后一条不删）
    if (m_sel.valid && m_sel.isShot && onEdit && m_project && m_project->shots.size() > 1) {
        sf::EditCmd cmd;
        cmd.kind = sf::EditKind::ShotRemove;
        cmd.id = m_sel.id;
        onEdit(cmd);
        return true;
    }
    return false;
}

void TimeCanvas::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const QPointF pos = e->position();
    const Hit h = hitTest(pos);

    // ── 部件块：部件级选择（可跨镜头多选），拖动 = 换层
    if (h.valid && h.row == Row::Image) {
        if (e->modifiers() & Qt::ControlModifier) {
            selectLayerEverywhere(h.layer); // Ctrl+点：这一层全选（跨镜头）
        } else if (e->modifiers() & Qt::ShiftModifier) {
            togglePart(h.index, h.layer);
        } else if (!isPartSelected(h.index, h.layer)) {
            m_partSel.clear();
            m_partSel.append(PartAddr{h.index, h.layer});
        }
        // 部件没有独立 id：检查器的编辑目标仍然是所属镜头
        const bool sameShot = (m_sel.valid && m_sel.isShot && m_sel.index == h.index);
        m_sel = h;
        m_selGroup = h.groupKey;
        m_selGroupLayer = h.layer;
        if (!sameShot && onSelect)
            onSelect(h.id, true);
        m_pressed = true;
        m_press = h;
        m_pressPos = pos;
        m_pressT = tAt(pos.x());
        m_drag = Drag::None;
        m_dropLayer = h.layer;
        reportStatus();
        updateCursor();
        update();
        return;
    }

    // ── 部件轨空白处：橡皮筋框选（不 Seek，播放头不动）
    if (isPartRow(pos.y())) {
        m_marquee = true;
        m_marqueeAdd = (e->modifiers() & Qt::ShiftModifier);
        m_marqueeFrom = m_marqueeTo = pos;
        if (!m_marqueeAdd) {
            m_partSel.clear();
            m_selGroup.clear();
        }
        reportStatus();
        m_drag = Drag::Marquee;
        m_pressed = false;
        updateCursor();
        update();
        return;
    }

    if (h.valid) { // 点中块：只选中，播放头不动
        if (!(m_sel.valid && m_sel.isShot == h.isShot && m_sel.index == h.index))
            setSelection(h);
        m_pressed = true;
        m_press = h;
        m_pressPos = pos;
        m_pressT = tAt(pos.x());
        m_drag = Drag::None;
        return;
    }

    // 空白处（含顶部刻度条）：取消选中，仍然 seek
    setSelection(Hit());
    m_partSel.clear();
    m_pressed = false;
    m_drag = Drag::Seek;
    emitSeek(pos.x());
}

void TimeCanvas::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF pos = e->position();

    if (m_drag == Drag::Seek) {
        emitSeek(pos.x());
        return;
    }
    if (m_drag == Drag::Marquee) {
        m_marqueeTo = pos;
        update();
        return;
    }
    if (m_drag == Drag::PartLayer) {
        const int lane = layerAtY(pos.y());
        if (lane != m_dropLayer) {
            m_dropLayer = lane;
            update();
        }
        return;
    }
    if (m_drag == Drag::Move || m_drag == Drag::TrimIn || m_drag == Drag::TrimOut) {
        updateDragPreview(pos.x());
        return;
    }

    if (m_pressed) { // 越过阈值才升级成拖动
        const QPointF d = pos - m_pressPos;
        if (std::fabs(d.x()) >= DRAG_THR || std::fabs(d.y()) >= DRAG_THR) {
            if (m_press.row == Row::Image) {
                // 部件块只有上下换层有意义：纵向没动就别进入拖动，免得横向抖一下就变「拖动」
                if (std::fabs(d.y()) < DRAG_THR)
                    return;
                m_drag = Drag::PartLayer;
                m_dropLayer = layerAtY(pos.y());
            } else {
                m_drag = m_press.edgeIn ? Drag::TrimIn
                                        : (m_press.edgeOut ? Drag::TrimOut : Drag::Move);
                updateDragPreview(pos.x());
            }
            updateCursor();
            update();
        }
        return;
    }

    const Hit nh = hitTest(pos);
    const bool changed = (nh.valid != m_hover.valid) ||
                         (nh.valid && (nh.index != m_hover.index || nh.isShot != m_hover.isShot ||
                                       nh.layer != m_hover.layer ||
                                       nh.edgeIn != m_hover.edgeIn ||
                                       nh.edgeOut != m_hover.edgeOut));
    m_hover = nh;
    updateCursor();
    if (changed)
        update();
}

void TimeCanvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    // 只有越过阈值并真的产生预览时才发命令；纯点击在这里什么都不做
    if (m_drag == Drag::Marquee) {
        selectPartsInRect(QRectF(m_marqueeFrom, m_marqueeTo), m_marqueeAdd);
        m_marquee = false;
    } else if (m_drag == Drag::PartLayer) {
        commitPartDrag();
    } else {
        commitDrag();
    }

    m_drag = Drag::None;
    m_pressed = false;
    m_hasPreview = false;
    m_marquee = false;
    m_dropLayer = -1;
    m_hover = hitTest(e->position());
    updateCursor();
    update();
}

void TimeCanvas::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Space: // 复用头部播放按钮的信号链，不另起一套
        if (!e->isAutoRepeat() && onPlayToggle)
            onPlayToggle();
        e->accept();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (deleteCurrent())
            e->accept();
        else
            QWidget::keyPressEvent(e);
        return;
    case Qt::Key_Escape: {
        // 拖动/框选/多选都算「进行中的操作」，Esc 一律退出且不落命令
        const bool dragging = (m_drag == Drag::Move || m_drag == Drag::TrimIn ||
                               m_drag == Drag::TrimOut || m_drag == Drag::PartLayer);
        if (dragging || m_marquee || !m_partSel.isEmpty()) {
            m_drag = Drag::None;
            m_pressed = false;
            m_hasPreview = false;
            m_marquee = false;
            m_dropLayer = -1;
            m_partSel.clear();
            m_selGroup.clear();
            reportStatus();
            updateCursor();
            update();
            e->accept();
            return;
        }
        break;
    }
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}

void TimeCanvas::wheelEvent(QWheelEvent *e)
{
    const double dy = e->angleDelta().y();
    if (dy == 0.0) {
        e->ignore();
        return;
    }
    if (e->modifiers() & Qt::ControlModifier) {
        // 以光标处时间点为锚：缩放前后光标下的时间不动
        const double anchorX = e->position().x();
        const double anchorT = tAt(anchorX);
        m_pps = std::clamp(m_pps * std::pow(1.15, dy / 120.0), PPS_MIN, PPS_MAX);
        m_viewStart = std::max(0.0, anchorT - (anchorX - PAD) / m_pps);
        m_hover = hitTest(e->position());
        if (onZoom)
            onZoom(m_pps);
        update();
        e->accept();
        return;
    }
    // 无修饰 = 横向平移：时间轴没有滚动条，滚轮就是浏览手段（同主流非编）
    const double visible = (width() - 2 * PAD) / m_pps;
    const double maxStart = std::max(0.0, duration() - visible);
    const double step = visible * 0.12 * (dy / 120.0);
    const double ns = clampSafe(m_viewStart - step, 0.0, maxStart);
    if (std::fabs(ns - m_viewStart) > 1e-9) {
        m_viewStart = ns;
        m_hover = hitTest(e->position());
        update();
    }
    e->accept();
}

void TimeCanvas::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    // 启动时 setProject 发生在 show 之前：把 fit 推迟到这一帧结束，
    // 等布局给出真实宽度，否则「适配全片」会按中间宽度算缩。
    if (m_fitPending) {
        m_fitPending = false;
        QTimer::singleShot(0, this, [this] { fitAll(); });
    }
}

void TimeCanvas::leaveEvent(QEvent *)
{
    if (m_hover.valid) {
        m_hover = Hit();
        update();
    }
}

/** 右键：部件块给「删除/同层全选」，镜头给「删除镜头」，空白给「适配全片」。 */
void TimeCanvas::contextMenuEvent(QContextMenuEvent *e)
{
    if (!m_project)
        return;
    const Hit h = hitTest(QPointF(e->pos()));
    QMenu menu(this);

    if (h.valid && h.row == Row::Image) {
        // 右键没点中已选块：先切到这一块（与左键点选保持一致）
        if (!isPartSelected(h.index, h.layer)) {
            m_sel = h;
            m_selGroup = h.groupKey;
            m_selGroupLayer = h.layer;
            m_partSel = {PartAddr{h.index, h.layer}};
            reportStatus();
            update();
        }
        menu.addAction(QStringLiteral("删除选中部件（%1 组）").arg(m_partSel.size()),
                       this, [this] { deleteCurrent(); });
        menu.addAction(QStringLiteral("选中该层全部镜头"), this, [this, h] {
            selectLayerEverywhere(h.layer);
            m_sel = h;
            m_selGroup = h.groupKey;
            m_selGroupLayer = h.layer;
            reportStatus();
            update();
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("取消选择"), this, [this] {
            m_partSel.clear();
            m_selGroup.clear();
            reportStatus();
            update();
        });
    } else {
        if (h.valid && h.row == Row::Shot && m_project->shots.size() > 1) {
            menu.addAction(QStringLiteral("删除镜头 %1").arg(h.id), this, [this, h] {
                sf::EditCmd c;
                c.kind = sf::EditKind::ShotRemove;
                c.id = h.id;
                if (onEdit)
                    onEdit(c);
            });
            menu.addSeparator();
        }
        menu.addAction(QStringLiteral("适配全片"), this, [this] { fitAll(); });
        if (!m_partSel.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("取消选择"), this, [this] {
                m_partSel.clear();
                m_selGroup.clear();
                reportStatus();
                update();
            });
        }
    }
    menu.exec(e->globalPos());
}

void TimeCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int W = width();
    const int H = height();

    p.fillRect(rect(), Theme::c(Theme::Col::TlBg));

    const double t0 = tAt(0);
    const double t1 = tAt(W);

    // ── 秒刻度（每 2 秒标数字）
    p.setFont(Theme::mono(9));
    for (double s = std::floor(t0); s <= t1; s += 1.0) {
        if (s < 0)
            continue;
        const double x = xAt(s) + 0.5;
        const bool major = std::fmod(s, 2.0) < 0.001;
        p.setPen(QPen(Theme::c(Theme::Col::Tx3, major ? 110 : 55), 1));
        p.drawLine(QPointF(x, 0), QPointF(x, major ? 12.0 : 7.0));
        if (major) {
            p.setPen(Theme::c(Theme::Col::Tx3, 210));
            p.drawText(QRectF(x + 3, 0, 44, 14), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(int(s)));
        }
    }

    // ── 节拍网格（小节首拍更长更亮，BPM 来自工程）
    const double bpm = m_project ? m_project->bpm : 120.0;
    const double beat = 60.0 / bpm;
    p.setPen(Qt::NoPen);
    for (double bt = std::floor(t0 / beat) * beat; bt <= t1; bt += beat) {
        if (bt < 0)
            continue;
        const double x = xAt(bt);
        const bool downbeat = (int(std::lround(bt / beat)) % 4) == 0;
        p.setBrush(Theme::c(Theme::Col::Beat, downbeat ? 120 : 48));
        const double hh = downbeat ? 9.0 : 5.0;
        p.drawRect(QRectF(x, H_BEAT - hh - 1, 1, hh));
    }

    // ── 胶片条（时间尺正下方）：整片缩略图，按当前缩放抽帧；太密就只留纯色条
    {
        const QRectF band(0, ROW_FILM, W, H_FILM);
        p.fillRect(band, Theme::c(Theme::Col::Stage));
        if (m_showFilm && m_frames.size() > 1) {
            const double step = m_frames.at(1).first - m_frames.at(0).first;
            const double cellW = (step > 1e-6 ? step : 2.0) * m_pps;
            if (cellW >= FILM_MIN_CELL) {
                const QRectF lane(0, ROW_FILM + 2, W, H_FILM - 4);
                p.save();
                p.setClipRect(lane);
                for (int i = 0; i < m_frames.size(); ++i) {
                    const double x = xAt(m_frames.at(i).first);
                    if (x + cellW < 0.0)
                        continue;
                    if (x > W)
                        break;
                    const QImage &img = m_frames.at(i).second;
                    if (img.isNull())
                        continue;
                    // 一格 = 该时刻画面按格子长宽比做的中心裁剪（16:9 竖切成胶片格）
                    const QRectF cell(std::floor(x), lane.top(), std::max(1.0, cellW - 1.0),
                                      lane.height());
                    p.drawImage(cell, img, coverSource(img, cell));
                }
                p.restore();
            }
        }
    }

    // 轨道分隔线
    p.setPen(QPen(Theme::c(Theme::Col::LineSoft), 1));
    for (int y : {ROW_FILM, ROW_LYRIC, ROW_SHOT, ROW_WAVE, ROW_IMAGE, ROW_FX})
        p.drawLine(QPointF(0, y + 0.5), QPointF(W, y + 0.5));

    // ── 歌词轨（工程数据）
    if (m_project) {
        for (int i = 0; i < m_project->lyrics.size(); ++i) {
            const auto &L = m_project->lyrics[i];
            const double x0 = xAt(L.start), x1 = xAt(L.end);
            if (x1 < 0 || x0 > W)
                continue;
            const QRectF r(x0 + 1, ROW_LYRIC + 4, std::max(2.0, x1 - x0 - 2), H_LYRIC - 9);
            const bool cur = (m_t >= L.start && m_t < L.end);
            const bool sel = (!m_sel.isShot && m_sel.index == i);
            QPainterPath path;
            path.addRoundedRect(r, 3, 3);
            p.fillPath(path, Theme::c(Theme::Col::Accent, cur ? 0x55 : 0x22));
            // 选中块：2px 强调色描边
            p.setPen(sel ? QPen(Theme::c(Theme::Col::Accent), 2)
                         : QPen(Theme::c(Theme::Col::Accent, cur ? 200 : 90), 1));
            p.drawPath(path);

            p.save();
            p.setClipPath(path);
            p.setFont(Theme::ui(10));
            p.setPen(cur ? Theme::c(Theme::Col::Tx) : Theme::c(Theme::Col::Tx, 185));
            p.drawText(r.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, L.text);
            p.restore();
        }
    }

    // ── 分镜轨（工程数据）
    if (m_project) {
        for (int i = 0; i < m_project->shots.size(); ++i) {
            const auto &s = m_project->shots[i];
            const double x0 = xAt(s.start), x1 = xAt(s.end);
            if (x1 < 0 || x0 > W)
                continue;
            const QRectF r(x0 + 1, ROW_SHOT + 4, std::max(2.0, x1 - x0 - 2), H_SHOT - 9);
            const bool cur = (m_t >= s.start && m_t < s.end);
            const bool sel = (m_sel.isShot && m_sel.index == i);
            const bool grp = inSelectedGroup(i);
            QPainterPath path;
            path.addRoundedRect(r, 3, 3);
            // 角色组选中的镜头用组色浅铺 + 2px 组色描边：主轨上也能看出「一次进场」连成一串
            p.fillPath(path, grp ? groupColor(m_selGroup, cur ? 0x3a : 0x22)
                                 : (cur ? Theme::c(Theme::Col::Accent, 0x30)
                                        : Theme::c(Theme::Col::Panel3)));
            p.setPen(grp ? QPen(groupColor(m_selGroup), 2)
                         : QPen(sel ? Theme::c(Theme::Col::Accent)
                                    : (cur ? Theme::c(Theme::Col::Accent, 185)
                                           : Theme::c(Theme::Col::Line)),
                                sel ? 2 : 1));
            p.drawPath(path);
            p.setFont(Theme::mono(9));
            p.setPen(cur ? Theme::c(Theme::Col::Accent) : Theme::c(Theme::Col::Tx3));
            p.drawText(r, Qt::AlignCenter, s.id.isEmpty() ? QString::asprintf("S%03d", i + 1)
                                                          : s.id);
        }
    }

    // ── 波形轨（真实峰值：100 桶/秒，绘制时按像素聚合 low/high，对称画）
    {
        const double cy = ROW_WAVE + H_WAVE / 2.0;
        const double amp = H_WAVE / 2.0 - 4;
        if (m_showWave && !m_peaks.isEmpty()) {
            QLinearGradient wg(0, ROW_WAVE, 0, ROW_WAVE + H_WAVE);
            wg.setColorAt(0.0, QColor(146, 124, 255, 215));
            wg.setColorAt(0.5, QColor(95, 216, 232, 205));
            wg.setColorAt(1.0, QColor(1, 183, 237, 215));
            p.setPen(QPen(QBrush(wg), 1));
            const double bps = sf::AudioPeaks::bucketsPerSecond;
            const int nb = m_peaks.low.size();
            for (int x = 0; x < W; ++x) {
                // 该像素覆盖的时间范围 → 桶范围（放大时 1 桶铺多像素，缩小时多桶并 1 像素）
                const double t0 = std::max(0.0, tAt(x));
                int i0 = int(std::floor(t0 * bps));
                if (i0 >= nb)
                    break; // 音频已放完
                int i1 = int(std::ceil(std::max(0.0, tAt(x + 1)) * bps));
                i0 = std::clamp(i0, 0, nb - 1);
                i1 = std::clamp(std::max(i1, i0 + 1), 1, nb);
                float lo = 0.f, hi = 0.f;
                for (int i = i0; i < i1; ++i) {
                    lo = std::min(lo, m_peaks.low[i]);
                    hi = std::max(hi, m_peaks.high[i]);
                }
                const double up = std::clamp(double(hi), 0.0, 1.0) * amp;
                const double dn = std::clamp(double(lo), -1.0, 0.0) * amp;
                p.drawLine(QPointF(x + 0.5, cy - up), QPointF(x + 0.5, cy - dn));
            }
        }
        p.setPen(QPen(Theme::c(Theme::Col::Beat, 45), 1));
        p.drawLine(QPointF(0, cy), QPointF(W, cy));
    }

    // ── 部件轨：背景 / 装饰 / 文字 / 覆盖 四条子道，一条子道一层
    //    每层每个镜头一块（同类多件合并），高度不随部件数增长
    if (m_project) {
        auto hatch = [&p](const QPainterPath &clip, const QRectF &rect) {
            p.save();
            p.setClipPath(clip);
            p.setPen(QPen(Theme::c(Theme::Col::Line), 1));
            for (double d = rect.left() - rect.height(); d < rect.right(); d += 6.0)
                p.drawLine(QPointF(d, rect.bottom()), QPointF(d + rect.height(), rect.top()));
            p.restore();
        };
        // 块窄的时候自动省略，太窄（只剩「…」）就干脆不画：全片缩略时不会糊成一排字
        auto label = [&p](const QPainterPath &clip, const QRectF &rect, const QString &text,
                          const QColor &color, double leftInset) {
            const int avail = int(rect.width() - leftInset - 3);
            if (avail < 8)
                return;
            p.save();
            p.setClipPath(clip);
            p.setFont(Theme::mono(9));
            p.setPen(color);
            const QString shown = p.fontMetrics().elidedText(text, Qt::ElideRight, avail);
            if (shown != QStringLiteral("…"))
                p.drawText(rect.adjusted(leftInset, 0, -3, 0),
                           Qt::AlignVCenter | Qt::AlignLeft, shown);
            p.restore();
        };
        // 视频记号：上下两排齿孔 +（无静帧时的）播放三角。纯绘制，不解码视频
        auto videoMarks = [&p](const QPainterPath &clip, const QRectF &rect, bool withPlay) {
            p.save();
            p.setClipPath(clip);
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::c(Theme::Col::Tx2, 150));
            for (double x = rect.left() + 2.5; x < rect.right() - 1.5; x += 4.0) {
                p.drawRect(QRectF(x, rect.top() + 1, 1.5, 1.5));
                p.drawRect(QRectF(x, rect.bottom() - 2.5, 1.5, 1.5));
            }
            if (withPlay) {
                const double cy = rect.center().y();
                QPolygonF tri;
                tri << QPointF(rect.left() + 4, cy - 2.5) << QPointF(rect.left() + 4, cy + 2.5)
                    << QPointF(rect.left() + 8.5, cy);
                p.setBrush(Theme::c(Theme::Col::Tx, 210));
                p.drawPolygon(tri);
            }
            p.setBrush(Qt::NoBrush);
            p.restore();
        };

        // 四层的子道底色 / 分隔线 / 左侧 3px 层色标记：不占布局宽度，不盖内容
        for (int L = 0; L < LAYER_N; ++L) {
            const QRectF lane(0, ROW_IMAGE + L * H_LAYER, W, H_LAYER);
            p.fillRect(lane, layerColor(L, (L % 2) ? 0x12 : 0x20));
            if (L > 0) {
                p.setPen(QPen(Theme::c(Theme::Col::LineSoft, 80), 1));
                p.drawLine(QPointF(0, lane.top() - 0.5), QPointF(W, lane.top() - 0.5));
            }
            p.fillRect(QRectF(0, lane.top() + 3, 3, H_LAYER - 6), layerColor(L, 180));
        }

        // 拖动换层：目标子道整条亮起来（还没提交，工程不变）
        if (m_drag == Drag::PartLayer && m_dropLayer >= 0 && m_dropLayer < LAYER_N) {
            const QRectF lane(0, ROW_IMAGE + m_dropLayer * H_LAYER, W, H_LAYER);
            p.fillRect(lane, layerColor(m_dropLayer, 0x34));
            p.setPen(QPen(layerColor(m_dropLayer, 230), 1, Qt::DashLine));
            p.drawLine(QPointF(0, lane.top() + 0.5), QPointF(W, lane.top() + 0.5));
            p.drawLine(QPointF(0, lane.bottom() - 0.5), QPointF(W, lane.bottom() - 0.5));
        }

        // 选中角色组：在各次进场的部件块之间拉一条组色虚线（块压在它上面，只在缝隙里露出）
        if (!m_selGroup.isEmpty()) {
            double gs = -1, ge = -1;
            for (int i = 0; i < m_project->shots.size(); ++i) {
                if (!inSelectedGroup(i))
                    continue;
                const auto &s = m_project->shots[i];
                gs = (gs < 0) ? s.start : std::min(gs, s.start);
                ge = (ge < 0) ? s.end : std::max(ge, s.end);
            }
            if (gs >= 0 && ge > gs) {
                const int gl = std::clamp(m_selGroupLayer, 0, LAYER_N - 1);
                const double y = ROW_IMAGE + gl * H_LAYER + H_LAYER - 2.0;
                p.setPen(QPen(groupColor(m_selGroup, 130), 1, Qt::DashLine));
                p.drawLine(QPointF(xAt(gs), y), QPointF(xAt(ge), y));
            }
        }

        // 逐镜头 × 逐层：一层一块（同类多件合并），块内给缩略图 / 视频齿孔 / 层色
        for (int i = 0; i < m_project->shots.size(); ++i) {
            const auto &s = m_project->shots[i];
            if (xAt(s.end) < 0 || xAt(s.start) > W)
                continue;
            const bool cur = (m_t >= s.start && m_t < s.end);

            for (int L = 0; L < LAYER_N; ++L) {
                const QVector<sf::PartRef> &v = partsOf(s, L);
                if (v.isEmpty())
                    continue;
                const QRectF r = partBlockRect(i, L);
                if (r.right() < 0 || r.left() > W)
                    continue;

                const bool sel = isPartSelected(i, L);
                const bool hov = (m_hover.valid && m_hover.row == Row::Image &&
                                  m_hover.index == i && m_hover.layer == L);
                QPainterPath cp;
                cp.addRoundedRect(r, 2.5, 2.5);
                p.fillPath(cp, layerColor(L, sel ? 0x66 : (hov ? 0x3c : 0x24)));

                // 缩略图只认图片/立绘；视频走齿孔，其余部件用斜纹占位（文字层留空更清爽）
                const sf::PartRef *vis = nullptr;
                bool hasVideo = false;
                for (const sf::PartRef &part : v) {
                    if (isVideoPart(part))
                        hasVideo = true;
                    else if (!vis && (isBackdropPart(part) || isPortraitPart(part)))
                        vis = &part;
                }

                double inset = 5;
                bool drewThumb = false;
                if (vis && r.width() >= THUMB_MIN_W) {
                    const QString path = resolveImagePath(vis->params);
                    const QImage img = path.isEmpty() ? QImage() : thumbFor(path);
                    if (!img.isNull()) {
                        p.save();
                        p.setClipPath(cp);
                        if (isPortraitPart(*vis)) {
                            // 立绘是竖构图：按道高缩放、贴左侧，人形轮廓才看得出来
                            const double ih = r.height() - 1;
                            const double iw = img.width() * ih / std::max(1, img.height());
                            p.drawImage(QRectF(r.left(), r.top() + 0.5, iw, ih), img);
                            inset = iw + 3;
                        } else {
                            p.drawImage(r, img, coverSource(img, r));
                            p.fillPath(cp, Theme::c(Theme::Col::Stage, 0x66));
                        }
                        p.restore();
                        drewThumb = true;
                    }
                }
                if (hasVideo) {
                    videoMarks(cp, r, !drewThumb);
                    inset = std::max(inset, 12.0);
                } else if (!drewThumb && L != 2) {
                    hatch(cp, r); // 取不到路径 / 加载失败 / 块太窄
                }

                label(cp, r, partCaption(i, L),
                      sel ? layerColor(L, 255) : Theme::c(Theme::Col::Tx, 200), inset);
                // 选中 2px 层色；hover 1.5px；平时 1px 弱色（当前镜头稍亮）
                p.setBrush(Qt::NoBrush);
                if (sel)
                    p.setPen(QPen(layerColor(L, 240), 2));
                else if (hov)
                    p.setPen(QPen(layerColor(L, 200), 1.5));
                else
                    p.setPen(QPen(layerColor(L, cur ? 150 : 95), 1));
                p.drawPath(cp);
            }
        }
        p.setBrush(Qt::NoBrush);
    }

    // ── 动效轨：转场 / 运镜 / 入场 / 停留（部件已在四层里，这里只放「怎么动」）
    if (m_project) {
        const auto tag = [](const sf::PartRef &part) {
            const QString k = part.params.value(QLatin1String("kind")).toString();
            return k.isEmpty() ? part.part : k;
        };
        for (const auto &s : m_project->shots) {
            const double x0 = xAt(s.start), x1 = xAt(s.end);
            if (x1 < 0 || x0 > W)
                continue;
            QStringList names;
            if (s.hasTransition)
                names << QStringLiteral("转·%1").arg(tag(s.transition));
            if (s.hasCamera)
                names << QStringLiteral("镜·%1").arg(tag(s.camera));
            if (s.hasEntrance)
                names << QStringLiteral("入·%1").arg(tag(s.entrance));
            if (s.hasHold)
                names << QStringLiteral("停·%1").arg(tag(s.hold));
            if (names.isEmpty())
                continue;
            QString name = names.join(QStringLiteral("+"));
            if (name.size() > 28)
                name = name.left(28) + QStringLiteral("…");
            const QRectF r(x0 + 1, ROW_FX + 4, std::max(2.0, x1 - x0 - 2), H_FX - 9);
            QPainterPath path;
            path.addRoundedRect(r, 3, 3);
            p.fillPath(path, Theme::c(Theme::Col::Warn, 0x26));
            p.setPen(QPen(Theme::c(Theme::Col::Warn, 100), 1));
            p.drawPath(path);
            p.save();
            p.setClipPath(path);
            p.setFont(Theme::mono(9));
            p.setPen(Theme::c(Theme::Col::Warn, 215));
            p.drawText(r.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, name);
            p.restore();
        }
    }

    // ── 边缘热区提示：hover 到块左右 6px 时画一条竖亮线
    if (m_hover.valid && !m_pressed && m_drag == Drag::None &&
        (m_hover.edgeIn || m_hover.edgeOut)) {
        const int row = rowTop(m_hover.row);
        const int rowH = rowHeight(m_hover.row);
        const double x = m_hover.edgeIn ? xAt(m_hover.start) : xAt(m_hover.end);
        p.setPen(QPen(Theme::c(Theme::Col::Accent, 210), 2));
        p.drawLine(QPointF(x, row + 3), QPointF(x, row + rowH - 4));
    }

    // ── 橡皮筋框选：半透明填充 + 虚线边，和拖动影子区分开
    if (m_marquee) {
        const QRectF mr = QRectF(m_marqueeFrom, m_marqueeTo).normalized();
        p.fillRect(mr, Theme::c(Theme::Col::Accent, 0x1e));
        p.setPen(QPen(Theme::c(Theme::Col::Accent, 200), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(mr);
    }

    // ── 拖动预览：只画半透明影子，松开才发 editRequested（不改工程）
    if (m_hasPreview) {
        const int row = rowTop(m_press.row);
        const int rowH = rowHeight(m_press.row);
        const double px0 = xAt(m_prevStart), px1 = xAt(m_prevEnd);
        const QRectF r(px0 + 1, row + 4, std::max(2.0, px1 - px0 - 2), rowH - 9);
        QPainterPath path;
        path.addRoundedRect(r, 3, 3);
        p.fillPath(path, Theme::c(Theme::Col::Accent, 0x40));
        p.setPen(QPen(Theme::c(Theme::Col::Accent, 210), 1, Qt::DashLine));
        p.drawPath(path);
    }

    // ── 播放头
    {
        const double x = xAt(m_t) + 0.5;
        if (x >= -2 && x <= W + 2) {
            p.setPen(QPen(Theme::c(Theme::Col::Accent), 1));
            p.drawLine(QPointF(x, 0), QPointF(x, H));
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::c(Theme::Col::Accent));
            QPolygonF tri;
            tri << QPointF(x - 4, 0) << QPointF(x + 4, 0) << QPointF(x, 6);
            p.drawPolygon(tri);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 后台波形分析任务
// 线程只往这块 shared state 里写结果；UI 线程用 QTimer 定时来取 —— 不跨线程碰 QObject，
// 面板先被销毁也不会回调到野指针（任务随 shared_ptr 自然收尾）。
// ═════════════════════════════════════════════════════════════════════════════
struct TimelinePane::PeakJob
{
    std::mutex mu;
    bool done = false;
    sf::AudioPeaks peaks;
    QString error;
};

/** 胶片条任务：线程里跑 sf_dump --thumbs 并把 png 读成 QImage，UI 线程只来取。 */
struct TimelinePane::FilmJob
{
    std::mutex mu;
    bool done = false;
    QVector<QPair<double, QImage>> frames; // t（秒）→ 缩略图，按时间升序
};

// ═════════════════════════════════════════════════════════════════════════════
// TimelinePane
// ═════════════════════════════════════════════════════════════════════════════
TimelinePane::TimelinePane(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background:%1;border:none;border-top:1px solid %2;")
                      .arg(Theme::Col::TlBg, Theme::Col::Line));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 头部工具条 32
    auto *head = new QWidget;
    head->setFixedHeight(Theme::M::TlHeadH);
    head->setAttribute(Qt::WA_StyledBackground, true);
    head->setStyleSheet(QStringLiteral("background:transparent;border:none;"
                                       "border-bottom:1px solid %1;")
                            .arg(Theme::Col::LineSoft));
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(10, 0, 10, 0);
    hl->setSpacing(6);

    m_playBtn = tlBtn(QStringLiteral("播放"), "ghost", Theme::M::BtnSmH);
    m_playBtn->setCheckable(true);
    connect(m_playBtn, &QPushButton::toggled, this, [this](bool on) {
        m_playBtn->setText(on ? QStringLiteral("暂停") : QStringLiteral("播放"));
        emit playToggled(on);
    });
    hl->addWidget(m_playBtn);

    auto *prev = tlBtn(QStringLiteral("上一镜"), "ghost", Theme::M::BtnSmH);
    auto *next = tlBtn(QStringLiteral("下一镜"), "ghost", Theme::M::BtnSmH);
    connect(prev, &QPushButton::clicked, this, [this] { emit stepShot(-1); });
    connect(next, &QPushButton::clicked, this, [this] { emit stepShot(+1); });
    hl->addWidget(prev);
    hl->addWidget(next);

    auto sep = [] {
        auto *f = new QFrame;
        f->setFixedSize(1, 18);
        f->setStyleSheet(QStringLiteral("background:%1;border:none;").arg(Theme::Col::Line));
        return f;
    };
    hl->addWidget(sep());

    for (const QString &t : {QStringLiteral("I 入点"), QStringLiteral("O 出点"),
                             QStringLiteral("K 值分割")}) {
        auto *b = tlBtn(t, "ghost", Theme::M::BtnSmH);
        b->setEnabled(false);
        hl->addWidget(b);
    }

    hl->addWidget(sep());

    m_tcLabel = new QLabel(QStringLiteral("00:12.340"));
    m_tcLabel->setFont(Theme::mono(12));
    m_tcLabel->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                                 .arg(Theme::Col::Tx));
    hl->addWidget(m_tcLabel);

    hl->addStretch(1);

    m_waveBtn = tlBtn(QStringLiteral("波形"), "ghost", Theme::M::BtnSmH);
    m_waveBtn->setCheckable(true);
    m_waveBtn->setChecked(true); // 默认开
    connect(m_waveBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_canvas) {
            m_canvas->m_showWave = on;
            m_canvas->update();
        }
    });
    hl->addWidget(m_waveBtn);

    auto *zl = new QLabel(QStringLiteral("缩放"));
    zl->setFont(Theme::ui(Theme::F::Small));
    zl->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(Theme::Col::Tx3));
    hl->addWidget(zl);

    // 缩放滑块：0..1000 对数映射到 [PPS_MIN, PPS_MAX]，两端的细调手感都均匀
    auto ppsFromSlider = [](int v) {
        return PPS_MIN * std::pow(PPS_MAX / PPS_MIN, v / 1000.0);
    };
    auto sliderFromPps = [](double pps) {
        const double t = std::log(std::clamp(pps, PPS_MIN, PPS_MAX) / PPS_MIN) /
                         std::log(PPS_MAX / PPS_MIN);
        return int(std::lround(t * 1000.0));
    };

    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setRange(0, 1000);
    m_zoomSlider->setValue(sliderFromPps(PPS));
    m_zoomSlider->setFixedWidth(96);
    m_zoomSlider->setFixedHeight(12);
    m_zoomSlider->setFocusPolicy(Qt::NoFocus);
    m_zoomSlider->setToolTip(QStringLiteral("时间轴缩放（Ctrl+滚轮也行）"));
    hl->addWidget(m_zoomSlider);

    m_zoomLabel = new QLabel(QStringLiteral("1.00×"));
    m_zoomLabel->setFont(Theme::mono(11));
    m_zoomLabel->setFixedWidth(38);
    m_zoomLabel->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                                   .arg(Theme::Col::Tx3));
    hl->addWidget(m_zoomLabel);
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this, ppsFromSlider](int v) {
        if (m_canvas)
            m_canvas->setPps(ppsFromSlider(v));
    });

    root->addWidget(head);

    // ── 轨道画布
    m_canvas = new TimeCanvas(this);
    m_canvas->m_t = 0.0;
    m_canvas->onSeek = [this](double t) { emit seeked(t); };
    m_canvas->onEdit = [this](const sf::EditCmd &cmd) { emit editRequested(cmd); };
    m_canvas->onBatch = [this](const QVector<sf::EditCmd> &cmds, const QString &label) {
        emit batchEditRequested(cmds, label);
    };
    m_canvas->onStatus = [this](const QString &text) {
        if (!m_selLabel)
            return;
        m_selLabel->setText(text.isEmpty()
                                ? QStringLiteral("部件轨：拖块换层 · 空白拖框多选 · Del 删除")
                                : QStringLiteral("选区 %1").arg(text));
    };
    m_canvas->onSelect = [this](const QString &id, bool isShot) {
        emit selectionChanged(id, isShot);
    };
    m_canvas->onPlayToggle = [this] {
        if (m_playBtn)
            m_playBtn->click(); // 复用现有播放按钮 → toggled → playToggled
    };
    // 滚轮缩放 / Ctrl+= / 适配全片都要让滑块和倍率标签跟上
    m_canvas->onZoom = [this](double pps) {
        const double t = std::log(std::clamp(pps, PPS_MIN, PPS_MAX) / PPS_MIN) /
                         std::log(PPS_MAX / PPS_MIN);
        if (m_zoomSlider) {
            const QSignalBlocker b(m_zoomSlider);
            m_zoomSlider->setValue(int(std::lround(t * 1000.0)));
        }
        if (m_zoomLabel)
            m_zoomLabel->setText(QStringLiteral("%1×").arg(pps / PPS, 0, 'f', 2));
    };
    root->addWidget(m_canvas, 1);

    // ── 波形分析结果回收：后台线程算，UI 线程定时来取（不跨线程碰控件）
    m_peakPoll = new QTimer(this);
    m_peakPoll->setInterval(120);
    connect(m_peakPoll, &QTimer::timeout, this, &TimelinePane::pollJobs);

    // ── 底部状态条 24
    auto *foot = new QWidget;
    foot->setFixedHeight(Theme::M::TlFootH);
    foot->setAttribute(Qt::WA_StyledBackground, true);
    foot->setStyleSheet(QStringLiteral("background:transparent;border:none;"
                                       "border-top:1px solid %1;")
                            .arg(Theme::Col::LineSoft));
    auto *fl = new QHBoxLayout(foot);
    fl->setContentsMargins(10, 0, 10, 0);
    fl->setSpacing(10);

    auto mono = [](const QString &html) {
        auto *e = new QLabel(html);
        e->setTextFormat(Qt::RichText);
        e->setFont(Theme::mono(Theme::F::Mono));
        e->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                             .arg(Theme::Col::Tx3));
        return e;
    };
    m_bpmLabel = mono(QStringLiteral("BPM <span style=\"color:%1\"><b>—</b></span>")
                          .arg(Theme::Col::Beat));
    fl->addWidget(m_bpmLabel);
    m_karaokeLabel = mono(QStringLiteral("歌词 0 行"));
    fl->addWidget(m_karaokeLabel);
    fl->addWidget(mono(QStringLiteral("|")));
    m_selLabel = mono(QStringLiteral("部件轨：拖块换层 · 空白拖框多选 · Del 删除"));
    m_selLabel->setToolTip(QStringLiteral("部件轨：上下拖块换层；空白处拖框多选；Shift 加选；"
                                          "Ctrl 点同一层全选；Del 删除选中部件"));
    fl->addWidget(m_selLabel);

    fl->addStretch(1);

    root->addWidget(foot);
}

void TimelinePane::setProject(const sf::Project *project)
{
    // 载入新工程时 m_project 还是同一个对象地址，只比指针判断不出「换了工程」，
    // 得看路径：换工程才清选择 + 适配全片。
    const QString path = project ? project->path : QString();
    const bool otherProject = (path != m_projPath) || (m_canvas && m_canvas->m_project != project);
    m_projPath = path;
    if (m_canvas && otherProject) {
        m_canvas->m_project = project;
        m_canvas->clearSelection(); // 换工程后旧下标无意义
        m_canvas->requestFitAll();  // 新工程先看全片（宽度没定就等 resize 补）
    } else if (m_canvas && !project) {
        m_canvas->clearSelection();
    }
    // 音轨变了才重新分析波形；BPM 一律用实测值（工程里那个 bpm 是早期估的，不参与显示）
    if (project) {
        const QString abs = projectAudioPath(*project);
        if (abs != m_audioAbs) {
            m_audioAbs = abs;
            if (m_canvas) {
                m_canvas->m_peaks = sf::AudioPeaks();
                m_canvas->update();
            }
            refreshBpmLabel(); // 先落回「—」，测出来再填
            if (!abs.isEmpty())
                kickPeaks(abs);
        }
    }
    // 换工程才重出胶片条（200+ 张渲染约 3 秒，不能跟着每次编辑跑）
    if (project && !project->path.isEmpty() && project->path != m_thumbsFor) {
        m_thumbsFor = project->path;
        if (m_canvas) {
            m_canvas->m_frames.clear();
            m_canvas->update();
        }
        kickThumbs(project->path);
    }
    if (project && m_karaokeLabel) {
        m_karaokeLabel->setText(QStringLiteral("歌词 %1 行 · 镜头 %2 组")
                                    .arg(project->lyrics.size())
                                    .arg(project->shots.size()));
    }
    update();
}

void TimelinePane::setTime(double t)
{
    if (m_canvas)
        m_canvas->setTime(t);
}

double TimelinePane::time() const
{
    return m_canvas ? m_canvas->m_t : 0.0;
}

void TimelinePane::setPlaying(bool on)
{
    if (m_playBtn && m_playBtn->isChecked() != on) {
        const QSignalBlocker b(m_playBtn);
        m_playBtn->setChecked(on);
        m_playBtn->setText(on ? QStringLiteral("暂停") : QStringLiteral("播放"));
    }
}

void TimelinePane::fitAll()
{
    if (m_canvas)
        m_canvas->fitAll();
}

bool TimelinePane::deleteSelection()
{
    return m_canvas && m_canvas->deleteCurrent();
}

void TimelinePane::reloadProjectView()
{
    if (!m_canvas)
        return;
    m_canvas->clearSelection();
    m_canvas->requestFitAll();
}

void TimelinePane::zoomBy(double factor)
{
    if (m_canvas)
        m_canvas->zoomAtPlayhead(factor);
}

double TimelinePane::pixelsPerSecond() const
{
    return m_canvas ? m_canvas->m_pps : 0.0;
}

TimelinePane::~TimelinePane()
{
    // 任务线程是 detach 的：它只持有 shared_ptr，面板先走也不会回调到野指针。
    // 这里停下轮询即可，分析结果没人收就被回收。
    if (m_peakPoll)
        m_peakPoll->stop();
}

/**
 * 起一个后台线程解码音轨（213 秒的歌约 0.3 秒）。
 * 失败（没装 ffmpeg / 文件缺失）不弹窗：波形留空、BPM 保持「—」，只在 qWarning 里留痕。
 */
void TimelinePane::kickPeaks(const QString &absAudioPath)
{
    auto job = std::make_shared<PeakJob>();
    m_peakJob = job;
    const QString ffmpeg = findFfmpeg();
    std::thread([job, absAudioPath, ffmpeg] {
        QString err;
        sf::AudioPeaks pk = sf::computePeaks(absAudioPath, ffmpeg, &err);
        std::lock_guard<std::mutex> lock(job->mu);
        job->peaks = pk;
        job->error = err;
        job->done = true;
    }).detach();
    if (m_peakPoll)
        m_peakPoll->start();
}

/**
 * 起一个后台线程调 sf_dump --thumbs 出整片缩略图，再在同一个线程里把 png 读进内存。
 * 失败（exe 不在 / 渲染崩）静默降级：胶片条就是一条纯色带，不弹窗、不拖慢打开工程。
 */
void TimelinePane::kickThumbs(const QString &projectPath)
{
    auto job = std::make_shared<FilmJob>();
    m_filmJob = job;
    const QString exe =
        QCoreApplication::applicationDirPath() + QStringLiteral("/sf_dump.exe");
    std::thread([job, projectPath, exe] {
        QVector<QPair<double, QImage>> frames;
        // 绝对路径：相对模板会落在进程 CWD（可能是仓库根），临时目录不能脏工作区
        QTemporaryDir tmp(QDir(QDir::tempPath()).filePath(QStringLiteral("sf_thumbs_XXXXXX")));
        if (!exe.isEmpty() && QFileInfo::exists(exe) && tmp.isValid()) {
            QProcess proc;
            proc.setProgram(exe);
            proc.setArguments({projectPath, QStringLiteral("--thumbs"), tmp.path(),
                               QStringLiteral("--every"), QStringLiteral("2"),
                               QStringLiteral("--cell"), QStringLiteral("192x108")});
            proc.setProcessChannelMode(QProcess::SeparateChannels);
            proc.start();
            if (proc.waitForStarted(10000) && proc.waitForFinished(180000)) {
                const QDir dir(tmp.path());
                const QStringList files =
                    dir.entryList({QStringLiteral("thumb_*.png")}, QDir::Files, QDir::Name);
                for (const QString &f : files) {
                    // 文件名自带毫秒：thumb_0002000.png → 2.0s，不用清单
                    const QString base = QFileInfo(f).completeBaseName();
                    bool ok = false;
                    const double t = base.mid(6).toDouble(&ok) / 1000.0;
                    if (!ok)
                        continue;
                    QImage img(dir.filePath(f));
                    if (!img.isNull())
                        frames.append(qMakePair(t, img));
                }
            } else {
                proc.kill();
                proc.waitForFinished(2000);
            }
        }
        std::sort(frames.begin(), frames.end(),
                  [](const QPair<double, QImage> &a, const QPair<double, QImage> &b) {
                      return a.first < b.first;
                  });
        std::lock_guard<std::mutex> lock(job->mu);
        job->frames = frames;
        job->done = true;
    }).detach();
    if (m_peakPoll)
        m_peakPoll->start();
}

void TimelinePane::pollJobs()
{
    // ── 波形
    if (m_peakJob) {
        bool done = false;
        sf::AudioPeaks peaks;
        QString error;
        {
            std::lock_guard<std::mutex> lock(m_peakJob->mu);
            done = m_peakJob->done;
            if (done) {
                peaks = m_peakJob->peaks;
                error = m_peakJob->error;
            }
        }
        if (done) {
            m_peakJob.reset();
            if (m_canvas) {
                m_canvas->m_peaks = peaks;
                m_canvas->update();
            }
            refreshBpmLabel();
            if (peaks.isEmpty() && !error.isEmpty())
                qWarning("[wave] %s", qUtf8Printable(error));
        }
    }

    // ── 胶片条
    if (m_filmJob) {
        bool done = false;
        QVector<QPair<double, QImage>> frames;
        {
            std::lock_guard<std::mutex> lock(m_filmJob->mu);
            done = m_filmJob->done;
            if (done)
                frames = m_filmJob->frames;
        }
        if (done) {
            m_filmJob.reset();
            if (m_canvas) {
                m_canvas->m_frames = frames;
                m_canvas->update();
            }
        }
    }

    if (!m_peakJob && !m_filmJob && m_peakPoll)
        m_peakPoll->stop();
}

void TimelinePane::refreshBpmLabel()
{
    if (!m_bpmLabel)
        return;
    const bool hasPeaks = m_canvas && !m_canvas->m_peaks.isEmpty();
    const sf::BeatInfo beat = hasPeaks ? sf::detectBeats(m_canvas->m_peaks) : sf::BeatInfo();
    if (beat.bpm <= 0.0) {
        // 还没测出来 / 测不出：显示「—」（工程里那个 bpm 是早期估的，不在这儿当真值用）
        m_bpmLabel->setText(QStringLiteral("BPM <span style=\"color:%1\"><b>—</b></span>")
                                .arg(Theme::Col::Beat));
        return;
    }
    m_bpmLabel->setText(
        QStringLiteral("BPM <span style=\"color:%1\"><b>%2</b></span>（首拍 %3ms · 置信 %4）")
            .arg(Theme::Col::Beat)
            .arg(beat.bpm, 0, 'f', 2)
            .arg(int(beat.offset * 1000))
            .arg(beat.confidence, 0, 'f', 2));
}
