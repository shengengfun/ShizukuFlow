#include "lume.h"

#include <QFontMetricsF>
#include <QHash>
#include <QJsonArray>
#include <QLinearGradient>
#include <QList>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <list>
#include <unordered_map>

namespace lume {

// ═══════════════════════════════════════════════════════════════════════════
// 颜色
// ═══════════════════════════════════════════════════════════════════════════

namespace {

int clampByte(double v)
{
    return int(std::clamp(std::lround(v), 0L, 255L));
}

/** "255,255,255,0.6" 里的分量 → 0..255（支持 %）。 */
double parseChannel(const QString &tok)
{
    const QString t = tok.trimmed();
    if (t.endsWith(QLatin1Char('%')))
        return t.left(t.size() - 1).toDouble() * 255.0 / 100.0;
    return t.toDouble();
}

} // namespace

QColor parseColor(const QString &css, const QColor &fallback)
{
    const QString s = css.trimmed();
    if (s.isEmpty())
        return fallback;

    if (s.startsWith(QLatin1Char('#'))) {
        QString h = s.mid(1);
        if (h.size() == 3 || h.size() == 4) {
            QString e;
            for (QChar c : h)
                e += QString(c) + c;
            h = e;
        }
        if (h.size() == 6) {
            bool ok = false;
            const uint v = h.toUInt(&ok, 16);
            if (!ok)
                return fallback;
            return QColor(int((v >> 16) & 0xff), int((v >> 8) & 0xff), int(v & 0xff));
        }
        if (h.size() == 8) {
            // CSS 语义是 RRGGBBAA（Qt 原生是 AARRGGBB，所以必须自己拆）
            bool ok = false;
            const uint v = h.toUInt(&ok, 16);
            if (!ok)
                return fallback;
            return QColor(int((v >> 24) & 0xff), int((v >> 16) & 0xff), int((v >> 8) & 0xff),
                          int(v & 0xff));
        }
        return fallback;
    }

    if (s.startsWith(QStringLiteral("rgb"), Qt::CaseInsensitive)) {
        const int lp = s.indexOf(QLatin1Char('('));
        const int rp = s.lastIndexOf(QLatin1Char(')'));
        if (lp < 0 || rp <= lp)
            return fallback;
        QString body = s.mid(lp + 1, rp - lp - 1);
        body.replace(QLatin1Char('/'), QLatin1Char(','));
        const QStringList parts = body.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (parts.size() < 3)
            return fallback;
        const int r = clampByte(parseChannel(parts[0]));
        const int g = clampByte(parseChannel(parts[1]));
        const int b = clampByte(parseChannel(parts[2]));
        double a = 1.0;
        if (parts.size() >= 4) {
            const QString t = parts[3].trimmed();
            a = t.endsWith(QLatin1Char('%')) ? t.left(t.size() - 1).toDouble() / 100.0
                                             : t.toDouble();
            a = std::clamp(a, 0.0, 1.0);
        }
        return QColor(r, g, b, clampByte(a * 255.0));
    }

    const QColor named = QColor::fromString(s);
    return named.isValid() ? named : fallback;
}

QColor withAlpha(const QColor &c, double alpha)
{
    QColor o = c;
    o.setAlphaF(std::clamp(c.alphaF() * alpha, 0.0, 1.0));
    return o;
}

// ═══════════════════════════════════════════════════════════════════════════
// LRU
// ═══════════════════════════════════════════════════════════════════════════

namespace {

template <typename K, typename V> class LruCache
{
public:
    explicit LruCache(int max) : m_max(std::max(1, max)) {}

    const V *get(const K &key)
    {
        auto it = m_map.find(key);
        if (it == m_map.end()) {
            ++m_stats.misses;
            return nullptr;
        }
        ++m_stats.hits;
        m_order.splice(m_order.begin(), m_order, it->second.second);
        return &it->second.first;
    }

    void put(const K &key, const V &value)
    {
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            it->second.first = value;
            m_order.splice(m_order.begin(), m_order, it->second.second);
            return;
        }
        m_order.push_front(key);
        m_map.emplace(key, std::make_pair(value, m_order.begin()));
        while (int(m_map.size()) > m_max && !m_order.empty()) {
            const K victim = m_order.back();
            m_order.pop_back();
            m_map.erase(victim);
            ++m_stats.evictions;
        }
    }

    int size() const { return int(m_map.size()); }
    const CacheStats &stats() const { return m_stats; }
    void clear()
    {
        m_map.clear();
        m_order.clear();
    }

private:
    int m_max;
    std::list<K> m_order;
    // key → (value, 在 order 中的位置)
    std::unordered_map<K, std::pair<V, typename std::list<K>::iterator>> m_map;
    CacheStats m_stats;
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 字体解析
// ═══════════════════════════════════════════════════════════════════════════

QFont parseFont(const QString &css)
{
    static QHash<QString, QFont> cache;
    auto hit = cache.constFind(css);
    if (hit != cache.constEnd())
        return hit.value();

    int weight = 400;
    double px = 16.0;
    QString familyPart;

    static const QRegularExpression re(
        QStringLiteral(R"(^\s*(?:(\d{2,3})\s+)?([\d.]+)px\s+(.+)$)"));
    const auto m = re.match(css.trimmed());
    if (m.hasMatch()) {
        if (!m.captured(1).isEmpty())
            weight = m.captured(1).toInt();
        px = m.captured(2).toDouble();
        familyPart = m.captured(3);
    } else {
        static const QRegularExpression rePx(QStringLiteral(R"(([\d.]+)px)"));
        const auto pm = rePx.match(css);
        if (pm.hasMatch())
            px = pm.captured(1).toDouble();
        familyPart = css;
    }

    QStringList families;
    for (QString f : familyPart.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        f = f.trimmed();
        if (f.startsWith(QLatin1Char('"')) && f.endsWith(QLatin1Char('"')))
            f = f.mid(1, f.size() - 2);
        else if (f.startsWith(QLatin1Char('\'')) && f.endsWith(QLatin1Char('\'')))
            f = f.mid(1, f.size() - 2);
        f = f.trimmed();
        if (f.isEmpty())
            continue;
        const QString lower = f.toLower();
        if (lower == QStringLiteral("serif") || lower == QStringLiteral("sans-serif") ||
            lower == QStringLiteral("monospace") || lower == QStringLiteral("cursive"))
            continue;
        families << f;
    }

    QFont font;
    font.setPixelSize(std::max(1, int(std::lround(px))));
    font.setWeight(QFont::Weight(std::clamp(weight, 1, 1000)));
    if (!families.isEmpty())
        font.setFamilies(families);
    font.setStyleStrategy(QFont::PreferAntialias);

    cache.insert(css, font);
    return font;
}

// ═══════════════════════════════════════════════════════════════════════════
// 布局
// ═══════════════════════════════════════════════════════════════════════════

namespace {
LruCache<QString, TextLayoutPtr> &layoutCache()
{
    static LruCache<QString, TextLayoutPtr> c(1024);
    return c;
}

struct FontBoxMetrics
{
    double ascent = 0;
    double descent = 0;
};

FontBoxMetrics fontBox(const QString &fontCss)
{
    static LruCache<QString, FontBoxMetrics> c(256);
    if (const FontBoxMetrics *hit = c.get(fontCss))
        return *hit;
    const QFont f = parseFont(fontCss);
    const QFontMetricsF fm(f);
    FontBoxMetrics m;
    m.ascent = fm.ascent();
    m.descent = fm.descent();
    if (m.ascent <= 0 || m.descent < 0) {
        const double px = f.pixelSize();
        m.ascent = px * 0.88;
        m.descent = px * 0.12;
    }
    c.put(fontCss, m);
    return m;
}
} // namespace

TextLayoutPtr layoutRun(const QStringList &segments, const QString &font, double letterSpacing)
{
    const QString key = font + QLatin1Char('|') + QString::number(letterSpacing, 'g', 12) +
                        QLatin1Char('|') + segments.join(QChar(0x01));
    if (const TextLayoutPtr *hit = layoutCache().get(key))
        return *hit;

    const QFont f = parseFont(font);
    const QFontMetricsF fm(f);
    auto layout = std::make_shared<TextLayout>();
    layout->font = font;
    layout->letterSpacing = letterSpacing;
    double x = 0;
    layout->boxes.reserve(segments.size());
    for (const QString &text : segments) {
        GlyphBox box;
        box.text = text;
        box.x = x;
        box.width = fm.horizontalAdvance(text);
        x += box.width + letterSpacing;
        layout->boxes.push_back(box);
    }
    layout->totalWidth = segments.isEmpty() ? 0.0 : x - letterSpacing;

    layoutCache().put(key, layout);
    return layout;
}

double baselineForMiddle(const QString &font, double middleY)
{
    const FontBoxMetrics m = fontBox(font);
    return middleY + (m.ascent - m.descent) / 2.0;
}

CacheStats layoutStats()
{
    return layoutCache().stats();
}

void resetLayoutCache()
{
    layoutCache().clear();
}

// ═══════════════════════════════════════════════════════════════════════════
// 缓动
// ═══════════════════════════════════════════════════════════════════════════

namespace {

QHash<QString, EasingFn> &easingRegistry()
{
    static QHash<QString, EasingFn> r;
    return r;
}

double clamp01(double x)
{
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

double e_linear(double x)
{
    return x;
}
double e_sineInOut(double x)
{
    return -(std::cos(M_PI * x) - 1) / 2;
}
double e_quadIn(double x)
{
    return x * x;
}
double e_quadOut(double x)
{
    return 1 - (1 - x) * (1 - x);
}
double e_quadInOut(double x)
{
    return x < 0.5 ? 2 * x * x : 1 - std::pow(-2 * x + 2, 2) / 2;
}
double e_cubicIn(double x)
{
    return x * x * x;
}
double e_cubicOut(double x)
{
    return 1 - std::pow(1 - x, 3);
}
double e_cubicInOut(double x)
{
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}
double e_backOut(double x)
{
    const double c1 = 1.70158;
    const double c3 = c1 + 1;
    return 1 + c3 * std::pow(x - 1, 3) + c1 * std::pow(x - 1, 2);
}
double e_elasticOut(double x)
{
    const double c4 = (2 * M_PI) / 3;
    if (x == 0)
        return 0;
    if (x == 1)
        return 1;
    return std::pow(2, -10 * x) * std::sin((x * 10 - 0.75) * c4) + 1;
}
double e_bounceOut(double x)
{
    const double n1 = 7.5625;
    const double d1 = 2.75;
    if (x < 1 / d1)
        return n1 * x * x;
    if (x < 2 / d1) {
        x -= 1.5 / d1;
        return n1 * x * x + 0.75;
    }
    if (x < 2.5 / d1) {
        x -= 2.25 / d1;
        return n1 * x * x + 0.9375;
    }
    x -= 2.625 / d1;
    return n1 * x * x + 0.984375;
}

struct BuiltinEasings
{
    BuiltinEasings()
    {
        auto &r = easingRegistry();
        r.insert(QStringLiteral("linear"), e_linear);
        r.insert(QStringLiteral("sineInOut"), e_sineInOut);
        r.insert(QStringLiteral("quadIn"), e_quadIn);
        r.insert(QStringLiteral("quadOut"), e_quadOut);
        r.insert(QStringLiteral("quadInOut"), e_quadInOut);
        r.insert(QStringLiteral("cubicIn"), e_cubicIn);
        r.insert(QStringLiteral("cubicOut"), e_cubicOut);
        r.insert(QStringLiteral("cubicInOut"), e_cubicInOut);
        r.insert(QStringLiteral("backOut"), e_backOut);
        r.insert(QStringLiteral("elasticOut"), e_elasticOut);
        r.insert(QStringLiteral("bounceOut"), e_bounceOut);
    }
};
const BuiltinEasings g_builtinEasings;

double num(const QJsonObject &o, const char *key, double dflt)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : dflt;
}

QString str(const QJsonObject &o, const char *key, const QString &dflt)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : dflt;
}

} // namespace

void registerEasing(const QString &name, EasingFn fn)
{
    easingRegistry().insert(name, fn);
}

double ease(const QString &name, double x)
{
    auto it = easingRegistry().constFind(name);
    if (it == easingRegistry().constEnd())
        return clamp01(x);
    return (*it)(clamp01(x));
}

// ═══════════════════════════════════════════════════════════════════════════
// 逐字算子
// ═══════════════════════════════════════════════════════════════════════════

void applyOperators(const QVector<OperatorSpec> &specs, int i, int n, double t, CharState &s)
{
    for (const OperatorSpec &spec : specs) {
        if (t < spec.start || t >= spec.end)
            continue;
        const double span = spec.end - spec.start;
        const double local = span <= 0 ? 1.0 : clamp01((t - spec.start) / span);
        const QJsonObject &p = spec.params;
        const double rand = hash01(i, 1);

        if (spec.name == QLatin1String("fade")) {
            const double from = clamp01(num(p, "from", 0));
            s.alpha = s.alpha * (from + (1 - from) * local);
        } else if (spec.name == QLatin1String("popIn")) {
            const double e = ease(str(p, "ease", QStringLiteral("backOut")), local);
            const double from = num(p, "from", 0.2);
            s.scale = s.scale * (from + (1 - from) * e);
        } else if (spec.name == QLatin1String("slideUp")) {
            const double e = ease(str(p, "ease", QStringLiteral("cubicOut")), local);
            s.dy = s.dy + (1 - e) * num(p, "distance", 48);
        } else if (spec.name == QLatin1String("slideDown")) {
            const double e = ease(str(p, "ease", QStringLiteral("cubicOut")), local);
            s.dy = s.dy - (1 - e) * num(p, "distance", 48);
        } else if (spec.name == QLatin1String("typewriter")) {
            const double stagger = num(p, "stagger", 0.06);
            const double dur = num(p, "duration", 0.18);
            const double k = clamp01((t - spec.start - i * stagger) / dur);
            s.alpha = s.alpha * k;
        } else if (spec.name == QLatin1String("shake")) {
            const double amp = num(p, "amplitude", 6) * (1 - local);
            s.dx = s.dx + (rand - 0.5) * 2 * amp;
            s.dy = s.dy + (hash01(rand * 97) - 0.5) * 2 * amp;
        } else if (spec.name == QLatin1String("wave")) {
            const double amp = num(p, "amplitude", 10);
            const double freq = num(p, "frequency", 2.4);
            const double phase = num(p, "phase", 0.55) * i;
            s.dy = s.dy + std::sin(t * freq + phase) * amp;
        } else if (spec.name == QLatin1String("spinIn")) {
            const double e = ease(str(p, "ease", QStringLiteral("cubicOut")), local);
            const double turns = num(p, "turns", 0.5);
            const double dir = num(p, "direction", 1) >= 0 ? 1.0 : -1.0;
            s.rotation = s.rotation + (1 - e) * turns * M_PI * 2 * dir;
        }
        // 未知算子静默跳过：渲染永不因未知项崩溃
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 模糊（盒式三次近似高斯）
// ═══════════════════════════════════════════════════════════════════════════

namespace {

void boxBlurPass(QImage &img, int radius, bool horizontal)
{
    if (radius < 1)
        return;
    const int w = img.width();
    const int h = img.height();
    if (w <= 0 || h <= 0)
        return;
    QImage out = img.copy();
    const int len = horizontal ? w : h;
    const int lines = horizontal ? h : w;
    const int span = radius * 2 + 1;

    for (int ln = 0; ln < lines; ++ln) {
        auto at = [&](int i) -> QRgb {
            if (i < 0)
                i = 0;
            if (i >= len)
                i = len - 1;
            return horizontal ? reinterpret_cast<const QRgb *>(img.constScanLine(ln))[i]
                              : reinterpret_cast<const QRgb *>(img.constScanLine(i))[ln];
        };
        QRgb *dst = horizontal ? reinterpret_cast<QRgb *>(out.scanLine(ln)) : nullptr;
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int k = -radius; k <= radius; ++k) {
            const QRgb v = at(k);
            sa += qAlpha(v);
            sr += qRed(v);
            sg += qGreen(v);
            sb += qBlue(v);
        }
        for (int i = 0; i < len; ++i) {
            const QRgb v = qRgba(sr / span, sg / span, sb / span, sa / span);
            if (horizontal)
                dst[i] = v;
            else
                reinterpret_cast<QRgb *>(out.scanLine(i))[ln] = v;
            const QRgb add = at(i + radius + 1);
            const QRgb sub = at(i - radius);
            sa += qAlpha(add) - qAlpha(sub);
            sr += qRed(add) - qRed(sub);
            sg += qGreen(add) - qGreen(sub);
            sb += qBlue(add) - qBlue(sub);
        }
    }
    img = out;
}

void blurImage(QImage &img, double sigma)
{
    if (sigma <= 0.3)
        return;
    // 高斯 ≈ 三次盒式模糊；半径按 sigma 换算
    const int r = std::max(1, int(std::lround(sigma * 0.85)));
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurPass(img, r, true);
        boxBlurPass(img, r, false);
    }
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Renderer
// ═══════════════════════════════════════════════════════════════════════════

struct GlyphCell
{
    QImage img;
    double w = 0;
    double h = 0;
    double bl = 0; // 基线在 cell 内的 y
};

struct Renderer::Impl
{
    LruCache<QString, GlyphCell> glyphs{256};
    LruCache<QString, std::shared_ptr<QImage>> bitmaps{24};
    Stats stats;

    double padOf(const TextStyle &s) const
    {
        const double stroke = s.hasStroke ? s.strokeWidth : 0;
        double shadowPad = 0;
        if (s.hasShadow)
            shadowPad = s.shadowBlur + std::max(std::fabs(s.shadowOffX), std::fabs(s.shadowOffY));
        return std::ceil(stroke / 2 + shadowPad) + 2;
    }

    QString styleKey(const TextStyle &s) const
    {
        QString k = QStringLiteral("a%1").arg(s.alpha);
        k += s.hasFill ? QStringLiteral("|f%1").arg(s.fill.name(QColor::HexArgb))
                       : QStringLiteral("|f-");
        if (s.hasStroke)
            k += QStringLiteral("|s%1:%2").arg(s.strokeColor.name(QColor::HexArgb)).arg(s.strokeWidth);
        if (s.hasShadow)
            k += QStringLiteral("|h%1:%2:%3:%4")
                     .arg(s.shadowColor.name(QColor::HexArgb))
                     .arg(s.shadowBlur)
                     .arg(s.shadowOffX)
                     .arg(s.shadowOffY);
        return k;
    }

    GlyphCell bake(const QString &fontCss, const QString &text, const TextStyle &st)
    {
        const QFont f = parseFont(fontCss);
        const QFontMetricsF fm(f);
        const QRectF br = fm.tightBoundingRect(text);

        const double pad = padOf(st);
        double ascent = -br.top();
        double descent = br.bottom();
        double inkWidth = br.width();
        if (inkWidth <= 0.01 || br.height() <= 0.01) {
            ascent = fm.ascent();
            descent = fm.descent();
            inkWidth = fm.horizontalAdvance(text);
        }
        const double abbLeft = -br.left();

        const double w = std::max(1.0, std::ceil(inkWidth + pad * 2));
        const double h = std::max(1.0, std::ceil(ascent + descent + pad * 2));
        const double bl = ascent + pad;

        GlyphCell cell;
        cell.w = w;
        cell.h = h;
        cell.bl = bl;

        QImage img(int(w), int(h), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);

        QPainterPath glyphPath;
        glyphPath.addText(0, 0, f, text);

        const double ox = pad + abbLeft;
        const double oy = bl;

        // ── 阴影层：先单独画 + 模糊，再贴到主图（等价 Canvas shadowBlur）
        if (st.hasShadow && st.shadowBlur >= 0) {
            QImage shadowImg(int(w), int(h), QImage::Format_ARGB32_Premultiplied);
            shadowImg.fill(Qt::transparent);
            {
                QPainter sp(&shadowImg);
                sp.setRenderHint(QPainter::Antialiasing, true);
                sp.translate(ox + st.shadowOffX, oy + st.shadowOffY);
                sp.fillPath(glyphPath, st.shadowColor);
            }
            blurImage(shadowImg, st.shadowBlur * 0.5);
            QPainter p(&img);
            p.drawImage(0, 0, shadowImg);
        }

        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.translate(ox, oy);
        if (st.hasStroke && st.strokeWidth > 0) {
            QPen pen(st.strokeColor);
            pen.setWidthF(st.strokeWidth);
            pen.setJoinStyle(Qt::RoundJoin);
            pen.setCapStyle(Qt::RoundCap);
            p.strokePath(glyphPath, pen);
        }
        if (st.hasFill)
            p.fillPath(glyphPath, st.fill);
        p.end();

        cell.img = std::move(img);
        return cell;
    }

    void drawText(QPainter &p, const Draw &d)
    {
        const TextLayoutPtr &layout = d.layout;
        if (!layout)
            return;
        const QVector<GlyphBox> &boxes = layout->boxes;

        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);

        for (int i = 0; i < boxes.size(); ++i) {
            const GlyphBox &box = boxes[i];
            const CharState *st = (i < d.charStates.size()) ? &d.charStates[i] : nullptr;
            const double reveal = st ? st->reveal : 1.0;
            if (reveal <= 0)
                continue;

            TextStyle style = d.style;
            if (st && st->hasColor)
                style.fill = st->color;

            const QString key = layout->font + QLatin1Char('|') + box.text +
                                QLatin1Char('|') + styleKey(style);
            GlyphCell cell;
            if (const GlyphCell *hit = glyphs.get(key)) {
                cell = *hit;
                ++stats.glyphHits;
            } else {
                cell = bake(layout->font, box.text, style);
                glyphs.put(key, cell);
                ++stats.glyphMisses;
            }
            stats.glyphEntries = glyphs.size();

            const double dx = st ? st->dx : 0;
            const double dy = st ? st->dy : 0;
            const double cx = d.x + box.x + box.width / 2 + dx;
            const double cy = d.y - cell.bl + cell.h / 2 + dy;

            p.save();
            p.translate(cx, cy);
            if (st && st->rotation != 0)
                p.rotate(st->rotation * 180.0 / M_PI);
            const double sc = st ? st->scale : 1.0;
            if (sc != 1.0)
                p.scale(sc, sc);
            if (reveal < 1.0) {
                p.setClipRect(QRectF(-box.width / 2 - 1, -cell.h / 2 - 1,
                                     box.width * reveal + 1, cell.h + 2));
            }
            const double alpha = d.style.alpha * (st ? st->alpha : 1.0);
            if (alpha != 1.0)
                p.setOpacity(p.opacity() * std::clamp(alpha, 0.0, 1.0));
            p.drawImage(QPointF(-cell.w / 2, -cell.h / 2), cell.img);
            p.restore();
        }
    }

    void paint(QPainter &p, const std::vector<Draw> &draws, int canvasW, int canvasH)
    {
        for (const Draw &d : draws) {
            switch (d.kind) {
            case Draw::Rect: {
                p.save();
                if (d.alpha != 1.0)
                    p.setOpacity(p.opacity() * std::clamp(d.alpha, 0.0, 1.0));
                p.setPen(Qt::NoPen);
                p.setBrush(d.color);
                p.drawRect(QRectF(d.x, d.y, d.w, d.h));
                p.restore();
                break;
            }
            case Draw::Gradient: {
                const double cx = d.x + d.w / 2;
                const double cy = d.y + d.h / 2;
                const double a = d.angleDeg * M_PI / 180.0;
                const double dx = std::cos(a) * d.w;
                const double dy = std::sin(a) * d.h;
                QLinearGradient g(cx - dx / 2, cy - dy / 2, cx + dx / 2, cy + dy / 2);
                g.setColorAt(0, d.color);
                g.setColorAt(1, d.color2);
                p.save();
                if (d.alpha != 1.0)
                    p.setOpacity(p.opacity() * std::clamp(d.alpha, 0.0, 1.0));
                p.setPen(Qt::NoPen);
                p.setBrush(g);
                p.drawRect(QRectF(d.x, d.y, d.w, d.h));
                p.restore();
                break;
            }
            case Draw::Vignette: {
                const double cx = d.x + d.w / 2;
                const double cy = d.y + d.h / 2;
                const double r = std::hypot(d.w, d.h) / 2;
                QRadialGradient g(cx, cy, r);
                g.setColorAt(0, d.inner);
                g.setColorAt(0.35, d.inner);
                g.setColorAt(1, d.edge);
                p.save();
                p.setPen(Qt::NoPen);
                p.setBrush(g);
                p.drawRect(QRectF(d.x, d.y, d.w, d.h));
                p.restore();
                break;
            }
            case Draw::Circle: {
                p.save();
                if (d.alpha != 1.0)
                    p.setOpacity(p.opacity() * std::clamp(d.alpha, 0.0, 1.0));
                p.setPen(Qt::NoPen);
                p.setBrush(d.color);
                p.drawEllipse(QPointF(d.cx, d.cy), std::max(0.0, d.r), std::max(0.0, d.r));
                p.restore();
                break;
            }
            case Draw::Ring: {
                p.save();
                if (d.alpha != 1.0)
                    p.setOpacity(p.opacity() * std::clamp(d.alpha, 0.0, 1.0));
                p.setBrush(Qt::NoBrush);
                QPen pen(d.color);
                pen.setWidthF(std::max(0.1, d.width));
                p.setPen(pen);
                p.drawEllipse(QPointF(d.cx, d.cy), std::max(0.0, d.r), std::max(0.0, d.r));
                p.restore();
                break;
            }
            case Draw::Radial: {
                const double x = d.x;
                const double y = d.y;
                const double w = d.w > 0 ? d.w : canvasW;
                const double h = d.h > 0 ? d.h : canvasH;
                const double r0 = std::max(0.0, d.rInner);
                const double r1 = std::max(r0 + 0.01, d.rOuter);
                QRadialGradient g(d.cx, d.cy, r1);
                g.setColorAt(0, d.inner);
                if (r0 > 0.01)
                    g.setColorAt(std::clamp(r0 / r1, 0.0, 1.0), d.inner);
                g.setColorAt(1, d.outer);
                p.save();
                p.setPen(Qt::NoPen);
                p.setBrush(g);
                p.drawRect(QRectF(x, y, w, h));
                p.restore();
                break;
            }
            case Draw::Line: {
                p.save();
                if (d.alpha != 1.0)
                    p.setOpacity(p.opacity() * std::clamp(d.alpha, 0.0, 1.0));
                QPen pen(d.color);
                pen.setWidthF(std::max(0.1, d.width));
                if (d.dashed) {
                    QVector<qreal> pattern;
                    pattern << d.dash0 << d.dash1;
                    pen.setDashPattern(pattern);
                }
                p.setPen(pen);
                p.drawLine(QPointF(d.x1, d.y1), QPointF(d.x2, d.y2));
                p.restore();
                break;
            }
            case Draw::Transform: {
                p.save();
                p.setTransform(QTransform(d.a, d.b, d.c, d.d, d.e, d.f), true);
                if (d.alpha != 1.0)
                    p.setOpacity(p.opacity() * std::clamp(d.alpha, 0.0, 1.0));
                paint(p, d.sub, canvasW, canvasH); // 递归：嵌套组合合法
                p.restore();
                break;
            }
            case Draw::Bitmap: {
                if (!d.src || d.w <= 0 || d.h <= 0)
                    break;
                if (d.src->width() <= 0 || d.src->height() <= 0)
                    break;
                p.save();
                if (d.alpha != 1.0)
                    p.setOpacity(p.opacity() * std::clamp(d.alpha, 0.0, 1.0));
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.drawImage(QRectF(d.x, d.y, d.w, d.h), *d.src);
                p.restore();
                break;
            }
            case Draw::Text:
                drawText(p, d);
                break;
            }
        }
    }
};

Renderer::Renderer() : d(new Impl) {}

Renderer::~Renderer() = default;

void Renderer::paint(QPainter &p, const std::vector<Draw> &draws, int canvasW, int canvasH)
{
    d->paint(p, draws, canvasW, canvasH);
}

Renderer::Stats Renderer::stats() const
{
    return d->stats;
}

void Renderer::resetCaches()
{
    d->glyphs.clear();
    d->bitmaps.clear();
    d->stats = Stats{};
    resetLayoutCache();
}

} // namespace lume
