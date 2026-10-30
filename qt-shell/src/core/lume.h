/**
 * LUME · 琉明 —— C++ 后端内核（与 app/src/lume 同一份契约）
 *
 * 与 TS 版的关系：
 *   core/types.ts      → lume::Draw / CharState / TextLayout
 *   core/cache.ts      → lume::LruCache / hash01
 *   core/easing.ts     → lume::ease
 *   core/operators.ts  → lume::applyOperators
 *   text/layout.ts     → lume::layoutRun / baselineForMiddle
 *   render/canvas2d.ts → lume::Renderer（QPainter 后端）
 *
 * 红线不变：内核不懂「歌词」「镜头」，只消费 Draw 指令流；
 * 一帧 = 一条 Frame，纯数据；布局与时间无关，必须缓存。
 */
#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace lume {

// ─────────────────────────── 颜色 ───────────────────────────

/** CSS 颜色 → QColor。支持 #rgb / #rgba / #rrggbb / #rrggbbaa / rgb() / rgba() / 命名色。 */
QColor parseColor(const QString &css, const QColor &fallback = Qt::transparent);

/** 套一层 alpha（与原 hexA/withAlpha 语义一致：在颜色自身 alpha 上再乘）。 */
QColor withAlpha(const QColor &c, double alpha);

// ─────────────────────────── 布局 ───────────────────────────

struct GlyphBox
{
    QString text;
    double x = 0;     // 相对整行左缘
    double width = 0; // advance 宽
};

struct TextLayout
{
    QString font;
    double letterSpacing = 0;
    QVector<GlyphBox> boxes;
    double totalWidth = 0;
};
using TextLayoutPtr = std::shared_ptr<TextLayout>;

/** 布局一段文字（段间距 = letterSpacing）；同 (segments,font,ls) 全宇宙只测一次。 */
TextLayoutPtr layoutRun(const QStringList &segments, const QString &font, double letterSpacing);

/** middle 基线锚点 → alphabetic 基线 y。 */
double baselineForMiddle(const QString &font, double middleY);

/** CSS font 串（"900 76px "Noto Serif SC", serif"）→ QFont。像素字号取整。 */
QFont parseFont(const QString &css);

/** 布局缓存统计（验收指标：命中率 > 90%）。 */
struct CacheStats
{
    qint64 hits = 0;
    qint64 misses = 0;
    qint64 evictions = 0;
};
CacheStats layoutStats();
void resetLayoutCache();

// ─────────────────────────── 缓动 / 哈希 ───────────────────────────

using EasingFn = double (*)(double);
void registerEasing(const QString &name, EasingFn fn);
double ease(const QString &name, double x);

/** 确定性伪随机（同输入恒同输出，禁 rand）。 */
inline double hash01(double i, double seed = 0)
{
    const double x = std::sin(i * 12.9898 + seed * 78.233) * 43758.5453;
    return x - std::floor(x);
}

// ─────────────────────────── 逐字状态与算子 ───────────────────────────

struct CharState
{
    double dx = 0;
    double dy = 0;
    double scale = 1;
    double rotation = 0; // 弧度
    double alpha = 1;
    double reveal = 1;   // 0..1，卡拉OK 左→右揭示
    bool hasColor = false;
    QColor color;
};

struct TextStyle
{
    QColor fill = Qt::white;
    bool hasFill = true;
    bool hasStroke = false;
    QColor strokeColor = Qt::black;
    double strokeWidth = 0;
    bool hasShadow = false;
    QColor shadowColor = QColor(0, 0, 0, 128);
    double shadowBlur = 0;
    double shadowOffX = 0;
    double shadowOffY = 0;
    double alpha = 1;
};

struct OperatorSpec
{
    QString name;
    double start = 0;
    double end = 0;
    QJsonObject params;
};

void applyOperators(const QVector<OperatorSpec> &specs, int i, int n, double t, CharState &state);

// ─────────────────────────── Draw 指令 ───────────────────────────

struct Draw
{
    enum Kind { Text, Rect, Gradient, Vignette, Circle, Ring, Radial, Line, Transform, Bitmap };

    Kind kind = Rect;

    // 矩形类几何
    double x = 0, y = 0, w = 0, h = 0;
    // 圆类几何
    double cx = 0, cy = 0, r = 0, rInner = 0, rOuter = 0;
    // 线
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0, dash0 = 0, dash1 = 0;
    bool dashed = false;

    double width = 1;
    double alpha = 1;
    double angleDeg = 90;

    QColor color = Qt::white;
    QColor color2 = Qt::black;
    QColor inner = Qt::transparent;
    QColor outer = Qt::transparent;
    QColor edge = Qt::black; // vignette 外缘色

    // transform
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    std::vector<Draw> sub;

    // bitmap
    std::shared_ptr<QImage> src;

    // text
    TextLayoutPtr layout;
    TextStyle style;
    QVector<CharState> charStates; // 空 = 静态完整显示
};

struct Frame
{
    double t = 0;
    std::vector<Draw> draws;
};

// ─────────────────────────── 渲染后端 ───────────────────────────

/**
 * QPainter 后端：把一帧 Draw 画到目标画布。
 * 文字走字形烘焙缓存（光栅化一次 + drawImage 采样），命中后每帧成本与文字复杂度无关。
 */
class Renderer
{
public:
    Renderer();
    ~Renderer();

    void paint(QPainter &p, const std::vector<Draw> &draws, int canvasW, int canvasH);

    struct Stats
    {
        qint64 glyphHits = 0;
        qint64 glyphMisses = 0;
        int glyphEntries = 0;
        qint64 bitmapHits = 0;
        qint64 bitmapMisses = 0;
    };
    Stats stats() const;
    void resetCaches();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace lume
