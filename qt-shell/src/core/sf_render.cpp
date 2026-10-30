#include "core/sf_render.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QMutex>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace sf {
namespace {

constexpr double kBgRefreshSec = 0.1; // 时变图层刷新间隔（秒）
constexpr size_t kBgCacheSlots = 16;  // 实测加到 48 反而更慢：多出的常驻图会顶掉 CPU 缓存
constexpr size_t kGrainCacheSlots = 8; // 噪点独立成池，不再和背景抢那 16 个槽

struct Box
{
    double x = 0, y = 0, w = 0, h = 0;
};

inline double clamp01(double v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

inline double pnum(const QJsonObject &o, const char *k, double dflt = 0)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toDouble() : dflt;
}

inline QString pstr(const QJsonObject &o, const char *k, const QString &dflt = QString())
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isString() ? v.toString() : dflt;
}

inline bool phas(const QJsonObject &o, const char *k)
{
    return o.contains(QLatin1String(k)) && !o.value(QLatin1String(k)).isUndefined() &&
           !o.value(QLatin1String(k)).isNull();
}

inline QColor A(const QColor &c, double alpha)
{
    return lume::withAlpha(c, alpha);
}

QString fontOf(int weight, double size)
{
    return QStringLiteral("%1 %2px \"Noto Serif SC\", \"Microsoft YaHei\", serif")
        .arg(weight)
        .arg(QString::number(size, 'f', 2));
}

QString lyricFontOf(const QString &family, int weight, double size)
{
    return QStringLiteral("%1 %2px \"%3\", \"Microsoft YaHei\", serif")
        .arg(weight)
        .arg(QString::number(size, 'f', 2))
        .arg(family);
}

QStringList toChars(const QString &text)
{
    QStringList out;
    out.reserve(text.size());
    for (QChar c : text)
        out << QString(c);
    return out;
}

QVector<lume::OperatorSpec> parseOps(const QJsonValue &v)
{
    QVector<lume::OperatorSpec> out;
    for (const QJsonValue &e : v.toArray()) {
        const QJsonObject o = e.toObject();
        lume::OperatorSpec s;
        s.name = pstr(o, "name");
        s.start = pnum(o, "start", 0);
        s.end = pnum(o, "end", 0);
        s.params = o.value(QLatin1String("params")).toObject();
        if (!s.name.isEmpty())
            out.push_back(s);
    }
    return out;
}

struct BgSlot
{
    /** 共享持有：Draw::Bitmap 直接引用它（引用计数 +1），不再每帧浅拷贝一张 QImage。 */
    std::shared_ptr<QImage> img;
    qint64 bucket = -1;    // 时变图层所属的 0.1s 档位
    double renderedAt = 0; // 最近一次真正烘焙的时刻（仅诊断用）
    double lastUsed = 0;   // 最近一次被取用；淘汰按它排，而不是按烘焙时刻
};

/** 时变图层的刷新节拍：按 0.1s 分桶，而不是「距离上次查询多久」——
    后者在来回 seek 时会把同一档位反复重烘（实测 180 帧烘了 320 次）。 */
inline qint64 bgBucket(double t)
{
    return qint64(std::floor(t / kBgRefreshSec));
}

/** 烘焙后统一淘汰：超出槽数就丢最久未用的。
    while 而不是 if —— 换槽数/换 key 时可能一次超好几条。
    形参不能叫 slots：Qt 里 slots 是关键字宏，会被展开成空，报「expected primary-expression」。 */
void evictIfNeeded(std::unordered_map<QString, BgSlot> &m, size_t cap)
{
    while (m.size() > cap) {
        auto oldest = m.begin();
        for (auto i = m.begin(); i != m.end(); ++i)
            if (i->second.lastUsed < oldest->second.lastUsed)
                oldest = i;
        m.erase(oldest);
    }
}

/** 视频图层的帧缓存：抽好的帧落在磁盘目录里，内存只留最近用到的若干张。
    不把帧塞进 images 池 —— 一个 10s 的素材就有 300 张，进去就再也出不来。 */
struct VideoSlot
{
    QString dir;         // 抽帧目录（含 done 标记才算完成）
    int frameCount = 0;  // 可用帧数
    double sampleFps = 24;
    bool failed = false; // 找不到 ffmpeg / 源文件损坏：本槽静默跳过，不再重试
    std::unordered_map<int, std::shared_ptr<QImage>> frames;
    std::deque<int> lru; // 队首 = 最久未用
};

constexpr size_t kVideoFrameSlots = 24; // 内存里最多留多少张解码后的帧

struct Env
{
    const Project *p = nullptr;
    lume::Renderer *lume = nullptr;
    std::unordered_map<QString, BgSlot> *bg = nullptr;
    /** 噪点独立成池：与背景共用一张 map 时，两个池的容量会互相顶掉。 */
    std::unordered_map<QString, BgSlot> *grain = nullptr;
    /** 图片资源池：按绝对路径存原图，绘制时由 Draw 的 w/h 缩放。
        加载失败也存 nullptr，避免每帧重复读盘。 */
    std::unordered_map<QString, std::shared_ptr<QImage>> *images = nullptr;
    /** 染色后的图片缓存（路径+色+强度 → 合成结果）。 */
    std::unordered_map<QString, std::shared_ptr<QImage>> *tints = nullptr;
    /** 视频素材池（抽帧目录 + 有界帧缓存）。 */
    std::unordered_map<QString, VideoSlot> *videos = nullptr;
    qint64 *bgRenders = nullptr;
    /** 当前镜头的起点。视频部件默认「跟镜头同时开演」，靠它把绝对时间折成镜头内相对时间。 */
    double shotStart = 0;
};

/** 取某时刻所有 active 镜头，按 (track, start) 升序 —— 决定合成叠放次序。 */
QVector<const Shot *> activeShots(const Project &p, double t)
{
    QVector<const Shot *> out;
    for (const Shot &s : p.shots)
        if (t >= s.start && t < s.end)
            out << &s;
    std::sort(out.begin(), out.end(), [](const Shot *a, const Shot *b) {
        return a->track != b->track ? a->track < b->track : a->start < b->start;
    });
    return out;
}

double tSinceDownbeat(const Project &p, double t)
{
    const double bpm = p.bpm > 0 ? p.bpm : 120;
    const double bar = (4 * 60) / bpm;
    return std::fmod(t - p.offset, bar);
}

// ═══════════════════════════════════════════════════════════════════════════
// 背景图元（可烘焙进离屏图）
// ═══════════════════════════════════════════════════════════════════════════

void buildBackgroundDraws(std::vector<lume::Draw> &out, double w, double h, const QString &kindIn,
                          const QColor &base, const QColor &base2, const QColor &accent,
                          double seed, double angle, double t, double bpm)
{
    const Box lb{0, 0, w, h};
    const QString kind = kindIn.toLower();
    const double beat = (t * bpm) / 60.0;
    const double pulse = 0.5 + 0.5 * std::cos(beat * M_PI * 2);
    const auto R = [&](int i, int s = 0) { return lume::hash01(seed * 97 + i * 13.7, s); };

    const auto rect = [&](const QColor &fill) {
        lume::Draw d;
        d.kind = lume::Draw::Rect;
        d.x = lb.x;
        d.y = lb.y;
        d.w = lb.w;
        d.h = lb.h;
        d.color = fill;
        out.push_back(d);
    };
    const auto grad = [&](const QColor &c1, const QColor &c2, double deg) {
        lume::Draw d;
        d.kind = lume::Draw::Gradient;
        d.x = lb.x;
        d.y = lb.y;
        d.w = lb.w;
        d.h = lb.h;
        d.color = c1;
        d.color2 = c2;
        d.angleDeg = deg;
        out.push_back(d);
    };
    const auto blob = [&](double cx, double cy, double r, const QColor &c, double a) {
        lume::Draw d;
        d.kind = lume::Draw::Radial;
        d.cx = cx;
        d.cy = cy;
        d.rInner = 0;
        d.rOuter = std::max(1.0, r);
        d.inner = A(c, a);
        d.outer = A(c, 0);
        d.x = lb.x;
        d.y = lb.y;
        d.w = lb.w;
        d.h = lb.h;
        out.push_back(d);
    };
    const auto line = [&](double x1, double y1, double x2, double y2, double width,
                          const QColor &c, double alpha, bool dashed = false, double d0 = 0,
                          double d1 = 0) {
        lume::Draw d;
        d.kind = lume::Draw::Line;
        d.x1 = x1;
        d.y1 = y1;
        d.x2 = x2;
        d.y2 = y2;
        d.width = width;
        d.color = c;
        d.alpha = alpha;
        d.dashed = dashed;
        d.dash0 = d0;
        d.dash1 = d1;
        out.push_back(d);
    };

    if (kind == QLatin1String("gradient")) {
        grad(base, base2, angle);
        return;
    }

    if (kind == QLatin1String("aurora")) {
        grad(base, base2, 100);
        const int n = 6;
        for (int i = 0; i < n; ++i) {
            const double ph = R(i, 1) * M_PI * 2;
            const double cx = lb.x + lb.w * (0.12 + 0.76 * R(i, 2)) +
                              lb.w * 0.05 * std::sin(t * 0.11 + ph);
            const double cy = lb.y + lb.h * (0.10 + 0.78 * R(i, 3)) +
                              lb.h * 0.04 * std::cos(t * 0.09 + ph);
            const double r = lb.h * (0.32 + 0.46 * R(i, 4)) * (1 + 0.04 * pulse);
            blob(cx, cy, r, (i % 3 == 0) ? accent : base2, 0.14 + 0.10 * R(i, 5));
        }
        grad(QColor(0, 0, 0, 0), QColor(0, 0, 0, 115), 90);
        return;
    }

    if (kind == QLatin1String("bokeh")) {
        rect(base);
        const int n = 24;
        for (int i = 0; i < n; ++i) {
            const double depth = 0.25 + 0.75 * R(i, 1);
            double xf = R(i, 2) + t * 0.0035 * depth;
            xf = xf - std::floor(xf);
            double yf = R(i, 3) - t * 0.0055 * depth;
            yf = yf - std::floor(yf);
            blob(lb.x + xf * lb.w, lb.y + yf * lb.h, lb.h * (0.018 + 0.052 * depth), accent,
                 0.05 + 0.11 * R(i, 4) * depth);
        }
        return;
    }

    if (kind == QLatin1String("grid")) {
        grad(base, base2, 90);
        const double sp = lb.h / 9;
        const double off = std::fmod(t * 16 * (bpm / 120), sp);
        for (double y = lb.y - sp + off; y <= lb.y + lb.h + sp; y += sp)
            line(lb.x, y, lb.x + lb.w, y, 1.1, accent, 0.10 + 0.05 * pulse);
        const double spx = lb.w / 16;
        const double offx = std::fmod(t * 22 * (bpm / 120), spx);
        for (double x = lb.x - spx + offx; x <= lb.x + lb.w + spx; x += spx)
            line(x, lb.y, x, lb.y + lb.h, 1, accent, 0.05);
        return;
    }

    if (kind == QLatin1String("rays")) {
        rect(base);
        const double cx = lb.x + lb.w * 0.5;
        const double cy = lb.y + lb.h * 0.5;
        const double rot = t * 0.14;
        const double len = std::hypot(lb.w, lb.h) * 0.8;
        const int n = 30;
        for (int i = 0; i < n; ++i) {
            const double a = (double(i) / n) * M_PI * 2 + rot;
            const double inner = lb.h * (0.10 + 0.04 * R(i, 1));
            line(cx + std::cos(a) * inner, cy + std::sin(a) * inner, cx + std::cos(a) * len,
                 cy + std::sin(a) * len, i % 2 ? 2.5 : 8, accent, 0.045 + 0.035 * pulse);
        }
        blob(cx, cy, lb.h * 0.85, accent, 0.10 + 0.05 * pulse);
        return;
    }

    if (kind == QLatin1String("stripes")) {
        rect(base);
        const double sp = lb.h * 0.085;
        const double shift = std::fmod(t * 26, sp);
        for (int i = -14; i < 30; ++i) {
            const double x0 = lb.x + i * sp + shift;
            line(x0, lb.y, x0 + lb.h * 0.6, lb.y + lb.h, lb.h * 0.014, accent, 0.055);
        }
        grad(QColor(0, 0, 0, 0), QColor(0, 0, 0, 102), 90);
        return;
    }

    if (kind == QLatin1String("halftone")) {
        rect(base);
        const int cols = 22;
        const int rows = 13;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const double x = lb.x + ((c + 0.5) / cols) * lb.w;
                const double y = lb.y + ((r + 0.5) / rows) * lb.h;
                const double dx = (x - (lb.x + lb.w * 0.5)) / lb.w;
                const double dy = (y - (lb.y + lb.h * 0.5)) / lb.h;
                const double d = std::min(1.0, std::hypot(dx, dy) * 1.7);
                const double rad = lb.h * 0.013 * (1 - d) * (0.75 + 0.55 * pulse);
                if (rad < 0.5)
                    continue;
                lume::Draw dd;
                dd.kind = lume::Draw::Circle;
                dd.cx = x;
                dd.cy = y;
                dd.r = rad;
                dd.color = A(accent, 0.26 * (1 - d));
                out.push_back(dd);
            }
        }
        return;
    }

    if (kind == QLatin1String("scanlines")) {
        rect(base);
        const double sp = std::max(4.0, lb.h / 150);
        for (double y = lb.y; y < lb.y + lb.h; y += sp)
            line(lb.x, y, lb.x + lb.w, y, std::max(1.0, sp * 0.35), QColor(0, 0, 0), 0.16);
        const double sweepY = lb.y + std::fmod(t * 0.12, 1.0) * lb.h;
        grad(QColor(255, 255, 255, 0), A(accent, 0.06), 0);
        lume::Draw dd;
        dd.kind = lume::Draw::Rect;
        dd.x = lb.x;
        dd.y = sweepY;
        dd.w = lb.w;
        dd.h = lb.h * 0.16;
        dd.color = A(accent, 0.04);
        out.push_back(dd);
        return;
    }

    if (kind == QLatin1String("noise")) {
        rect(base);
        const double f = std::floor(t * 30);
        const int n = 260;
        for (int i = 0; i < n; ++i) {
            const double x = lb.x + lume::hash01(i * 1.7 + f * 0.37, 3) * lb.w;
            const double y = lb.y + lume::hash01(i * 2.3 + f * 0.51, 7) * lb.h;
            lume::Draw dd;
            dd.kind = lume::Draw::Circle;
            dd.cx = x;
            dd.cy = y;
            dd.r = lb.h * 0.0016;
            dd.color = A(accent, 0.07);
            out.push_back(dd);
        }
        return;
    }

    if (kind == QLatin1String("paper")) {
        const QColor c2 = base2.isValid() ? base2 : QColor("#f4eee4");
        const QColor c1 = base.isValid() ? base : QColor("#e9e2d6");
        grad(c2, c1, 110);
        const double f = std::floor(t * 24);
        for (int i = 0; i < 120; ++i) {
            const double x = lb.x + lume::hash01(i * 2.1 + f * 0.21, 11) * lb.w;
            const double y = lb.y + lume::hash01(i * 3.3 + f * 0.17, 13) * lb.h;
            lume::Draw dd;
            dd.kind = lume::Draw::Circle;
            dd.cx = x;
            dd.cy = y;
            dd.r = lb.h * 0.0014;
            dd.color = QColor(0, 0, 0, 13);
            out.push_back(dd);
        }
        lume::Draw vg;
        vg.kind = lume::Draw::Vignette;
        vg.x = lb.x;
        vg.y = lb.y;
        vg.w = lb.w;
        vg.h = lb.h;
        vg.edge = QColor(120, 100, 80, 41);
        vg.inner = QColor(120, 100, 80, 0);
        out.push_back(vg);
        return;
    }

    rect(base);
}

/** 背景部件（带离屏缓存：同参数同内容，时变图层按 kBgRefreshSec 低频刷新）。 */
void compileBackgroundPart(Env &e, std::vector<lume::Draw> &draws, const Box &box,
                           const PartRef &part, double t, double bpm)
{
    const QJsonObject &pr = part.params;
    const QString kind = pstr(pr, "kind", QStringLiteral("solid")).toLower();
    const int w = int(std::lround(box.w));
    const int h = int(std::lround(box.h));
    if (w <= 0 || h <= 0)
        return;

    const double seed = pnum(pr, "seed", 1);
    const QColor base = lume::parseColor(pstr(pr, "color", QStringLiteral("#0a0a12")),
                                         QColor("#0a0a12"));
    const QColor base2 = lume::parseColor(pstr(pr, "color2", QStringLiteral("#1a0a14")),
                                          QColor("#1a0a14"));
    const QColor accent = lume::parseColor(pstr(pr, "accent", QStringLiteral("#ffffff")),
                                           Qt::white);
    const double angle = pnum(pr, "angle", 90);
    const bool animated =
        !(kind == QLatin1String("gradient") || kind == QLatin1String("solid") ||
          kind == QLatin1String("default"));

    const QString key = QStringLiteral("bg|%1|%2|%3|%4|%5|%6|%7x%8")
                            .arg(kind)
                            .arg(seed)
                            .arg(base.name(QColor::HexArgb), base2.name(QColor::HexArgb),
                                 accent.name(QColor::HexArgb))
                            .arg(angle)
                            .arg(w)
                            .arg(h);

    auto it = e.bg->find(key);
    const qint64 bucket = bgBucket(t);
    const bool needRefresh =
        animated && (it == e.bg->end() || it->second.bucket != bucket);
    if (it == e.bg->end() || needRefresh) {
        auto img = std::make_shared<QImage>(w, h, QImage::Format_ARGB32_Premultiplied);
        img->fill(Qt::transparent);
        {
            std::vector<lume::Draw> local;
            buildBackgroundDraws(local, w, h, kind, base, base2, accent, seed, angle, t, bpm);
            QPainter gp(img.get());
            gp.setRenderHint(QPainter::Antialiasing, true);
            e.lume->paint(gp, local, w, h);
        }
        BgSlot slot;
        slot.img = std::move(img);
        slot.bucket = bucket;
        slot.renderedAt = t;
        slot.lastUsed = t;
        (*e.bg)[key] = slot;
        ++(*e.bgRenders);
        // 淘汰后再取迭代器：key 自己刚被 lastUsed 刷新过，不会被选作最久未用。
        evictIfNeeded(*e.bg, kBgCacheSlots);
        it = e.bg->find(key);
    } else {
        it->second.lastUsed = t; // 命中也要刷新 LRU 时间
    }
    if (it == e.bg->end() || !it->second.img)
        return;

    lume::Draw d;
    d.kind = lume::Draw::Bitmap;
    d.src = it->second.img;
    d.x = box.x;
    d.y = box.y;
    d.w = box.w;
    d.h = box.h;
    draws.push_back(d);
}

/** 覆盖层：vignette / grain / 其余回落到背景部件。 */
void compileOverlayPart(Env &e, std::vector<lume::Draw> &draws, const Box &box, const PartRef &part,
                        double t, double bpm);

// ═══════════════════════════════════════════════════════════════════════════
// 装饰部件
// ═══════════════════════════════════════════════════════════════════════════

void glowDot(std::vector<lume::Draw> &draws, double x, double y, double r, const QColor &color,
             double k)
{
    const double radii[3] = {r * 4.5, r * 2.4, r};
    const double alphas[3] = {0.06 + 0.05 * k, 0.14 + 0.08 * k, 0.85 + 0.15 * k};
    for (int i = 0; i < 3; ++i) {
        lume::Draw d;
        d.kind = lume::Draw::Circle;
        d.cx = x;
        d.cy = y;
        d.r = radii[i];
        d.color = color;
        d.alpha = std::clamp(alphas[i], 0.0, 1.0);
        draws.push_back(d);
    }
}

/** 下拍扩散环：每个（或每第 every 拍）从中心扩散一圈，随时间淡出。 */
void compileBeatRings(std::vector<lume::Draw> &draws, const Box &box, const PartRef &part,
                      double t, const Project &p)
{
    const QJsonObject &pr = part.params;
    const double bpm = p.bpm > 0 ? p.bpm : 120;
    const double off = p.offset;
    const double beat = 60 / bpm;
    const double every = pnum(pr, "every", 4) * beat;
    const double life = pnum(pr, "life", every);
    const double maxR = pnum(pr, "maxR", 0.75) * std::min(box.w, box.h) * 0.5;
    const QColor color = lume::parseColor(pstr(pr, "color", QStringLiteral("#ffffff")), Qt::white);
    const double width = pnum(pr, "width", 2.5);
    const double cx = box.x + box.w * pnum(pr, "x", 0.5);
    const double cy = box.y + box.h * pnum(pr, "y", 0.5);

    const double idx = std::floor((t - off) / every);
    for (int back = 0; back < 2; ++back) {
        const double t0 = off + (idx - back) * every;
        const double k = (t - t0) / life;
        if (k <= 0 || k >= 1)
            continue;
        lume::Draw d;
        d.kind = lume::Draw::Ring;
        d.cx = cx;
        d.cy = cy;
        d.r = maxR * (0.08 + 0.92 * k);
        d.width = std::max(0.5, width * (1 - k));
        d.color = color;
        d.alpha = (1 - k) * 0.7;
        draws.push_back(d);
    }
}

/** 节拍频谱条：确定性正弦 + 下拍脉冲。 */
void compileBars(std::vector<lume::Draw> &draws, const Box &box, const PartRef &part, double t,
                 const Project &p)
{
    const QJsonObject &pr = part.params;
    const int n = int(pnum(pr, "n", 48));
    const QColor color =
        lume::parseColor(pstr(pr, "color", QStringLiteral("rgba(255,255,255,0.18)")),
                         QColor(255, 255, 255, 46));
    const double maxH = pnum(pr, "maxH", 0.16) * box.h;
    const double gap = pnum(pr, "gap", 0.15) * (box.w / std::max(1, n));
    const double punch = pnum(pr, "punch", 0.5);
    const double seed = pnum(pr, "seed", 0);
    const double bw = box.w / std::max(1, n) - gap;
    const double pulse = std::exp(-8 * tSinceDownbeat(p, t)) * punch;
    for (int i = 0; i < n; ++i) {
        const double ph = lume::hash01(i * 7.13 + seed) * M_PI * 2;
        const double sp = 1.5 + lume::hash01(i * 3.7 + seed + 11) * 2.5;
        const double level =
            0.18 + 0.62 * std::fabs(std::sin(t * sp + ph)) + pulse * lume::hash01(i * 1.9 + seed + 31);
        const double hh = std::min(maxH, maxH * std::min(1.25, level));
        lume::Draw d;
        d.kind = lume::Draw::Rect;
        d.x = box.x + gap / 2 + i * (bw + gap);
        d.y = box.y + box.h - hh;
        d.w = bw;
        d.h = hh;
        d.color = color;
        draws.push_back(d);
    }
}

/** 确定性漂浮粒子（hash01 种子 → 无随机、可复现）。 */
void compileParticles(std::vector<lume::Draw> &draws, const Box &box, const PartRef &part, double t)
{
    const QJsonObject &pr = part.params;
    const int n = int(pnum(pr, "n", 40));
    const QColor color = lume::parseColor(pstr(pr, "color", QStringLiteral("#ffffff")), Qt::white);
    const double size = pnum(pr, "size", 2.5);
    const double speed = pnum(pr, "speed", 0.02);
    const double seed = pnum(pr, "seed", 0);
    for (int i = 0; i < n; ++i) {
        const double vx = (lume::hash01(i * 5.31 + seed) - 0.5) * speed;
        const double vy = -(0.008 + lume::hash01(i * 9.77 + seed + 3) * 0.03) * speed * 50;
        double xf = lume::hash01(i * 13.7 + seed) + t * vx;
        xf = xf - std::floor(xf);
        double yf = lume::hash01(i * 17.3 + seed + 7) + t * vy;
        yf = yf - std::floor(yf);
        lume::Draw d;
        d.kind = lume::Draw::Circle;
        d.cx = box.x + xf * box.w;
        d.cy = box.y + yf * box.h;
        d.r = size * (0.5 + lume::hash01(i * 2.9 + seed + 5)) * (box.h / 1080.0) * 3;
        d.color = color;
        d.alpha = 0.15 + lume::hash01(i * 23.1 + seed + 9) * 0.35;
        draws.push_back(d);
    }
}

void compileDecorFlash(std::vector<lume::Draw> &draws, const Box &box, const PartRef &part,
                       double t)
{
    const QJsonObject &pr = part.params;
    const double at = pnum(pr, "at", 0);
    const double dur = pnum(pr, "dur", 0.2);
    const double k = (t - at) / dur;
    if (k < 0 || k >= 1)
        return;
    lume::Draw d;
    d.kind = lume::Draw::Rect;
    d.x = box.x;
    d.y = box.y;
    d.w = box.w;
    d.h = box.h;
    d.color = lume::parseColor(pstr(pr, "color", QStringLiteral("#ffffff")), Qt::white);
    d.alpha = (1 - k) * pnum(pr, "maxAlpha", 1.0);
    draws.push_back(d);
}

/** 细线画框 + 角十字准星（术曲PV排版装饰）。 */
void compileFrameLines(std::vector<lume::Draw> &draws, const Box &box, const PartRef &part)
{
    const QJsonObject &pr = part.params;
    const double m = pnum(pr, "margin", 0.055);
    const QColor color =
        lume::parseColor(pstr(pr, "color", QStringLiteral("rgba(255,255,255,0.28)")),
                         QColor(255, 255, 255, 71));
    const double lw = std::max(1.0, pnum(pr, "width", 2)) * (box.h / 1080.0);
    const double cross = pnum(pr, "cross", 14) * (box.h / 1080.0);
    const double x0 = box.x + box.w * m;
    const double y0 = box.y + box.h * m;
    const double x1 = box.x + box.w * (1 - m);
    const double y1 = box.y + box.h * (1 - m);

    const auto line = [&](double x, double y, double w, double h) {
        lume::Draw d;
        d.kind = lume::Draw::Rect;
        d.x = x;
        d.y = y;
        d.w = w;
        d.h = h;
        d.color = color;
        draws.push_back(d);
    };
    line(x0, y0, x1 - x0, lw);
    line(x0, y1 - lw, x1 - x0, lw);
    line(x0, y0, lw, y1 - y0);
    line(x1 - lw, y0, lw, y1 - y0);
    const double corners[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    for (auto &c : corners) {
        line(c[0] - cross / 2, c[1] - lw / 2, cross, lw);
        line(c[0] - lw / 2, c[1] - cross / 2, lw, cross);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 静态文字
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 把 draws[from..] 整体包进「绕 (px,py) 旋转 deg 度」的仿射里。
 *
 * 描边、阴影、残影、逐字状态都是各自独立的 Draw，只能整块转 ——
 * 一个个转会在同一个锚点上叠成一坨。deg≈0 时什么都不做（不改变既有工程的表现）。
 */
void wrapRotated(std::vector<lume::Draw> &draws, size_t from, double deg, double px, double py)
{
    if (std::fabs(deg) < 1e-6 || draws.size() <= from)
        return;
    const double r = deg * 3.14159265358979323846 / 180.0;
    const double cs = std::cos(r);
    const double sn = std::sin(r);
    lume::Draw tf;
    tf.kind = lume::Draw::Transform;
    tf.a = cs;
    tf.b = sn;
    tf.c = -sn;
    tf.d = cs;
    tf.e = px - (cs * px - sn * py);
    tf.f = py - (sn * px + cs * py);
    tf.sub.assign(std::make_move_iterator(draws.begin() + std::ptrdiff_t(from)),
                  std::make_move_iterator(draws.end()));
    draws.resize(from);
    draws.push_back(std::move(tf));
}

void compileTextPart(std::vector<lume::Draw> &draws, const Box &box, const PartRef &part, double t,
                     double safe)
{
    const QJsonObject &pr = part.params;
    if (phas(pr, "showStart") && t < pnum(pr, "showStart"))
        return;
    if (phas(pr, "showEnd") && t >= pnum(pr, "showEnd"))
        return;

    const QString text = pstr(pr, "text");
    if (text.isEmpty())
        return;
    const size_t firstDraw = draws.size(); // rotate 要把这一整块整体转

    const double scale = box.h / 1080.0;
    double sizePx = pnum(pr, "size", 64) * scale;
    const int weight = int(pnum(pr, "weight", 700));
    const double spacing = pnum(pr, "spacing", 0) * scale;

    const QVector<lume::OperatorSpec> ops = parseOps(pr.value(QLatin1String("ops")));
    const bool useOps = !ops.isEmpty();
    QStringList seg = useOps ? toChars(text) : QStringList{text};

    const double maxW = box.w * 0.92 * safe;
    const bool vertical = phas(pr, "vertical") && pr.value(QLatin1String("vertical")).toBool();

    if (vertical) {
        const int chars = int(toChars(text).size());
        const double maxH = box.h * 0.9 * safe;
        if (chars * sizePx * 1.14 > maxH)
            sizePx = maxH / (chars * 1.14);
        for (int pass = 0; pass < 2; ++pass) {
            double widest = 0;
            for (const QString &c : toChars(text))
                widest = std::max(widest, lume::layoutRun({c}, fontOf(weight, sizePx), 0)->totalWidth);
            if (widest > box.w * 0.18 && widest > 0)
                sizePx *= (box.w * 0.18) / widest;
        }
    } else {
        for (int pass = 0; pass < 3; ++pass) {
            const double w = lume::layoutRun(seg, fontOf(weight, sizePx), spacing)->totalWidth;
            if (w <= maxW || w <= 0)
                break;
            sizePx *= std::max(0.2, maxW / w);
        }
    }
    sizePx = std::max(9.0 * scale, sizePx);

    const QString font = fontOf(weight, sizePx);

    lume::TextStyle style;
    style.fill = lume::parseColor(pstr(pr, "fill", QStringLiteral("#ffffff")), Qt::white);
    if (pr.contains(QLatin1String("stroke"))) {
        const QJsonObject st = pr.value(QLatin1String("stroke")).toObject();
        const QString sc = pstr(st, "color");
        if (!sc.isEmpty()) {
            style.hasStroke = true;
            style.strokeColor = lume::parseColor(sc, Qt::black);
            style.strokeWidth = pnum(st, "width", 3) * scale;
        }
    }
    if (pr.contains(QLatin1String("shadow"))) {
        const QJsonObject sh = pr.value(QLatin1String("shadow")).toObject();
        const QString sc = pstr(sh, "color");
        if (!sc.isEmpty()) {
            style.hasShadow = true;
            style.shadowColor = lume::parseColor(sc, QColor(0, 0, 0, 128));
            style.shadowBlur = pnum(sh, "blur", 8) * scale;
            const QJsonArray off = sh.value(QLatin1String("offset")).toArray();
            style.shadowOffX = (off.size() > 0 ? off.at(0).toDouble() : 0) * scale;
            style.shadowOffY = (off.size() > 1 ? off.at(1).toDouble() : 4) * scale;
        }
    }

    const QString align = pstr(pr, "align", QStringLiteral("center"));
    const double anchorX = box.x + box.w * pnum(pr, "x", 0.5);
    const double anchorY = box.y + box.h * pnum(pr, "y", 0.85);

    bool hasEcho = false;
    double echoN = 0, echoDx = 0, echoDecay = 0.4;
    if (pr.contains(QLatin1String("echo"))) {
        const QJsonObject ec = pr.value(QLatin1String("echo")).toObject();
        hasEcho = true;
        echoN = pnum(ec, "n", 1);
        echoDx = pnum(ec, "dx", 8);
        echoDecay = pnum(ec, "decay", 0.4);
    }
    const int copies = hasEcho ? int(std::clamp(echoN, 1.0, 6.0)) : 1;

    if (vertical) {
        const QStringList chars = toChars(text);
        const double lh = sizePx * 1.14;
        for (int i = 0; i < chars.size(); ++i) {
            const auto one = lume::layoutRun({chars[i]}, font, 0);
            const double x = anchorX - one->totalWidth / 2;
            const double ym = anchorY + lh * i + lh / 2;
            for (int c = copies - 1; c >= 0; --c) {
                lume::TextStyle st = style;
                if (c > 0 && hasEcho)
                    st.alpha = std::pow(echoDecay, c);
                lume::Draw d;
                d.kind = lume::Draw::Text;
                d.layout = one;
                d.x = x;
                d.y = lume::baselineForMiddle(font, ym);
                d.style = st;
                if (useOps) {
                    lume::CharState cs;
                    lume::applyOperators(ops, i, chars.size(), t, cs);
                    if (c > 0 && hasEcho)
                        cs.alpha = cs.alpha * st.alpha;
                    d.charStates.push_back(cs);
                }
                draws.push_back(d);
            }
        }
        wrapRotated(draws, firstDraw, pnum(pr, "rotate", 0), anchorX, anchorY);
        return;
    }

    const auto layout = lume::layoutRun(seg, font, spacing);

    double x = anchorX - layout->totalWidth / 2;
    if (align == QLatin1String("left"))
        x = anchorX;
    else if (align == QLatin1String("right"))
        x = anchorX - layout->totalWidth;
    x = std::min(std::max(x, box.x + box.w * 0.02),
                 std::max(box.x + box.w * 0.02, box.x + box.w * 0.98 - layout->totalWidth));

    QVector<lume::CharState> charStates;
    if (useOps) {
        charStates.reserve(seg.size());
        for (int i = 0; i < seg.size(); ++i) {
            lume::CharState cs;
            lume::applyOperators(ops, i, seg.size(), t, cs);
            charStates.push_back(cs);
        }
    }

    for (int c = copies - 1; c >= 0; --c) {
        lume::TextStyle st = style;
        if (c > 0 && hasEcho)
            st.alpha = std::pow(echoDecay, c);
        QVector<lume::CharState> cs2;
        if (useOps) {
            cs2 = charStates;
            if (c > 0) {
                for (lume::CharState &s : cs2)
                    s.alpha = s.alpha * st.alpha;
            }
        }
        lume::Draw d;
        d.kind = lume::Draw::Text;
        d.layout = layout;
        d.x = x - (hasEcho ? echoDx * scale * c : 0);
        d.y = lume::baselineForMiddle(font, anchorY);
        d.style = st;
        d.charStates = cs2;
        draws.push_back(d);
    }
    wrapRotated(draws, firstDraw, pnum(pr, "rotate", 0), anchorX, anchorY);
}

// ═══════════════════════════════════════════════════════════════════════════
// 覆盖 / 部件分发
// ═══════════════════════════════════════════════════════════════════════════

void compileOverlayPart(Env &e, std::vector<lume::Draw> &draws, const Box &box, const PartRef &part,
                        double t, double bpm)
{
    const QJsonObject &pr = part.params;
    const QString kind = pstr(pr, "kind", QStringLiteral("vignette")).toLower();
    if (kind == QLatin1String("vignette")) {
        lume::Draw d;
        d.kind = lume::Draw::Vignette;
        d.x = box.x;
        d.y = box.y;
        d.w = box.w;
        d.h = box.h;
        d.edge = lume::parseColor(pstr(pr, "color", QStringLiteral("rgba(0,0,0,0.55)")),
                                  QColor(0, 0, 0, 140));
        d.inner = lume::parseColor(pstr(pr, "inner", QStringLiteral("rgba(0,0,0,0)")),
                                   QColor(0, 0, 0, 0));
        draws.push_back(d);
        return;
    }
    if (kind == QLatin1String("grain")) {
        const int w = int(std::lround(box.w));
        const int h = int(std::lround(box.h));
        if (w <= 0 || h <= 0)
            return;
        const double amount = pnum(pr, "amount", 420);
        const QColor color = lume::parseColor(pstr(pr, "color", QStringLiteral("#ffffff")), Qt::white);
        const double a = pnum(pr, "alpha", 0.05);
        const QString key = QStringLiteral("grain|%1|%2|%3|%4x%5")
                                .arg(amount)
                                .arg(color.name(QColor::HexArgb))
                                .arg(a)
                                .arg(w)
                                .arg(h);

        // 与背景同一条刷新规则：按 0.1s 档位分桶。
        // 旧写法用「距离上次烘焙多久」，来回 seek 会反复重烘同一档位。
        const qint64 bucket = bgBucket(t);
        auto it = e.grain->find(key);
        if (it == e.grain->end() || it->second.bucket != bucket) {
            auto img = std::make_shared<QImage>(w, h, QImage::Format_ARGB32_Premultiplied);
            img->fill(Qt::transparent);
            {
                std::vector<lume::Draw> local;
                const int n = int(std::min(700.0, amount));
                const double f = std::floor(t * 30);
                for (int i = 0; i < n; ++i) {
                    const double x = lume::hash01(i * 1.13 + f * 0.31, 21) * w;
                    const double y = lume::hash01(i * 2.71 + f * 0.47, 23) * h;
                    lume::Draw d;
                    d.kind = lume::Draw::Circle;
                    d.cx = x;
                    d.cy = y;
                    d.r = h * 0.0015;
                    d.color = A(color, a);
                    local.push_back(d);
                }
                QPainter gp(img.get());
                gp.setRenderHint(QPainter::Antialiasing, true);
                e.lume->paint(gp, local, w, h);
            }
            BgSlot slot;
            slot.img = std::move(img);
            slot.bucket = bucket;
            slot.renderedAt = t;
            slot.lastUsed = t; // 旧代码漏写，值恒为 0 → 一插入就被 LRU 当成最久未用
            (*e.grain)[key] = slot;
            ++(*e.bgRenders);
            evictIfNeeded(*e.grain, kGrainCacheSlots);
            it = e.grain->find(key);
        } else {
            it->second.lastUsed = t;
        }
        if (it == e.grain->end() || !it->second.img)
            return;
        lume::Draw d;
        d.kind = lume::Draw::Bitmap;
        d.src = it->second.img;
        d.x = box.x;
        d.y = box.y;
        d.w = box.w;
        d.h = box.h;
        draws.push_back(d);
        return;
    }
    compileBackgroundPart(e, draws, box, part, t, bpm);
}

/** 取一张图片（按绝对路径缓存）。加载失败返回空，并缓存失败避免每帧重试 IO。 */
std::shared_ptr<QImage> imageOf(Env &e, const QString &absPath)
{
    if (!e.images)
        return nullptr;
    auto it = e.images->find(absPath);
    if (it != e.images->end())
        return it->second;
    QImageReader rd(absPath);
    rd.setAutoTransform(true); // 尊重 EXIF 方向：手机拍的素材不转就是躺着的
    const QImage img = rd.read();
    std::shared_ptr<QImage> sp;
    if (!img.isNull())
        sp = std::make_shared<QImage>(img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    (*e.images)[absPath] = sp;
    return sp;
}

/**
 * 给图片染上一层主色调（`tint` + `tintAmount`）。
 *
 * 用途是"把人和景缝在一起"：立绘原图是中性的冷调，放进夕阳走廊里像剪贴上去的；
 * 盖一层暖色蒙版（保留明暗，只改色相）立刻就像在现场打的光。
 *
 * 结果按 (路径, 颜色, 强度) 缓存 —— 染色是一次全图合成，不能每帧做。
 */
std::shared_ptr<QImage> tintedImage(Env &e, const QString &absPath,
                                    const std::shared_ptr<QImage> &src, const QColor &tint,
                                    double amount)
{
    if (!src || src->isNull() || !e.tints)
        return src;
    amount = std::clamp(amount, 0.0, 1.0);
    if (amount <= 0.001 || !tint.isValid())
        return src;

    const QString key = absPath + QLatin1Char('|') + tint.name(QColor::HexRgb) + QLatin1Char('|') +
                        QString::number(int(amount * 100));
    auto it = e.tints->find(key);
    if (it != e.tints->end())
        return it->second;

    auto out = std::make_shared<QImage>(src->convertToFormat(QImage::Format_ARGB32_Premultiplied));
    QPainter p(out.get());
    // SourceAtop：只在已有像素上着色，透明区域保持透明（立绘的抠图边缘不会被染出方框）
    p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    QColor c = tint;
    c.setAlphaF(float(amount));
    p.fillRect(out->rect(), c);
    p.end();
    (*e.tints)[key] = out;
    return out;
}

/** 部件自带 `tint` 时返回染色后的图；没配就原图返回（零开销）。 */
std::shared_ptr<QImage> tintedFor(Env &e, const PartRef &part, const QString &absPath,
                                  const std::shared_ptr<QImage> &img)
{
    const QString hex = pstr(part.params, "tint");
    if (hex.isEmpty() || !img)
        return img;
    return tintedImage(e, absPath, img, lume::parseColor(hex, QColor()),
                       pnum(part.params, "tintAmount", 0.18));
}

/** 解析图片路径：`path`（可相对 .sf 目录）或 `asset` 走工程资源表。 */
QString resolveImagePath(Env &e, const PartRef &part)
{
    const QJsonObject &pr = part.params;
    QString path = pstr(pr, "path");
    if (path.isEmpty()) {
        // 二选一：直接给 path（脚本生成的工程常用），或给 asset id 走工程资源表
        const QString aid = pstr(pr, "asset");
        if (!aid.isEmpty() && e.p)
            path = e.p->assetPath(aid);
    }
    if (path.isEmpty())
        return QString();
    if (!QFileInfo(path).isAbsolute()) {
        const QFileInfo proj(e.p ? e.p->path : QString());
        path = QDir(proj.absolutePath()).absoluteFilePath(path);
    }
    return path;
}

/**
 * 这一层是否「不透明地铺满整个画幅」—— 是的话，它下面的图层一帧也看不见，
 * 可以直接跳过（省掉整幅离屏缓存刷新和一次全画幅 blit）。
 *
 * 判据：图片部件、alpha≈1、中心对齐画幅、cover/stretch 且占幅 >= 1、图片确实读得出来。
 * 只判"几何上盖得住"是不够的 —— 素材丢了还得让底下的渐变顶上来。
 */
bool paintsFullFrameOpaquely(Env &e, const PartRef &part)
{
    if (!(part.part == QLatin1String("image") || part.part == QLatin1String("imagePlate")))
        return false;
    const QJsonObject &pr = part.params;
    if (pnum(pr, "alpha", 1.0) < 0.999)
        return false;
    const QString fit = pstr(pr, "fit", QStringLiteral("cover")).toLower();
    if (fit == QLatin1String("contain"))
        return false; // contain 会留边，底下的东西露得出来
    if (pnum(pr, "w", 1.0) < 0.999 || pnum(pr, "h", 1.0) < 0.999)
        return false;
    if (std::fabs(pnum(pr, "x", 0.5) - 0.5) > 1e-3 || std::fabs(pnum(pr, "y", 0.5) - 0.5) > 1e-3)
        return false;
    const QString path = resolveImagePath(e, part);
    if (path.isEmpty())
        return false;
    const auto img = imageOf(e, path);
    return img && !img->isNull();
}

/**
 * 图片图层（部件名 "image" / "imagePlate"）。
 *
 * params：
 *   path   绝对路径，或相对 .sf 所在目录的路径
 *   x, y   图片中心在画幅上的相对位置（0..1，默认 0.5）
 *   w, h   占画幅的比例（默认 1.0）
 *   fit    cover（默认，铺满后裁掉溢出）/ contain（完整可见）/ stretch（拉伸变形）
 *   alpha  0..1
 *
 *   rotate 绕图片中心旋转的角度（度，默认 0）
 *
 * 时间范围由所在镜头的 start/end 决定 —— 部件自身不判时间，
 * 这样「把图片拖成任意长度」只是改镜头区间，不需要额外的时间参数。
 */
void compileImagePart(Env &e, std::vector<lume::Draw> &draws, const Box &box, const PartRef &part)
{
    const QJsonObject &pr = part.params;
    QString path = pstr(pr, "path");
    if (path.isEmpty()) {
        // 二选一：直接给 path（脚本生成的工程常用），或给 asset id 走工程资源表
        const QString aid = pstr(pr, "asset");
        if (!aid.isEmpty() && e.p)
            path = e.p->assetPath(aid);
    }
    if (path.isEmpty())
        return;
    if (!QFileInfo(path).isAbsolute()) {
        const QFileInfo proj(e.p ? e.p->path : QString());
        path = QDir(proj.absolutePath()).absoluteFilePath(path);
    }
    const auto img = tintedFor(e, part, path, imageOf(e, path));
    if (!img || img->isNull())
        return;

    const double cx = box.x + box.w * pnum(pr, "x", 0.5);
    const double cy = box.y + box.h * pnum(pr, "y", 0.5);
    const double bw = box.w * pnum(pr, "w", 1.0);
    const double bh = box.h * pnum(pr, "h", 1.0);
    const QString fit = pstr(pr, "fit", QStringLiteral("cover")).toLower();

    const double iw = img->width();
    const double ih = img->height();
    double dw = bw;
    double dh = bh;
    if (iw > 0 && ih > 0 && fit != QLatin1String("stretch")) {
        const double sx = bw / iw;
        const double sy = bh / ih;
        const double s = fit == QLatin1String("contain") ? std::min(sx, sy) : std::max(sx, sy);
        dw = iw * s;
        dh = ih * s;
    }

    lume::Draw d;
    d.kind = lume::Draw::Bitmap;
    d.src = img;
    d.x = cx - dw / 2;
    d.y = cy - dh / 2;
    d.w = dw;
    d.h = dh;
    d.alpha = pnum(pr, "alpha", 1.0);
    draws.push_back(d);
    wrapRotated(draws, draws.size() - 1, pnum(pr, "rotate", 0), cx, cy);
}

/** 定位 ffmpeg：环境变量 → 与本 exe 相对的常见位置 → PATH。找不到返回空，视频图层静默跳过。 */
QString ffmpegExe()
{
    static QMutex lock;
    QMutexLocker guard(&lock);
    static bool probed = false;
    static QString path;
    if (probed)
        return path;
    probed = true;

    const QByteArray env = qgetenv("SHIZUKUFLOW_FFMPEG");
    if (!env.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(env))) {
        path = QString::fromLocal8Bit(env);
        return path;
    }
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        dir + QStringLiteral("/tools/ffmpeg.exe"),
        dir + QStringLiteral("/../../app/src-tauri/tools/ffmpeg.exe"),
        dir + QStringLiteral("/../app/src-tauri/tools/ffmpeg.exe"),
        dir + QStringLiteral("/../../../app/src-tauri/tools/ffmpeg.exe"),
    };
    for (const QString &c : candidates) {
        const QFileInfo fi(c);
        if (fi.exists() && fi.isFile()) {
            path = fi.absoluteFilePath();
            return path;
        }
    }
    path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    return path;
}

/** 抽帧缓存目录：路径 + 大小 + 修改时间 + 抽帧率 + 宽度一起做键。
    素材被替换或重新导出时 mtime 变化 → 自动换目录，不会读到旧帧。 */
QString videoCacheDir(const QString &absPath, double sampleFps, int width)
{
    const QFileInfo fi(absPath);
    const QByteArray key = (absPath + QLatin1Char('|') + QString::number(fi.size()) +
                            QLatin1Char('|') + QString::number(fi.lastModified().toMSecsSinceEpoch()) +
                            QLatin1Char('|') + QString::number(int(sampleFps)) + QLatin1Char('|') +
                            QString::number(width))
                               .toUtf8();
    const QString h =
        QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty())
        root = QDir::tempPath() + QStringLiteral("/shizukuflow");
    return root + QStringLiteral("/video/") + h;
}

/**
 * 把视频抽成 JPEG 序列（一次进程调用解码整段），返回可用帧数。
 *
 * 为什么不用「每帧 seek 一次 ffmpeg」：6400 帧的成片就是 6400 次进程启动，
 * 光进程创建就要几十秒。整段抽一次落盘、按帧号读，同一素材只付一次成本，
 * 而且缓存跨进程存活 —— 第二次打开工程直接命中。
 */
bool ensureVideoFrames(const QString &src, const QString &dir, double sampleFps, int width)
{
    QDir d(dir);
    const QString done = d.absoluteFilePath(QStringLiteral("done"));
    if (QFileInfo::exists(done))
        return true; // 已经抽过（marker 里记帧数，这里只需知道"完成"）

    const QString ff = ffmpegExe();
    if (ff.isEmpty())
        return false;

    if (!d.exists() && !QDir().mkpath(dir))
        return false;
    // 上次中断留下的半套帧：先清干净，否则帧数会串
    for (const QString &old : d.entryList(QStringList{QStringLiteral("f*.jpg")}, QDir::Files))
        d.remove(old);

    QProcess proc;
    proc.setProgram(ff);
    proc.setArguments({QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                       QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), src,
                       QStringLiteral("-vf"),
                       QStringLiteral("fps=%1,scale=%2:-2")
                           .arg(QString::number(sampleFps, 'g', 4))
                           .arg(width),
                       QStringLiteral("-q:v"), QStringLiteral("3"),
                       d.absoluteFilePath(QStringLiteral("f%05d.jpg"))});
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    if (!proc.waitForStarted(10000))
        return false;
    // 长素材要留足时间：解码 10 分钟的视频在慢机器上可能几十秒
    if (!proc.waitForFinished(600000) || proc.exitCode() != 0) {
        proc.kill();
        proc.waitForFinished(2000);
        return false;
    }

    const int count = d.entryList(QStringList{QStringLiteral("f*.jpg")}, QDir::Files).size();
    if (count <= 0)
        return false;
    QFile mark(done);
    if (mark.open(QIODevice::WriteOnly))
        mark.write(QByteArray::number(count));
    return true;
}

/** 从抽帧目录读第 idx 帧（0 基），走槽内 LRU；读失败返回空。 */
std::shared_ptr<QImage> videoFrameAt(Env &e, VideoSlot &slot, int idx)
{
    auto it = slot.frames.find(idx);
    if (it != slot.frames.end()) {
        for (auto i = slot.lru.begin(); i != slot.lru.end(); ++i)
            if (*i == idx) {
                slot.lru.erase(i);
                break;
            }
        slot.lru.push_back(idx);
        return it->second;
    }

    const QString path =
        QStringLiteral("%1/f%2.jpg").arg(slot.dir, QString::number(idx + 1).rightJustified(5, '0'));
    QImageReader rd(path);
    rd.setAutoTransform(true);
    const QImage img = rd.read();
    if (img.isNull())
        return nullptr;
    auto sp = std::make_shared<QImage>(img.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    slot.frames[idx] = sp;
    slot.lru.push_back(idx);
    while (slot.lru.size() > kVideoFrameSlots) {
        slot.frames.erase(slot.lru.front());
        slot.lru.pop_front();
    }
    (void)e;
    return sp;
}

/**
 * 视频图层（部件名 "video"）。
 *
 * 和 image 的差别只有一件事：内容随时间变。参数刻意做成 image 的超集 ——
 * 同一个图层从静帧换成视频，只需要把 part 改成 video，构图参数一个都不用动。
 *
 * params：
 *   path        视频文件；相对 .sf 所在目录，也可用 asset 走资源表
 *   in          源内起点（秒，默认 0）
 *   start       时间轴上的起点（绝对秒，默认 = 所在镜头起点）
 *   rate        播放速率（默认 1，2 = 两倍速）
 *   loop        true（默认）循环；false 播完停在最后一帧
 *   x, y, w, h, fit, alpha, flip   与 image / portrait 同义
 *   at / outAt / inDur / outDur    绝对时间淡入淡出（与 portrait 同一套语义）
 *
 * 落地方式：首次用到时用 ffmpeg 把整段抽成 JPEG 序列到缓存目录（跨进程复用），
 * 渲染时按时间取帧 —— 内核不碰解码器，导出/预览/无头工具走同一条路径。
 */
void compileVideoPart(Env &e, std::vector<lume::Draw> &draws, const Box &box, const PartRef &part,
                      double t)
{
    if (!e.videos)
        return;
    const QJsonObject &pr = part.params;
    const QString path = resolveImagePath(e, part);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;

    const double canvasFps = (e.p && e.p->canvas.fps > 0) ? e.p->canvas.fps : 30;
    const int sampleFps = int(std::lround(std::clamp(canvasFps, 12.0, 30.0)));
    const int vw = std::max(64, int(std::lround(box.w)));
    const QString key =
        path + QLatin1Char('|') + QString::number(sampleFps) + QLatin1Char('|') + QString::number(vw);

    auto it = e.videos->find(key);
    if (it == e.videos->end()) {
        VideoSlot slot;
        slot.dir = videoCacheDir(path, sampleFps, vw);
        slot.sampleFps = sampleFps;
        // 导出是并行的，多个渲染器可能同时撞上同一段素材：抽帧是「整段落盘」的
        // 独占操作，必须串行 —— 否则两个进程同时往一个目录里写序列会互相覆盖。
        static QMutex extractMutex;
        {
            QMutexLocker lock(&extractMutex);
            if (!ensureVideoFrames(path, slot.dir, sampleFps, vw))
                slot.failed = true;
            else
                slot.frameCount = QDir(slot.dir)
                                      .entryList(QStringList{QStringLiteral("f*.jpg")}, QDir::Files)
                                      .size();
        }
        if (slot.frameCount <= 0)
            slot.failed = true;
        it = e.videos->emplace(key, std::move(slot)).first;
    }
    VideoSlot &slot = it->second;
    if (slot.failed || slot.frameCount <= 0)
        return;

    // 绝对时间 → 源内秒 → 帧号。start 缺省时跟镜头同开，用户拖进来就是「这一段用它」。
    const double inSec = pnum(pr, "in", 0);
    const double startSec = phas(pr, "start") ? pnum(pr, "start", 0) : e.shotStart;
    const double rate = std::max(0.05, pnum(pr, "rate", 1));
    const bool loop = !phas(pr, "loop") || pr.value(QLatin1String("loop")).toBool(true);
    double f = ((t - startSec) * rate + inSec) * slot.sampleFps;
    int idx = int(std::floor(f));
    if (loop) {
        idx %= slot.frameCount;
        if (idx < 0)
            idx += slot.frameCount;
    } else {
        idx = std::clamp(idx, 0, slot.frameCount - 1);
    }

    const auto img = videoFrameAt(e, slot, idx);
    if (!img || img->isNull() || img->width() <= 0 || img->height() <= 0)
        return;

    double a = clamp01(pnum(pr, "alpha", 1.0));
    if (phas(pr, "at"))
        a *= clamp01((t - pnum(pr, "at", 0)) / std::max(1e-3, pnum(pr, "inDur", 0.5)));
    if (phas(pr, "outAt"))
        a *= clamp01((pnum(pr, "outAt", 0) - t) / std::max(1e-3, pnum(pr, "outDur", 0.5)));
    if (a <= 0.002)
        return;

    const double cx = box.x + box.w * pnum(pr, "x", 0.5);
    const double cy = box.y + box.h * pnum(pr, "y", 0.5);
    const double bw = box.w * pnum(pr, "w", 1.0);
    const double bh = box.h * pnum(pr, "h", 1.0);
    const QString fit = pstr(pr, "fit", QStringLiteral("cover")).toLower();

    const double iw = img->width();
    const double ih = img->height();
    double dw = bw;
    double dh = bh;
    if (fit != QLatin1String("stretch")) {
        const double s = (fit == QLatin1String("contain")) ? std::min(bw / iw, bh / ih)
                                                           : std::max(bw / iw, bh / ih);
        dw = iw * s;
        dh = ih * s;
    }
    const double dx = cx - dw / 2;
    const double dy = cy - dh / 2;

    lume::Draw im;
    im.kind = lume::Draw::Bitmap;
    im.src = img;
    im.alpha = a;

    if (pr.value(QLatin1String("flip")).toBool(false)) {
        lume::Draw tf;
        tf.kind = lume::Draw::Transform;
        tf.a = -1;
        tf.d = 1;
        tf.e = dx + dw;
        tf.f = dy;
        im.x = 0;
        im.y = 0;
        im.w = dw;
        im.h = dh;
        tf.sub.push_back(im);
        draws.push_back(tf);
        wrapRotated(draws, draws.size() - 1, pnum(pr, "rotate", 0), cx, cy);
        return;
    }

    im.x = dx;
    im.y = dy;
    im.w = dw;
    im.h = dh;
    draws.push_back(im);
    wrapRotated(draws, draws.size() - 1, pnum(pr, "rotate", 0), cx, cy);
}

/**
 * 角色立绘图层（部件名 "portrait"）。
 *
 * 和 image 的区别：image 是"铺满画幅的一张图"，portrait 是"站在画里的人物"——
 * 底边对齐、按身高定宽、自带呼吸与节拍、入场不硬切。
 *
 * params：
 *   path   绝对路径，或相对 .sf 所在目录（与 image 同一套解析）
 *   x      人物水平中心（0..1，默认 0.78，靠右不挡歌词）
 *   y      人物底边（0..1，默认 1.0，站在画框底）
 *   h      人物高度占画幅比例（默认 0.95）
 *   alpha  0..1
 *   flip   true = 水平镜像
 *   at / inDur    绝对时间入场（淡入 + 上浮）；不填则整段常显
 *   outAt / outDur 绝对时间退场
 *   sway   呼吸幅度（占画幅高的比例，默认 0.005）
 *   swaySpeed 呼吸速度（默认 0.55）
 *   beat   下拍脉冲强度（默认 0.012）
 *   glow   背后柔光强度（0..1，默认 0）
 *   glowColor
 *   rotate 绕脚下中心旋转的角度（度，默认 0；正 = 顺时针）
 *   seed   相位种子：同一段里多个镜头给了同一个 seed，切镜时呼吸不会跳
 *
 * 时间用绝对秒（at/outAt）：立绘横跨若干歌词镜头时，每个镜头各放一份同样的
 * 部件、at 指向同一时刻，入场就只发生一次 —— 镜头切了，人没动，这才是"人在画里"。
 */
void compilePortraitPart(Env &e, std::vector<lume::Draw> &draws, const Box &box,
                         const PartRef &part, double t)
{
    const QJsonObject &pr = part.params;
    QString path = pstr(pr, "path");
    if (path.isEmpty()) {
        const QString aid = pstr(pr, "asset");
        if (!aid.isEmpty() && e.p)
            path = e.p->assetPath(aid);
    }
    if (path.isEmpty())
        return;
    if (!QFileInfo(path).isAbsolute()) {
        const QFileInfo proj(e.p ? e.p->path : QString());
        path = QDir(proj.absolutePath()).absoluteFilePath(path);
    }
    const auto img = tintedFor(e, part, path, imageOf(e, path));
    if (!img || img->isNull() || img->width() <= 0 || img->height() <= 0)
        return;

    double a = clamp01(pnum(pr, "alpha", 1.0));
    if (phas(pr, "at"))
        a *= clamp01((t - pnum(pr, "at", 0)) / std::max(1e-3, pnum(pr, "inDur", 0.7)));
    if (phas(pr, "outAt"))
        a *= clamp01((pnum(pr, "outAt", 0) - t) / std::max(1e-3, pnum(pr, "outDur", 0.5)));
    if (a <= 0.002)
        return;

    const double seed = pnum(pr, "seed", 0);
    const double sway = pnum(pr, "sway", 0.005) * box.h;
    const double sp = pnum(pr, "swaySpeed", 0.55);
    const double ph = seed * 1.7;
    // 呼吸：两条不同频率的正弦，避免看出周期的机械感
    const double bob = std::sin(t * sp + ph) * sway;
    const double drift = std::sin(t * sp * 0.47 + ph * 1.3) * sway * 0.7;

    double scale = 1;
    const double beat = pnum(pr, "beat", 0.012);
    if (beat > 0 && e.p)
        scale += beat * std::exp(-8 * tSinceDownbeat(*e.p, t));

    // 入场是从下方 2% 画幅处浮上来；at 之前 alpha=0，所以位移不可见
    double rise = 0;
    if (phas(pr, "at")) {
        const double k = clamp01((t - pnum(pr, "at", 0)) / std::max(1e-3, pnum(pr, "inDur", 0.7)));
        rise = (1 - k) * (1 - k) * box.h * 0.02;
    }

    const double iw = img->width();
    const double ih = img->height();
    const double bh = box.h * pnum(pr, "h", 0.95) * scale;
    const double bw = bh * iw / ih;
    const double cx = box.x + box.w * pnum(pr, "x", 0.78) + drift;
    const double by = box.y + box.h * pnum(pr, "y", 1.0) + bob + rise;
    const double bx = cx - bw / 2;
    const double ty = by - bh;

    // 接触影：立绘是从字卡上裁下来的"硬边人",不落地就会像贴纸浮在画上。
    // 在脚下压一团压扁的柔影，暗底上看不见、浅底上立刻把人按到台面上。
    const double shadow = pnum(pr, "shadow", 0);
    if (shadow > 0) {
        const double rx = bw * 0.62;
        const double ry = bh * 0.16;
        lume::Draw sh;
        sh.kind = lume::Draw::Radial;
        sh.cx = 0;
        sh.cy = 0;
        sh.x = -rx;
        sh.y = -rx;
        sh.w = rx * 2;
        sh.h = rx * 2;
        sh.rInner = rx * 0.05;
        sh.rOuter = rx;
        sh.inner = A(QColor(6, 10, 18), 0.30 * shadow);
        sh.outer = A(QColor(6, 10, 18), 0);

        lume::Draw tf;
        tf.kind = lume::Draw::Transform;
        tf.a = 1;
        tf.d = ry / rx;   // 正圆压成椭圆
        tf.e = cx;
        tf.f = by - bh * 0.06;
        tf.sub.push_back(sh);
        draws.push_back(tf);
    }

    const double glow = pnum(pr, "glow", 0);
    if (glow > 0) {
        const QColor gc = lume::parseColor(pstr(pr, "glowColor", QStringLiteral("#ffffff")),
                                           Qt::white);
        const double r = bh * 0.62;
        lume::Draw g;
        g.kind = lume::Draw::Radial;
        g.cx = cx;
        g.cy = ty + bh * 0.52;
        g.x = g.cx - r;
        g.y = g.cy - r;
        g.w = r * 2;
        g.h = r * 2;
        g.rInner = r * 0.12;
        g.rOuter = r;
        g.inner = A(gc, 0.13 * glow);
        g.outer = A(gc, 0);
        draws.push_back(g);
    }

    lume::Draw im;
    im.kind = lume::Draw::Bitmap;
    im.src = img;
    im.x = bx;
    im.y = ty;
    im.w = bw;
    im.h = bh;
    im.alpha = a;

    // 绕脚下中心旋转：人歪着靠向画面（PV 里常见的斜构图）。支点必须取底边，
    // 否则一转人就离地飘起来。
    const size_t figStart = draws.size();
    const double tilt = pnum(pr, "rotate", 0);
    if (pr.value(QLatin1String("flip")).toBool()) {
        // 镜像：QPainter 没有"负宽度"，只能套一层矩阵（x' = -x + (bx + bw)）
        lume::Draw tf;
        tf.kind = lume::Draw::Transform;
        tf.a = -1;
        tf.d = 1;
        tf.e = bx + bw;
        tf.f = ty;
        im.x = 0;
        im.y = 0;
        tf.sub.push_back(im);
        draws.push_back(tf);
        wrapRotated(draws, figStart, tilt, cx, by);
        return;
    }
    draws.push_back(im);
    wrapRotated(draws, figStart, tilt, cx, by);
}

void compilePart(Env &e, std::vector<lume::Draw> &draws, const Box &box, const PartRef &part,
                 double t, double bpm)
{
    // 未知部件静默跳过：加了一个不认识的部件也不该让整片渲染崩掉
    if (part.part == QLatin1String("background"))
        compileBackgroundPart(e, draws, box, part, t, bpm);
    else if (part.part == QLatin1String("text"))
        compileTextPart(draws, box, part, t, 1.0);
    else if (part.part == QLatin1String("overlay"))
        compileOverlayPart(e, draws, box, part, t, bpm);
    else if (part.part == QLatin1String("image") || part.part == QLatin1String("imagePlate"))
        compileImagePart(e, draws, box, part);
    else if (part.part == QLatin1String("video"))
        compileVideoPart(e, draws, box, part, t);
    else if (part.part == QLatin1String("portrait"))
        compilePortraitPart(e, draws, box, part, t);
}

void compileDecorPart(Env &e, std::vector<lume::Draw> &draws, const Box &box, const PartRef &part,
                      double t)
{
    const Project &p = *e.p;
    // 图片也允许挂在装饰层：多数构图里图片就是"底图 + 上面的字"，
    // 放 decor 能保证它画在文字之下（decor 在 text 之前绘制）。
    if (part.part == QLatin1String("image") || part.part == QLatin1String("imagePlate")) {
        compileImagePart(e, draws, box, part);
        return;
    }
    if (part.part == QLatin1String("video")) {
        compileVideoPart(e, draws, box, part, t);
        return;
    }
    if (part.part == QLatin1String("beatRings")) {
        compileBeatRings(draws, box, part, t, p);
        return;
    }
    if (part.part == QLatin1String("bars")) {
        compileBars(draws, box, part, t, p);
        return;
    }
    if (part.part == QLatin1String("particles")) {
        compileParticles(draws, box, part, t);
        return;
    }
    if (part.part == QLatin1String("flash")) {
        compileDecorFlash(draws, box, part, t);
        return;
    }
    if (part.part == QLatin1String("frameLines")) {
        compileFrameLines(draws, box, part);
        return;
    }
    if (part.part == QLatin1String("glowDot")) {
        const QJsonObject &pr2 = part.params;
        const double scale = box.h / 1080.0;
        glowDot(draws, box.x + box.w * pnum(pr2, "x", 0.5), box.y + box.h * pnum(pr2, "y", 0.5),
                pnum(pr2, "r", 6) * scale,
                lume::parseColor(pstr(pr2, "color", QStringLiteral("#ffffff")), Qt::white),
                clamp01(pnum(pr2, "k", 0.8)));
        return;
    }
    compilePart(e, draws, box, part, t, p.bpm);
}

// ═══════════════════════════════════════════════════════════════════════════
// 歌词（逐字卡拉OK）
// ═══════════════════════════════════════════════════════════════════════════

struct LyricLayoutCfg
{
    QString mode = QStringLiteral("center");
    double sizeScale = 1;
    double x = 0;
    double y = 0;
    bool hasX = false;
    bool hasY = false;
    double rotate = 0; // 斜め歌詞：整行绕排版锚点转
};

LyricLayoutCfg lyricLayoutOf(const Shot &shot)
{
    LyricLayoutCfg c;
    if (!shot.hasHold)
        return c;
    const QJsonObject &pr = shot.hold.params;
    c.mode = pstr(pr, "mode", QStringLiteral("center"));
    c.sizeScale = pnum(pr, "sizeScale", 1);
    if (phas(pr, "x")) {
        c.hasX = true;
        c.x = pnum(pr, "x", 0);
    }
    if (phas(pr, "y")) {
        c.hasY = true;
        c.y = pnum(pr, "y", 0);
    }
    c.rotate = pnum(pr, "rotate", 0);
    return c;
}

void compileLyric(std::vector<lume::Draw> &draws, const Box &box, const Project &p,
                  const Lyric &lyric, double t, const QColor &accent, const Shot &shot, double safe)
{
    const StyleDef *stp = p.style(lyric.style);
    StyleDef st;
    if (stp)
        st = *stp;

    const double scale = box.h / 1080.0;
    const LyricLayoutCfg lay = lyricLayoutOf(shot);
    double size = (st.size > 0 ? st.size : 96) * scale * lay.sizeScale;

    QVector<Syllable> syllables = lyric.syllables;
    if (syllables.isEmpty()) {
        Syllable s;
        s.text = lyric.text;
        s.start = lyric.start;
        s.end = lyric.end;
        syllables.push_back(s);
    }
    const int n = syllables.size();

    const QStringList texts = [&] {
        QStringList l;
        for (const Syllable &s : syllables)
            l << s.text;
        return l;
    }();

    const double maxW = box.w * 0.92 * safe;
    const auto fontAt = [&](double s) { return lyricFontOf(st.font, st.weight, s); };
    const auto lsAt = [&](double s) { return st.letterSpacing * s; };
    const auto measureAt = [&](double s) {
        return lume::layoutRun(texts, fontAt(s), lsAt(s));
    };

    if (lay.mode == QLatin1String("vertical")) {
        const double yFrac = lay.hasY ? lay.y : 0.16;
        const double availH = box.h * std::max(0.2, 0.92 - yFrac) * safe;
        size = std::min(size, availH / std::max(1.0, n * 1.16));
        for (int pass = 0; pass < 2; ++pass) {
            double widest = 0;
            for (const Syllable &s : syllables)
                widest = std::max(widest, lume::layoutRun({s.text}, fontAt(size), 0)->totalWidth);
            if (widest > box.w * 0.16 * safe && widest > 0)
                size *= (box.w * 0.16 * safe) / widest;
        }
    } else {
        const double maxH = (lay.mode == QLatin1String("giant")
                                 ? box.h * 0.76
                                 : lay.mode == QLatin1String("corner") ? box.h * 0.2 : box.h * 0.32) *
                            safe;
        const double maxRatio = lay.mode == QLatin1String("giant") ? 9 : 1;
        for (int pass = 0; pass < 2; ++pass) {
            const auto probe = measureAt(size);
            if (probe->totalWidth <= 0)
                break;
            const double ratio = std::min(maxRatio, maxW / probe->totalWidth);
            if (ratio == 1)
                break;
            size *= ratio;
        }
        size = std::min(size, maxH);
    }
    size = std::max(14.0 * scale, size);

    const double ls = lsAt(size);
    const QString font = fontAt(size);

    // ── 长音留白 ──
    // .lrc 只有行级时间，逐字时值全靠均分，所以行尾那个字天然比中位时值长 1.5 倍。
    // 线性推进在长音上会"卡住"：字唱到三成就停在那儿，剩下的时间画面是死的。
    // 做法：时值 > 中位数 ×1.35 的音节改走 ease-out，在 62% 时值内唱满，其余留白
    // （字不再动，交给背景/运镜呼吸）；唱满后强调色仍挂在字上并缓缓褪回本色 ——
    // 已唱的亮着，未唱的仍是 ghost 暗字。
    double medDur = 0;
    {
        QVector<double> ds;
        ds.reserve(n);
        for (const Syllable &s : syllables)
            if (s.end > s.start)
                ds.push_back(s.end - s.start);
        if (!ds.isEmpty()) {
            std::sort(ds.begin(), ds.end());
            medDur = ds[ds.size() / 2];
        }
    }
    constexpr double kLongRatio = 1.35;
    constexpr double kLongSing = 0.62;
    const auto isLongNote = [&](const Syllable &s) {
        return medDur > 0 && (s.end - s.start) > medDur * kLongRatio;
    };
    const auto measureK = [&](const Syllable &s) {
        const double d = s.end - s.start;
        if (d <= 0)
            return 1.0;
        const double x = clamp01((t - s.start) / d);
        if (!isLongNote(s))
            return x;
        const double u = clamp01(x / kLongSing);
        return 1.0 - std::pow(1.0 - u, 2.4); // 起音冲过去，尾巴平住
    };
    // 长音余韵：0 = 刚唱满，1 = 音节结束。
    const auto holdK = [&](const Syllable &s) {
        const double done = s.start + (s.end - s.start) * kLongSing;
        if (t <= done)
            return 0.0;
        return clamp01((t - done) / std::max(1e-4, s.end - done));
    };

    // 行入场：trackIn / popUp / flyIn
    QString eStyle;
    double eDur = 0.35;
    if (shot.hasEntrance) {
        const QJsonObject &ep = shot.entrance.params;
        eStyle = pstr(ep, "style");
        eDur = pnum(ep, "dur", 0.35);
    }
    const double entranceK = eStyle.isEmpty() ? 1.0 : clamp01((t - lyric.start) / eDur);

    const auto applyEntrance = [&](lume::CharState &cs, int i, int nn) {
        if (eStyle.isEmpty() || entranceK >= 1)
            return;
        const double side = (i % 2 == 0) ? -1.0 : 1.0;
        if (eStyle == QLatin1String("trackIn")) {
            cs.dx = ((i - (nn - 1) / 2.0) / std::max(1, nn)) * (1 - entranceK) * box.w * 0.6;
            cs.alpha = entranceK;
        } else if (eStyle == QLatin1String("popUp")) {
            const double kb = lume::ease(QStringLiteral("backOut"), entranceK);
            cs.scale = cs.scale * (0.55 + 0.45 * kb);
            cs.dy = (1 - entranceK) * 26 * scale;
            cs.alpha = std::min(1.0, entranceK * 2.5);
        } else if (eStyle == QLatin1String("flyIn")) {
            cs.dx = side * (1 - entranceK) * box.w * 0.45;
            cs.rotation = side * (1 - entranceK) * 0.5;
            cs.alpha = std::min(1.0, entranceK * 1.6);
        }
    };

    const auto mkCharState = [&](int i) {
        const Syllable &s = syllables[i];
        const double k = measureK(s);
        lume::CharState cs;
        cs.reveal = k;
        if (k > 0 && k < 1) {
            cs.hasColor = true;
            cs.color = accent;
            cs.scale = isLongNote(s) ? 1.20 : 1.14;
        } else if (k >= 1 && isLongNote(s) && t < s.end) {
            // 留白段：强调色继续挂在字上，再用剩余时值退回本色（余韵渐隐）。
            const double h = holdK(s);
            const double w = 1.0 - 0.75 * h;
            cs.hasColor = true;
            cs.color = QColor::fromRgbF(accent.redF() * w + st.fill.redF() * (1 - w),
                                        accent.greenF() * w + st.fill.greenF() * (1 - w),
                                        accent.blueF() * w + st.fill.blueF() * (1 - w),
                                        accent.alphaF());
            cs.scale = 1.0 + 0.14 * (1 - h);
        }
        applyEntrance(cs, i, n);
        return cs;
    };

    lume::TextStyle fillStyle;
    fillStyle.fill = st.fill;
    if (st.hasShadow) {
        fillStyle.hasShadow = true;
        fillStyle.shadowColor = st.hasShadow ? st.shadowColor : QColor(0, 0, 0, 128);
        fillStyle.shadowBlur = st.shadowBlur * scale;
        fillStyle.shadowOffX = st.shadowOffX * scale;
        fillStyle.shadowOffY = st.shadowOffY * scale;
    }

    // 长音压暗：正在唱长音时，这一行还没唱到的字再压暗一档 ——
    // 已唱的那个字被拉亮放大，未唱的自然退到背景里，视线只跟着声音走。
    int longAt = -1;
    for (int i = 0; i < n; ++i) {
        const Syllable &s = syllables[i];
        if (isLongNote(s) && t >= s.start && t < s.end) {
            longAt = i;
            break;
        }
    }

    // ghost 模板：一律用歌词本色低透明度（描边样式若用描边色，深底上会隐身）
    lume::TextStyle ghostStyle;
    if (st.hasStroke) {
        ghostStyle.hasStroke = true;
        ghostStyle.strokeColor = A(st.fill, 0.5);
        ghostStyle.strokeWidth = st.strokeWidth * scale * lay.sizeScale;
        // 必须显式给透明填充：TextStyle::fill 默认是白色实心，
        // 只设描边的话 ghost 会变成一坨白字（浅底上直接看不见）。
        ghostStyle.fill = QColor(0, 0, 0, 0);
    } else {
        // 0.20 而不是 0.16：压了暗底图之后，0.16 的 ghost 字基本看不见，
        // "未唱"就变成了"没有"，卡拉OK 的推进感也一并消失。
        ghostStyle.fill = A(st.fill, 0.20);
    }

    // ── 竖排：逐字一个 draw，沿 y 下排
    if (lay.mode == QLatin1String("vertical")) {
        const double lh = size * 1.16;
        double widest = 0;
        for (const Syllable &s : syllables)
            widest = std::max(widest, lume::layoutRun({s.text}, font, 0)->totalWidth);
        const double xWant = box.x + box.w * (lay.hasX ? lay.x : 0.84);
        const double x = std::min(std::max(xWant, box.x + widest * 0.6 + box.w * 0.02),
                                  box.x + box.w * 0.98 - widest * 0.6);
        const double colH = lh * n;
        double y0 = box.y + box.h * (lay.hasY ? lay.y : 0.16);
        if (y0 + colH > box.y + box.h * 0.98)
            y0 = std::max(box.y + box.h * 0.02, box.y + box.h * 0.98 - colH);
        for (int i = 0; i < n; ++i) {
            const auto one = lume::layoutRun({syllables[i].text}, font, 0);
            const double ym = y0 + lh * i + lh / 2;
            lume::Draw g;
            g.kind = lume::Draw::Text;
            g.layout = one;
            g.x = x - one->totalWidth / 2;
            g.y = lume::baselineForMiddle(font, ym);
            g.style = ghostStyle;
            if (longAt >= 0 && i > longAt)
                g.style.alpha = 0.45;
            draws.push_back(g);

            lume::Draw f;
            f.kind = lume::Draw::Text;
            f.layout = one;
            f.x = x - one->totalWidth / 2;
            f.y = lume::baselineForMiddle(font, ym);
            f.style = fillStyle;
            f.charStates.push_back(mkCharState(i));
            draws.push_back(f);
        }
        return;
    }

    // ── 横排系（center / corner / giant）
    const auto layout = lume::layoutRun(texts, font, ls);
    double startX = 0;
    double cy = 0;
    if (lay.mode == QLatin1String("corner")) {
        startX = box.x + box.w * (lay.hasX ? lay.x : 0.08);
        cy = box.y + box.h * (lay.hasY ? lay.y : 0.8);
        startX = std::min(startX, box.x + box.w * 0.92 - layout->totalWidth);
        startX = std::max(startX, box.x + box.w * 0.04);
    } else {
        const double yFrac = lay.hasY ? lay.y : (lay.mode == QLatin1String("giant") ? 0.5 : 0.58);
        startX = box.x + box.w * (lay.hasX ? lay.x : 0.5) - layout->totalWidth / 2;
        cy = box.y + box.h * yFrac;
    }
    const double half = size * 0.5;
    cy = std::min(std::max(cy, box.y + half * 1.02), box.y + box.h - half * 1.02);
    const double baseline = lume::baselineForMiddle(font, cy);

    {
        lume::Draw g;
        g.kind = lume::Draw::Text;
        g.layout = layout;
        g.x = startX;
        g.y = baseline;
        g.style = ghostStyle;
        if (longAt >= 0) {
            // 只给未唱到的字压暗；已唱的反正会被上层实心字盖住，给不给都一样。
            g.charStates.resize(n);
            for (int i = longAt + 1; i < n; ++i)
                g.charStates[i].alpha = 0.45;
        }
        draws.push_back(g);
    }
    {
        lume::Draw f;
        f.kind = lume::Draw::Text;
        f.layout = layout;
        f.x = startX;
        f.y = baseline;
        f.style = fillStyle;
        f.charStates.reserve(n);
        for (int i = 0; i < n; ++i)
            f.charStates.push_back(mkCharState(i));
        draws.push_back(f);
    }

    if (lay.mode == QLatin1String("corner")) {
        const double dotK =
            0.5 + 0.5 * std::sin(t * (p.bpm > 0 ? (p.bpm / 60) * M_PI : 2.0));
        glowDot(draws, box.x + box.w * 0.85, cy - size * 0.3, 7 * scale, accent, dotK);
    }

    // 强调色下划（正在唱的那一字的进度条）
    const double uy = std::min(cy + size * 0.62, box.y + box.h * 0.985);
    for (int i = 0; i < n; ++i) {
        const Syllable &s = syllables[i];
        const double k = measureK(s);
        double barK = (k > 0 && k < 1) ? k : 0.0;
        double barA = 1.0;
        if (barK <= 0 && t < s.end && isLongNote(s)) {
            // 长音唱满后进度条停满格并淡出：留白也看得见"音还在"
            barK = 1.0;
            barA = 1.0 - holdK(s);
        }
        if (barK <= 0)
            continue;
        lume::Draw d;
        d.kind = lume::Draw::Rect;
        d.x = startX + layout->boxes[i].x;
        d.y = uy;
        d.w = layout->boxes[i].width * barK;
        d.h = std::max(2.0, 3 * scale);
        d.color = A(accent, clamp01(barA));
        draws.push_back(d);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 运镜 / 转场
// ═══════════════════════════════════════════════════════════════════════════

bool cameraMatrix(const Shot &shot, double t, const Box &box, const Project &p, double *out)
{
    if (!shot.hasCamera)
        return false;
    const QJsonObject &pr = shot.camera.params;
    const double dur = std::max(0.001, shot.end - shot.start);
    const double k = clamp01((t - shot.start) / dur);

    double z = 1;
    const QJsonArray zoom = pr.value(QLatin1String("zoom")).toArray();
    if (zoom.size() >= 2)
        z = zoom.at(0).toDouble() + (zoom.at(1).toDouble() - zoom.at(0).toDouble()) * k;

    const double punch = pnum(pr, "punch", 0);
    const double beatScale = punch > 0 ? 1 + punch * std::exp(-8 * tSinceDownbeat(p, t)) : 1;
    double scale = z * beatScale;

    double px = 0, py = 0;
    const QJsonArray pan = pr.value(QLatin1String("pan")).toArray();
    if (pan.size() >= 2) {
        px = pan.at(0).toDouble() * (t - shot.start);
        py = pan.at(1).toDouble() * (t - shot.start);
    }

    double deg = 0;
    const QJsonArray rot = pr.value(QLatin1String("rotate")).toArray();
    if (rot.size() >= 2)
        deg = rot.at(0).toDouble() + (rot.at(1).toDouble() - rot.at(0).toDouble()) * k;

    // ── 关键帧路径 ─────────────────────────────────────────────────────
    // keys: [{ t: 绝对秒, zoom: 1.0, x: 0.5, y: 0.5 }, ...]，x/y 是"要落在画面中心的
    // 那个点"（0..1 的画幅百分比）。两段式 zoom/pan 只能表达"从 A 匀速到 B"，
    // 而 PV 里真正好看的是"先压住不动、进副歌再推、最后一个急停" —— 那需要三个点。
    // 有 keys 时它接管缩放与对焦，pan/zoom 忽略；rotate/punch 仍然叠加。
    double focusX = box.x + box.w / 2;
    double focusY = box.y + box.h / 2;
    const QJsonArray keys = pr.value(QLatin1String("keys")).toArray();
    if (keys.size() >= 2) {
        struct Key
        {
            double t, z, x, y;
        };
        std::vector<Key> ks;
        ks.reserve(size_t(keys.size()));
        for (const QJsonValue &v : keys) {
            const QJsonObject o = v.toObject();
            ks.push_back(Key{pnum(o, "t", 0), pnum(o, "zoom", 1.0), pnum(o, "x", 0.5),
                             pnum(o, "y", 0.5)});
        }
        std::sort(ks.begin(), ks.end(), [](const Key &a, const Key &b) { return a.t < b.t; });

        double zk = ks.front().z;
        double fx = ks.front().x;
        double fy = ks.front().y;
        if (t <= ks.front().t) {
            // 落在第一帧之前：保持起始姿态
        } else if (t >= ks.back().t) {
            zk = ks.back().z;
            fx = ks.back().x;
            fy = ks.back().y;
        } else {
            for (size_t i = 1; i < ks.size(); ++i) {
                if (t <= ks.at(i).t) {
                    const Key &a = ks[i - 1];
                    const Key &b = ks[i];
                    double u = (t - a.t) / std::max(1e-6, b.t - a.t);
                    u = u * u * (3 - 2 * u); // smoothstep：起停都不生硬
                    zk = a.z + (b.z - a.z) * u;
                    fx = a.x + (b.x - a.x) * u;
                    fy = a.y + (b.y - a.y) * u;
                    break;
                }
            }
        }
        scale = zk * beatScale;
        focusX = box.x + box.w * fx;
        focusY = box.y + box.h * fy;
    }

    const double rad = deg * M_PI / 180.0;

    if (scale == 1 && px == 0 && py == 0 && rad == 0 &&
        std::fabs(focusX - (box.x + box.w / 2)) < 0.5 &&
        std::fabs(focusY - (box.y + box.h / 2)) < 0.5)
        return false;

    const double cx = box.x + box.w / 2;
    const double cy = box.y + box.h / 2;
    const double cosv = std::cos(rad) * scale;
    const double sinv = std::sin(rad) * scale;
    out[0] = cosv;
    out[1] = sinv;
    out[2] = -sinv;
    out[3] = cosv;
    // 绕"对焦点"缩放/旋转后再把这个点搬回画面中心（kp=画面中心，与旧行为一致）
    out[4] = cx - (out[0] * focusX + out[2] * focusY) + px * box.w;
    out[5] = cy - (out[1] * focusX + out[3] * focusY) + py * box.h;
    return true;
}

double transitionAlpha(const Shot &shot, double t)
{
    if (!shot.hasTransition)
        return 1;
    const QJsonObject &pr = shot.transition.params;
    double alpha = 1;
    const double inDur = pnum(pr, "inDur", 0);
    const double outDur = pnum(pr, "outDur", 0);
    if (pstr(pr, "in") == QLatin1String("fade") && inDur > 0)
        alpha = std::min(alpha, clamp01((t - shot.start) / inDur));
    if (pstr(pr, "out") == QLatin1String("fade") && outDur > 0)
        alpha = std::min(alpha, clamp01((shot.end - t) / outDur));
    return alpha;
}

void wipeRect(std::vector<lume::Draw> &draws, const Box &box, double k, const QString &dir,
              const QColor &color)
{
    if (k <= 0)
        return;
    lume::Draw d;
    d.kind = lume::Draw::Rect;
    d.x = box.x;
    d.y = box.y;
    d.w = box.w;
    d.h = box.h;
    if (dir == QLatin1String("right"))
        d.w = box.w * k;
    else if (dir == QLatin1String("left")) {
        d.w = box.w * k;
        d.x = box.x + box.w * (1 - k);
    } else if (dir == QLatin1String("up")) {
        d.h = box.h * k;
        d.y = box.y + box.h * (1 - k);
    } else
        d.h = box.h * k;
    d.color = color;
    draws.push_back(d);
}

/** flash（闪白）/ wipe（遮扫黑幕）—— 画在运镜之上，术曲PV硬切语言。 */
void compileTransitionFx(std::vector<lume::Draw> &draws, const Box &box, const Shot &shot, double t)
{
    if (!shot.hasTransition)
        return;
    const QJsonObject &pr = shot.transition.params;
    const double inDur = pnum(pr, "inDur", 0.3);
    const double outDur = pnum(pr, "outDur", 0.3);
    const QString kindIn = pstr(pr, "in", QStringLiteral("cut"));
    const QString kindOut = pstr(pr, "out", QStringLiteral("cut"));
    const QColor color = lume::parseColor(pstr(pr, "color", QStringLiteral("#ffffff")), Qt::white);

    if (kindIn == QLatin1String("flash") && inDur > 0 && t - shot.start < inDur) {
        lume::Draw d;
        d.kind = lume::Draw::Rect;
        d.x = box.x;
        d.y = box.y;
        d.w = box.w;
        d.h = box.h;
        d.color = color;
        d.alpha = 1 - (t - shot.start) / inDur;
        draws.push_back(d);
    }
    if (kindIn == QLatin1String("wipe") && inDur > 0 && t - shot.start < inDur) {
        const double k = clamp01((t - shot.start) / inDur);
        wipeRect(draws, box, 1 - k, pstr(pr, "dir", QStringLiteral("left")),
                 lume::parseColor(pstr(pr, "color", QStringLiteral("#000000")), Qt::black));
    }
    if (kindOut == QLatin1String("flash") && outDur > 0 && shot.end - t < outDur) {
        // 出场闪白：越接近切点越亮，最后一帧全白再接硬切。
        lume::Draw d;
        d.kind = lume::Draw::Rect;
        d.x = box.x;
        d.y = box.y;
        d.w = box.w;
        d.h = box.h;
        d.color = color;
        d.alpha = clamp01(1 - (shot.end - t) / outDur);
        draws.push_back(d);
    }
    if (kindOut == QLatin1String("wipe") && outDur > 0 && shot.end - t < outDur) {
        const double k = clamp01((shot.end - t) / outDur);
        wipeRect(draws, box, 1 - k, pstr(pr, "outDir", QStringLiteral("right")),
                 lume::parseColor(pstr(pr, "color", QStringLiteral("#000000")), Qt::black));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 镜头编译
// ═══════════════════════════════════════════════════════════════════════════

void compileIdle(std::vector<lume::Draw> &draws, const Box &box)
{
    const double size = std::round(box.h * 0.02);
    const QString font = QStringLiteral("%1px ui-monospace, monospace")
                             .arg(int(std::max(1.0, size)));
    const auto layout = lume::layoutRun({QStringLiteral("— 间奏 / 空镜 —")}, font, 0);
    lume::Draw d;
    d.kind = lume::Draw::Text;
    d.layout = layout;
    d.x = box.x + box.w / 2 - layout->totalWidth / 2;
    d.y = lume::baselineForMiddle(font, box.y + box.h / 2);
    d.style.fill = Qt::white;
    d.style.alpha = 0.10;
    draws.push_back(d);
}

void compileShot(Env &e, std::vector<lume::Draw> &draws, const Box &box, const Shot &shot, double t,
                 const QColor &accent)
{
    const Project &p = *e.p;
    std::vector<lume::Draw> inner;

    const double bpm = p.bpm > 0 ? p.bpm : 120;
    e.shotStart = shot.start; // 视频部件缺省从此刻开始播
    // 背景数组里允许混入图片：底图必须在文字之下，而 background 正是最早绘制的层。
    // （放 decor 会盖住文字 —— decor 在 text 之后绘制。）
    // 有一层不透明场景图铺满画幅时，它「之前」绘制的背景部件被彻底盖死：直接不编译，
    // 省掉时变背景每个刷新桶一次的整幅离屏重绘（暗场 bokeh/grid 最贵）。
    // 注意只跳它之前的：场景图之后常用一层渐变当压暗/文字衬底，那是看得见的。
    int coverIdx = -1;
    for (int i = 0; i < shot.background.size(); ++i) {
        if (paintsFullFrameOpaquely(e, shot.background.at(i))) {
            coverIdx = i;
            break;
        }
    }
    for (int i = 0; i < shot.background.size(); ++i) {
        const PartRef &part = shot.background.at(i);
        if (coverIdx >= 0 && i < coverIdx && part.part == QLatin1String("background"))
            continue;
        if (part.part == QLatin1String("image") || part.part == QLatin1String("imagePlate"))
            compileImagePart(e, inner, box, part);
        else if (part.part == QLatin1String("video"))
            compileVideoPart(e, inner, box, part, t);
        else if (part.part == QLatin1String("portrait"))
            compilePortraitPart(e, inner, box, part, t);
        else
            compileBackgroundPart(e, inner, box, part, t, bpm);
    }

    // 安全区折减：文字先按画幅排好、再被运镜整体缩放，fitting 必须预留运镜余量
    double zMax = 1;
    double punch = 0;
    if (shot.hasCamera) {
        const QJsonObject &camPr = shot.camera.params;
        const QJsonArray zoom = camPr.value(QLatin1String("zoom")).toArray();
        if (zoom.size() >= 2) {
            zMax = std::max({1.0, std::fabs(zoom.at(0).toDouble()), std::fabs(zoom.at(1).toDouble())});
        }
        // 关键帧路径：取整条路径上的最大放大倍率，否则推到最紧的那一下会把字切掉
        for (const QJsonValue &v : camPr.value(QLatin1String("keys")).toArray())
            zMax = std::max(zMax, std::fabs(v.toObject().value(QLatin1String("zoom")).toDouble(1.0)));
        punch = pnum(camPr, "punch", 0);
    }
    zMax *= (1 + punch);
    const double safe = 1.0 / zMax;

    if (!shot.lyricRefs.isEmpty()) {
        const size_t lyricMark = inner.size();
        for (const QString &ref : shot.lyricRefs) {
            const Lyric *ly = p.lyric(ref);
            if (ly && t >= ly->start && t < ly->end)
                compileLyric(inner, box, p, *ly, t, accent, shot, safe);
        }
        // 斜め歌詞：整行统一转，逐字状态（亮起/描边/残影）跟着一起走
        const LyricLayoutCfg lc = lyricLayoutOf(shot);
        wrapRotated(inner, lyricMark, lc.rotate, box.x + box.w * (lc.hasX ? lc.x : 0.5),
                    box.y + box.h * (lc.hasY ? lc.y : 0.5));
    }

    for (const PartRef &part : shot.text)
        compileTextPart(inner, box, part, t, safe);
    for (const PartRef &part : shot.decor)
        compileDecorPart(e, inner, box, part, t);
    for (const PartRef &part : shot.overlay)
        compileOverlayPart(e, inner, box, part, t, bpm);

    const double alpha = transitionAlpha(shot, t);
    double m[6];
    const bool hasCam = cameraMatrix(shot, t, box, p, m);
    if (hasCam) {
        lume::Draw d;
        d.kind = lume::Draw::Transform;
        d.a = m[0];
        d.b = m[1];
        d.c = m[2];
        d.d = m[3];
        d.e = m[4];
        d.f = m[5];
        d.alpha = alpha < 1 ? alpha : 1;
        d.sub = std::move(inner);
        draws.push_back(std::move(d));
    } else if (alpha < 1) {
        lume::Draw d;
        d.kind = lume::Draw::Transform;
        d.alpha = alpha;
        d.sub = std::move(inner);
        draws.push_back(std::move(d));
    } else {
        draws.insert(draws.end(), inner.begin(), inner.end());
    }

    compileTransitionFx(draws, box, shot, t);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// FrameRenderer
// ═══════════════════════════════════════════════════════════════════════════

struct FrameRenderer::Impl
{
    const Project *p = nullptr;
    lume::Renderer lume;
    std::unordered_map<QString, BgSlot> bg;
    std::unordered_map<QString, BgSlot> grain;
    std::unordered_map<QString, std::shared_ptr<QImage>> images;
    std::unordered_map<QString, std::shared_ptr<QImage>> tints;
    std::unordered_map<QString, VideoSlot> videos;
    qint64 bgRenders = 0;
};

FrameRenderer::FrameRenderer() : d(new Impl) {}

FrameRenderer::~FrameRenderer() = default;

void FrameRenderer::setProject(const Project *p)
{
    d->p = p;
    resetCaches();
}

void FrameRenderer::resetCaches()
{
    d->lume.resetCaches();
    d->bg.clear();
    d->grain.clear();
    d->images.clear();
    d->tints.clear();
    // 抽好的帧留在磁盘上（跨进程复用），这里只丢内存里的解码结果
    d->videos.clear();
    d->bgRenders = 0;
}

void FrameRenderer::compile(double t, std::vector<lume::Draw> &out, double w, double h)
{
    out.clear();
    if (!d->p)
        return;
    const Project &p = *d->p;
    Env env;
    env.p = &p;
    env.lume = &d->lume;
    env.bg = &d->bg;
    env.grain = &d->grain;
    env.images = &d->images;
    env.tints = &d->tints;
    env.videos = &d->videos;
    env.bgRenders = &d->bgRenders;

    const Box box{0, 0, w, h};
    const QVector<const Shot *> shots = activeShots(p, t);
    if (shots.isEmpty())
        compileIdle(out, box);
    else
        for (const Shot *s : shots)
            compileShot(env, out, box, *s, t, p.accent);
}

QImage FrameRenderer::render(double t, int outW, int outH, double *compileMs)
{
    QImage img;
    renderInto(img, t, outW, outH, compileMs);
    return img;
}

void FrameRenderer::renderInto(QImage &dst, double t, int outW, int outH, double *compileMs)
{
    if (!d->p)
        return;
    const Project &p = *d->p;
    const int W = outW > 0 ? outW : p.canvas.width;
    const int H = outH > 0 ? outH : p.canvas.height;
    if (W <= 0 || H <= 0)
        return;

    if (dst.width() != W || dst.height() != H ||
        dst.format() != QImage::Format_ARGB32_Premultiplied)
        dst = QImage(W, H, QImage::Format_ARGB32_Premultiplied);
    dst.fill(QColor("#08080b"));

    std::vector<lume::Draw> draws;
    QElapsedTimer compileClock;
    compileClock.start();
    compile(t, draws, double(W), double(H));
    if (compileMs)
        *compileMs = compileClock.nsecsElapsed() / 1e6;

    QPainter painter(&dst);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 画幅底色（镜头未提供背景部件时仍是工程底色）
    painter.fillRect(QRectF(0, 0, W, H), p.canvas.background);
    d->lume.paint(painter, draws, W, H);
    painter.end();
}

lume::Renderer::Stats FrameRenderer::cacheStats() const
{
    return d->lume.stats();
}

FrameRenderer::BgStats FrameRenderer::bgStats() const
{
    BgStats s;
    s.entries = int(d->bg.size());
    s.renders = d->bgRenders;
    return s;
}

} // namespace sf
