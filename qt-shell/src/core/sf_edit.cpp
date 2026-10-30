/**
 * ShizukuFlow 编辑命令层实现（可撤销）
 *
 * 三条原则：
 *   1. 命令 = 纯数据（EditCmd），本层不存工程快照，只存「反向命令」
 *   2. 每个 doApply 要么整体成功、要么一个字段都不动（失败时工程必须保持原样）
 *   3. 时间永远先夹进空位再落盘：同 track 镜头不重叠、歌词允许重叠
 *
 * ShotRemove 的还原：EditCmd 只有 id/a/b 三个字段，装不下一个完整镜头的副本，
 * 而 sf_edit.h 的公开接口不允许改。所以删除时把镜头整份拷贝塞进本文件内的
 * 「镜头暂存箱」（token → 副本 + 原索引），反向命令写成
 *   ShotAdd{ id = "sfrm:<序号>", a = 原 start, b = 原时长 }
 * doApply 看到 "sfrm:" 前缀就走还原分支：按原索引插入、原样恢复所有字段与时间。
 */
#include "core/sf_edit.h"

#include <QHash>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sf {

namespace {

constexpr double kEps = 1e-6;
constexpr double kMinShot = 0.5;      // 新镜头最短时长
constexpr double kMinContrast = 0.15; // 入点/出点与另一端的最小间隔
constexpr int kBinCap = 512;          // 删除副本暂存上限（超出丢最旧的，还原会优雅失败）

const QString kRestorePrefix = QStringLiteral("sfrm:");

struct Span
{
    double s = 0;
    double e = 0;
};

QString labelOf(const char *verb, const QString &id)
{
    return QString::fromUtf8(verb) + QLatin1Char(' ') + id;
}

int shotIndexById(const Project &p, const QString &id)
{
    for (int i = 0; i < p.shots.size(); ++i)
        if (p.shots[i].id == id)
            return i;
    return -1;
}

int lyricIndexById(const Project &p, const QString &id)
{
    for (int i = 0; i < p.lyrics.size(); ++i)
        if (p.lyrics[i].id == id)
            return i;
    return -1;
}

/** 工程里镜头始终按 (track, start) 有序 —— 与 Project::load 的排序保持一致。 */
void sortShots(Project &p)
{
    std::stable_sort(p.shots.begin(), p.shots.end(), [](const Shot &a, const Shot &b) {
        return a.track != b.track ? a.track < b.track : a.start < b.start;
    });
}

bool shotIdNumber(const QString &id, int *n)
{
    if (!id.startsWith(QLatin1Char('S')) || id.size() < 2)
        return false;
    for (int i = 1; i < id.size(); ++i)
        if (!id.at(i).isDigit())
            return false;
    *n = id.mid(1).toInt();
    return true;
}

QString nextShotId(const Project &p)
{
    int maxN = 0;
    for (const Shot &s : p.shots) {
        int n = 0;
        if (shotIdNumber(s.id, &n))
            maxN = std::max(maxN, n);
    }
    return QStringLiteral("S%1").arg(maxN + 1, 3, 10, QLatin1Char('0'));
}

/** 同 track 上除 ignoreShotId 以外的占位区间，按 start 升序。 */
QVector<Span> otherSpans(const Project &p, int track, const QString &ignoreShotId)
{
    QVector<Span> out;
    for (const Shot &s : p.shots) {
        if (s.track != track)
            continue;
        if (!ignoreShotId.isEmpty() && s.id == ignoreShotId)
            continue;
        out.push_back(Span{s.start, s.end});
    }
    std::sort(out.begin(), out.end(), [](const Span &a, const Span &b) { return a.s < b.s; });
    return out;
}

bool hitsAny(const QVector<Span> &spans, double s, double e)
{
    for (const Span &sp : spans)
        if (s < sp.e - kEps && sp.s < e - kEps)
            return true;
    return false;
}

// ── 删除副本暂存箱 ────────────────────────────────────────────────────────────
struct BinEntry
{
    Shot shot;
    int index = -1;
};

QHash<QString, BinEntry> &shotBin()
{
    static QHash<QString, BinEntry> bin;
    return bin;
}

QStringList &shotBinOrder()
{
    static QStringList order;
    return order;
}

QString stashShot(const Shot &s, int index)
{
    static int seq = 0;
    const QString token = kRestorePrefix + QString::number(++seq);
    shotBin().insert(token, BinEntry{s, index});
    shotBinOrder().append(token);
    while (shotBinOrder().size() > kBinCap)
        shotBin().remove(shotBinOrder().takeFirst());
    return token;
}

const BinEntry *stashedShot(const QString &token)
{
    const auto it = shotBin().constFind(token);
    return it == shotBin().constEnd() ? nullptr : &it.value();
}

/** 清空暂存箱：历史一起清掉之后，token 不会再被任何 Entry 引用。 */
void clearShotBin()
{
    shotBin().clear();
    shotBinOrder().clear();
}

/** 同 track 上「出点 <= from」的最远出点；没有则返回 -1。 */
double prevEndOnTrack(const Project &p, int track, const QString &selfId, double from)
{
    double best = -1;
    for (const Shot &s : p.shots) {
        if (s.track != track || s.id == selfId)
            continue;
        if (s.end <= from + kEps)
            best = std::max(best, s.end);
    }
    return best;
}

// ── 部件层 ───────────────────────────────────────────────────────────────────
/** 层名 → 图层数组。层名写错返回 nullptr（宁可失败，也不要静默改错层）。 */
QVector<PartRef> *layerParts(Shot &s, const QString &layer)
{
    if (layer == QLatin1String("background"))
        return &s.background;
    if (layer == QLatin1String("decor"))
        return &s.decor;
    if (layer == QLatin1String("text"))
        return &s.text;
    if (layer == QLatin1String("overlay"))
        return &s.overlay;
    return nullptr;
}

QString layerTitle(const QString &layer)
{
    if (layer == QLatin1String("background"))
        return QStringLiteral("背景");
    if (layer == QLatin1String("decor"))
        return QStringLiteral("装饰");
    if (layer == QLatin1String("text"))
        return QStringLiteral("文字");
    if (layer == QLatin1String("overlay"))
        return QStringLiteral("覆盖");
    return layer;
}

/** 同 track 上「入点 >= from」的最近入点；没有则返回 +inf。 */
double nextStartOnTrack(const Project &p, int track, const QString &selfId, double from)
{
    double best = std::numeric_limits<double>::infinity();
    for (const Shot &s : p.shots) {
        if (s.track != track || s.id == selfId)
            continue;
        if (s.start >= from - kEps)
            best = std::min(best, s.start);
    }
    return best;
}

} // namespace

bool clampIntoFreeSlot(const Project &p, int track, const QString &ignoreShotId, double *s,
                       double *e)
{
    if (!s || !e)
        return false;

    const double len = *e - *s;
    if (!(len > 0))
        return false;

    const double hi = p.duration();
    if (!(hi > 0) || len > hi + kEps)
        return false;

    const QVector<Span> spans = otherSpans(p, track, ignoreShotId);

    // 先把整体框进 [0, duration]
    double start = *s;
    if (start < 0)
        start = 0;
    if (start + len > hi)
        start = hi - len;
    if (start < -kEps)
        return false;
    if (start < 0)
        start = 0;

    if (!hitsAny(spans, start, start + len)) {
        *s = start;
        *e = start + len;
        return true;
    }

    // ① 向右推：贴着每个障碍的后沿跳，直到落进空位（最近的右侧空位）
    double right = start;
    for (int guard = 0; guard <= spans.size(); ++guard) {
        bool hit = false;
        for (const Span &sp : spans) {
            if (right < sp.e - kEps && sp.s < right + len - kEps) {
                right = sp.e;
                hit = true;
            }
        }
        if (!hit)
            break;
    }
    if (right + len <= hi + kEps && !hitsAny(spans, right, right + len)) {
        *s = right;
        *e = right + len;
        return true;
    }

    // ② 推不动再向左推：贴着障碍的前沿退
    double left = start;
    for (int guard = 0; guard <= spans.size(); ++guard) {
        bool hit = false;
        for (const Span &sp : spans) {
            if (left < sp.e - kEps && sp.s < left + len - kEps) {
                left = sp.s - len;
                hit = true;
            }
        }
        if (!hit)
            break;
    }
    if (left >= -kEps && !hitsAny(spans, left, left + len)) {
        *s = std::max(0.0, left);
        *e = *s + len;
        return true;
    }

    return false; // 无处可放，*s/*e 原样不动
}

// ── ProjectEditor ─────────────────────────────────────────────────────────────

ProjectEditor::ProjectEditor(Project *project) : m_p(project) {}

void ProjectEditor::setProject(Project *p)
{
    m_p = p;
    clearHistory();
}

EditResult ProjectEditor::doApply(const EditCmd &cmd)
{
    EditResult r;
    r.inverse.kind = EditKind::None;

    if (!m_p) {
        r.note = QStringLiteral("没有绑定工程");
        return r;
    }
    Project &p = *m_p;

    switch (cmd.kind) {
    case EditKind::None:
        r.note = QStringLiteral("空命令");
        return r;

    case EditKind::ShotMove: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        const Shot &shot = p.shots[i];
        const double len = shot.end - shot.start;
        if (!(len > 0)) {
            r.note = QStringLiteral("镜头时长非法");
            return r;
        }
        double ns = cmd.a;
        double ne = cmd.a + len;
        if (!clampIntoFreeSlot(p, shot.track, shot.id, &ns, &ne)) {
            r.note = QStringLiteral("同轨没有能容纳该镜头的空位");
            return r;
        }
        const double oldStart = shot.start;
        const int track = shot.track;
        const QString id = shot.id;
        p.shots[i].start = ns;
        p.shots[i].end = ne;
        sortShots(p);
        r.ok = true;
        r.inverse = EditCmd{EditKind::ShotMove, id, oldStart, 0};
        r.label = labelOf("移动镜头", id);
        if (std::abs(ns - cmd.a) > kEps)
            r.note = QStringLiteral("已吸附到最近的空位（track %1，%2s 起）")
                         .arg(track)
                         .arg(ns, 0, 'f', 2);
        return r;
    }

    case EditKind::ShotTrimIn: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        const Shot &shot = p.shots[i];
        const QString id = shot.id;
        double ns = cmd.a;
        bool clamped = false;
        if (ns < 0) {
            ns = 0;
            clamped = true;
        }
        if (ns >= shot.end - kMinContrast) {
            r.note = QStringLiteral("入点必须早于出点 %1s 以上").arg(kMinContrast);
            return r;
        }
        const double prevEnd = prevEndOnTrack(p, shot.track, id, shot.start);
        if (prevEnd >= 0 && ns < prevEnd - kEps) {
            r.note = QStringLiteral("入点会与前一个镜头重叠（前镜头到 %1s）").arg(prevEnd, 0, 'f', 2);
            return r;
        }
        const double oldStart = shot.start;
        p.shots[i].start = ns;
        r.ok = true;
        r.inverse = EditCmd{EditKind::ShotTrimIn, id, oldStart, 0};
        r.label = labelOf("裁切入点", id);
        if (clamped)
            r.note = QStringLiteral("入点已夹到 0s");
        return r;
    }

    case EditKind::ShotTrimOut: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        const Shot &shot = p.shots[i];
        const QString id = shot.id;
        double ne = cmd.b;
        if (ne <= shot.start + kMinContrast) {
            r.note = QStringLiteral("出点必须晚于入点 %1s 以上").arg(kMinContrast);
            return r;
        }
        const double nextStart = nextStartOnTrack(p, shot.track, id, shot.end);
        if (ne > nextStart + kEps) {
            r.note = QStringLiteral("出点会与下一个镜头重叠（下镜头 %1s 起）")
                         .arg(nextStart, 0, 'f', 2);
            return r;
        }
        // 工程时长被音轨/后续镜头撑开时，出点不得越过它
        const double dur = p.duration();
        bool clamped = false;
        if (dur > shot.end + kEps && ne > dur) {
            ne = dur;
            clamped = true;
            if (ne <= shot.start + kMinContrast) {
                r.note = QStringLiteral("工程时长不够，出点无法延长");
                return r;
            }
        }
        const double oldEnd = shot.end;
        p.shots[i].end = ne;
        r.ok = true;
        r.inverse = EditCmd{EditKind::ShotTrimOut, id, 0, oldEnd};
        r.label = labelOf("裁切出点", id);
        if (clamped)
            r.note = QStringLiteral("出点已夹到工程时长 %1s").arg(ne, 0, 'f', 2);
        return r;
    }

    case EditKind::ShotAdd: {
        // ① 还原分支：删除时塞进暂存箱的那份副本
        if (cmd.id.startsWith(kRestorePrefix)) {
            const BinEntry *be = stashedShot(cmd.id);
            if (!be) {
                r.note = QStringLiteral("删除副本已失效，无法还原");
                return r;
            }
            const Shot copy = be->shot;
            const int at = std::clamp(be->index, 0, int(p.shots.size()));
            p.shots.insert(at, copy);
            sortShots(p);
            r.ok = true;
            r.inverse = EditCmd{EditKind::ShotRemove, copy.id, 0, 0};
            r.label = labelOf("还原镜头", copy.id);
            return r;
        }

        // ② 新建分支：在 a 处插一个空镜头，时长 b（最小 kMinShot）
        const double want = cmd.b;
        const double len = std::max(kMinShot, want);
        double ns = cmd.a;
        bool clamped = false;
        if (ns < 0) {
            ns = 0;
            clamped = true;
        }
        double ne = ns + len;
        if (!clampIntoFreeSlot(p, 0, QString(), &ns, &ne)) {
            r.note = QStringLiteral("track 0 没有能容纳新镜头的空位");
            return r;
        }
        if (std::abs(ns - cmd.a) > kEps)
            clamped = true;

        Shot s;
        s.id = nextShotId(p);
        s.start = ns;
        s.end = ne;
        s.track = 0;
        s.seed = int(std::lround(ns * 1000.0)); // a 换算的整数：毫秒偏移，够稳定也够区分
        // 其余字段保持默认：四个图层数组为空，entrance/hold/camera/transition 的
        // has* 均为 false（等价于 JSON 里的 null）
        p.shots.push_back(s);
        sortShots(p);

        r.ok = true;
        r.inverse = EditCmd{EditKind::ShotRemove, s.id, 0, 0};
        r.label = QStringLiteral("新建镜头");
        if (clamped)
            r.note = QStringLiteral("新镜头吸附到 %1s 起").arg(ns, 0, 'f', 2);
        if (want < kMinShot)
            r.note = QStringLiteral("时长不足 %1s，已按下限创建").arg(kMinShot);
        return r;
    }

    case EditKind::ShotRemove: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        const Shot copy = p.shots[i];
        const QString token = stashShot(copy, i); // 整份副本 + 原索引 → 反向命令的载体
        p.shots.remove(i);
        r.ok = true;
        r.inverse = EditCmd{EditKind::ShotAdd, token, copy.start, copy.end - copy.start};
        r.label = labelOf("删除镜头", cmd.id);
        return r;
    }

    case EditKind::LyricMove: {
        const int i = lyricIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到歌词 ") + cmd.id;
            return r;
        }
        Lyric &l = p.lyrics[i];
        const double len = l.end - l.start;
        if (!(len > 0)) {
            r.note = QStringLiteral("歌词时长非法");
            return r;
        }
        const double dur = p.duration();
        double ns = cmd.a;
        if (ns < 0)
            ns = 0;
        if (dur > 0 && ns + len > dur)
            ns = dur - len;
        if (ns < -kEps) {
            r.note = QStringLiteral("歌词比工程还长，放不下");
            return r;
        }
        if (ns < 0)
            ns = 0;
        const double oldStart = l.start;
        const QString id = l.id;
        l.start = ns;
        l.end = ns + len;
        r.ok = true;
        r.inverse = EditCmd{EditKind::LyricMove, id, oldStart, 0};
        r.label = labelOf("移动歌词", id);
        if (std::abs(ns - cmd.a) > kEps)
            r.note = QStringLiteral("已夹到 %1s（0 ~ 工程时长内）").arg(ns, 0, 'f', 2);
        return r;
    }

    case EditKind::LyricTrimIn: {
        const int i = lyricIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到歌词 ") + cmd.id;
            return r;
        }
        Lyric &l = p.lyrics[i];
        const double ns = cmd.a;
        if (ns < -kEps) {
            r.note = QStringLiteral("歌词入点不能为负");
            return r;
        }
        if (ns >= l.end - kMinContrast) {
            r.note = QStringLiteral("入点必须早于出点 %1s 以上").arg(kMinContrast);
            return r;
        }
        const double oldStart = l.start;
        const QString id = l.id;
        l.start = std::max(0.0, ns);
        r.ok = true;
        r.inverse = EditCmd{EditKind::LyricTrimIn, id, oldStart, 0};
        r.label = labelOf("裁切歌词入点", id);
        return r;
    }

    case EditKind::LyricTrimOut: {
        const int i = lyricIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到歌词 ") + cmd.id;
            return r;
        }
        Lyric &l = p.lyrics[i];
        const double ne = cmd.b;
        if (ne <= l.start + kMinContrast) {
            r.note = QStringLiteral("出点必须晚于入点 %1s 以上").arg(kMinContrast);
            return r;
        }
        const double dur = p.duration();
        if (dur > 0 && ne > dur + kEps) {
            r.note = QStringLiteral("出点超出工程时长 %1s").arg(dur, 0, 'f', 2);
            return r;
        }
        const double oldEnd = l.end;
        const QString id = l.id;
        l.end = ne;
        r.ok = true;
        r.inverse = EditCmd{EditKind::LyricTrimOut, id, 0, oldEnd};
        r.label = labelOf("裁切歌词出点", id);
        return r;
    }

    case EditKind::ShotTrack: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        const Shot copy = p.shots[i];
        const int newTrack = int(std::lround(cmd.a));
        if (newTrack < 0) {
            r.note = QStringLiteral("轨道号不能为负");
            return r;
        }
        const double len = copy.end - copy.start;
        if (!(len > 0)) {
            r.note = QStringLiteral("镜头时长非法");
            return r;
        }
        double ns = copy.start;
        if (cmd.payload.contains(QLatin1String("start")))
            ns = cmd.payload.value(QLatin1String("start")).toDouble();
        double ne = ns + len;
        if (!clampIntoFreeSlot(p, newTrack, copy.id, &ns, &ne)) {
            r.note = QStringLiteral("track %1 没有能容纳该镜头的空位").arg(newTrack);
            return r;
        }
        if (newTrack == copy.track && std::abs(ns - copy.start) < kEps) {
            r.note = QStringLiteral("镜头已在该位置的轨道上");
            return r;
        }
        p.shots[i].track = newTrack;
        p.shots[i].start = ns;
        p.shots[i].end = ne;
        sortShots(p);
        r.ok = true;
        r.inverse = EditCmd{EditKind::ShotTrack, copy.id, double(copy.track), 0, 0, QString(),
                            QJsonObject{{QStringLiteral("start"), copy.start}}};
        r.label = labelOf("移动镜头到 track", QString::number(newTrack));
        if (std::abs(ns - cmd.a) > kEps && !cmd.payload.contains(QLatin1String("start")))
            r.note = QStringLiteral("已吸附到空位（track %1，%2s 起）")
                         .arg(newTrack)
                         .arg(ns, 0, 'f', 2);
        return r;
    }

    case EditKind::PartMove: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        QVector<PartRef> *v = layerParts(p.shots[i], cmd.layer);
        if (!v) {
            r.note = QStringLiteral("未知图层「%1」").arg(cmd.layer);
            return r;
        }
        const int n = v->size();
        if (n == 0) {
            r.note = QStringLiteral("%1层没有部件").arg(layerTitle(cmd.layer));
            return r;
        }
        const int from = int(std::lround(cmd.a));
        int to = int(std::lround(cmd.b));
        if (from < 0 || from >= n) {
            r.note = QStringLiteral("部件下标越界（%1 / %2）").arg(from).arg(n);
            return r;
        }
        to = std::clamp(to, 0, n - 1);
        if (to == from) {
            r.note = QStringLiteral("部件位置未变");
            return r;
        }
        const PartRef moved = v->at(from);
        v->remove(from);
        v->insert(to, moved);
        r.ok = true;
        r.inverse = EditCmd{EditKind::PartMove, cmd.id, double(to), double(from), 0, cmd.layer,
                            QJsonObject()};
        r.label = QStringLiteral("调整%1层序").arg(layerTitle(cmd.layer));
        return r;
    }

    case EditKind::PartRemove: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        QVector<PartRef> *v = layerParts(p.shots[i], cmd.layer);
        if (!v) {
            r.note = QStringLiteral("未知图层「%1」").arg(cmd.layer);
            return r;
        }
        const int idx = int(std::lround(cmd.a));
        if (idx < 0 || idx >= v->size()) {
            r.note = QStringLiteral("部件下标越界（%1 / %2）").arg(idx).arg(v->size());
            return r;
        }
        const PartRef removed = v->at(idx);
        v->remove(idx);
        QJsonObject pl;
        pl.insert(QStringLiteral("part"), removed.part);
        pl.insert(QStringLiteral("params"), removed.params);
        pl.insert(QStringLiteral("valid"), removed.valid);
        r.ok = true;
        r.inverse = EditCmd{EditKind::PartInsert, cmd.id, double(idx), 0, 0, cmd.layer, pl};
        r.label = QStringLiteral("删除%1部件").arg(layerTitle(cmd.layer));
        return r;
    }

    case EditKind::PartInsert: {
        const int i = shotIndexById(p, cmd.id);
        if (i < 0) {
            r.note = QStringLiteral("找不到镜头 ") + cmd.id;
            return r;
        }
        QVector<PartRef> *v = layerParts(p.shots[i], cmd.layer);
        if (!v) {
            r.note = QStringLiteral("未知图层「%1」").arg(cmd.layer);
            return r;
        }
        const QString name = cmd.payload.value(QLatin1String("part")).toString();
        if (name.isEmpty()) {
            r.note = QStringLiteral("缺少部件名");
            return r;
        }
        int idx = int(std::lround(cmd.a));
        idx = std::clamp(idx, 0, int(v->size()));
        PartRef pr;
        pr.part = name;
        pr.params = cmd.payload.value(QLatin1String("params")).toObject();
        // 手工构造的命令可能没带 valid：默认按有效部件插入
        pr.valid = cmd.payload.contains(QLatin1String("valid"))
                       ? cmd.payload.value(QLatin1String("valid")).toBool()
                       : true;
        v->insert(idx, pr);
        r.ok = true;
        r.inverse = EditCmd{EditKind::PartRemove, cmd.id, double(idx), 0, 0, cmd.layer,
                            QJsonObject()};
        r.label = QStringLiteral("插入%1部件").arg(layerTitle(cmd.layer));
        return r;
    }

    case EditKind::ShotText:
        r.note = QStringLiteral("未实现");
        return r;
    }

    r.note = QStringLiteral("未知命令");
    return r;
}

EditResult ProjectEditor::apply(const EditCmd &cmd)
{
    EditResult r = doApply(cmd);
    if (!r.ok)
        return r;

    Entry e;
    e.cmds.push_back(cmd);
    e.inverses.push_back(r.inverse.kind == EditKind::None ? EditCmd() : r.inverse);
    e.label = r.label;
    m_undo.push_back(e);
    m_redo.clear();
    return r;
}

bool ProjectEditor::applyBatch(const QVector<EditCmd> &cmds, const QString &label)
{
    if (!m_p || cmds.isEmpty())
        return false;

    QVector<EditCmd> inverses;
    inverses.reserve(cmds.size());
    for (const EditCmd &c : cmds) {
        const EditResult r = doApply(c);
        if (!r.ok) {
            // 整批不留半截：已执行的按逆序回滚，工程回到调用前
            for (int k = inverses.size() - 1; k >= 0; --k) {
                if (inverses[k].kind != EditKind::None)
                    doApply(inverses[k]);
            }
            return false;
        }
        inverses.push_back(r.inverse.kind == EditKind::None ? EditCmd() : r.inverse);
    }

    Entry e;
    e.cmds = cmds;
    e.inverses = inverses;
    e.label = label.isEmpty() ? QStringLiteral("批量编辑") : label;
    m_undo.push_back(e);
    m_redo.clear();
    return true;
}

bool ProjectEditor::undo()
{
    if (!m_p || m_undo.isEmpty())
        return false;
    const Entry e = m_undo.last();
    if (e.inverses.isEmpty())
        return false;

    for (int k = e.inverses.size() - 1; k >= 0; --k) {
        if (e.inverses[k].kind == EditKind::None)
            continue;
        if (!doApply(e.inverses[k]).ok) {
            // 回滚失败：把已经退掉的那些按正序重做回来，保持「要么全退、要么全不退」
            for (int j = k + 1; j < e.cmds.size(); ++j)
                doApply(e.cmds[j]);
            return false;
        }
    }
    m_undo.removeLast();
    m_redo.push_back(e);
    return true;
}

bool ProjectEditor::redo()
{
    if (!m_p || m_redo.isEmpty())
        return false;
    Entry e = m_redo.last();
    QVector<EditCmd> inverses;
    inverses.reserve(e.cmds.size());

    for (const EditCmd &c : e.cmds) {
        const EditResult r = doApply(c);
        if (!r.ok) {
            for (int k = inverses.size() - 1; k >= 0; --k) {
                if (inverses[k].kind != EditKind::None)
                    doApply(inverses[k]);
            }
            return false;
        }
        inverses.push_back(r.inverse.kind == EditKind::None ? EditCmd() : r.inverse);
    }

    m_redo.removeLast();
    // 重做是「重新执行」，反向数据要换成这一次拿到的（例如删除副本的新 token）
    e.inverses = inverses;
    m_undo.push_back(e);
    return true;
}

QString ProjectEditor::undoLabel() const
{
    return m_undo.isEmpty() ? QString() : m_undo.last().label;
}

QString ProjectEditor::redoLabel() const
{
    return m_redo.isEmpty() ? QString() : m_redo.last().label;
}

void ProjectEditor::clearHistory()
{
    m_undo.clear();
    m_redo.clear();
    clearShotBin();
}

} // namespace sf
