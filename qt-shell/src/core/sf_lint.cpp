#include "sf_lint.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace sf {
namespace {

/** 时间比较的容差：秒。生成器算出来的接缝常在 1e-9 级别抖动，不该报成问题。 */
constexpr double kEps = 1e-3;

QString shotWhere(const Shot &s, int index)
{
    return QStringLiteral("shot#%1 (%2 @%3s)")
        .arg(index)
        .arg(s.id.isEmpty() ? QStringLiteral("?") : s.id)
        .arg(QString::number(s.start, 'f', 2));
}

/** 部件参数里所有 `path` 字段（图片/视频帧都走它）。 */
void collectPaths(const QJsonValue &v, QStringList &out)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == QLatin1String("path") && it.value().isString())
                out << it.value().toString();
            else
                collectPaths(it.value(), out);
        }
    } else if (v.isArray()) {
        const QJsonArray a = v.toArray();
        for (const QJsonValue &x : a)
            collectPaths(x, out);
    }
}

void checkPart(const PartRef &part, const QString &where, const QString &projectDir,
               QVector<LintIssue> &out)
{
    if (!part.valid)
        return;
    QStringList paths;
    collectPaths(part.params, paths);
    for (const QString &rel : paths) {
        if (rel.isEmpty())
            continue;
        const QString abs = QFileInfo(rel).isAbsolute() ? rel : QDir(projectDir).filePath(rel);
        if (!QFileInfo::exists(abs))
            out.push_back({LintIssue::Error, where,
                           QStringLiteral("找不到文件 %1（%2）").arg(rel, abs)});
    }

    // 关键帧：t 必须递增且落在镜头内，否则运镜会跳。
    if (part.part == QLatin1String("camera") && part.params.contains(QLatin1String("keys"))) {
        const QJsonArray keys = part.params.value(QLatin1String("keys")).toArray();
        double last = -1e9;
        for (int i = 0; i < keys.size(); ++i) {
            const double t = keys.at(i).toObject().value(QLatin1String("t")).toDouble();
            if (t <= last)
                out.push_back({LintIssue::Warning, where,
                               QStringLiteral("camera.keys[%1] 的 t 没有递增（%2）")
                                   .arg(i)
                                   .arg(QString::number(t, 'f', 3))});
            last = t;
        }
    }
}

} // namespace

QVector<LintIssue> lintProject(const Project &p, const QString &projectDir)
{
    QVector<LintIssue> issues;

    if (p.canvas.width <= 0 || p.canvas.height <= 0)
        issues.push_back({LintIssue::Error, QStringLiteral("canvas"),
                          QStringLiteral("画布尺寸非法：%1x%2")
                              .arg(p.canvas.width)
                              .arg(p.canvas.height)});
    if (p.canvas.fps <= 0)
        issues.push_back({LintIssue::Error, QStringLiteral("canvas"),
                          QStringLiteral("fps 非法：%1").arg(p.canvas.fps)});

    if (!p.audioPath.isEmpty()) {
        const QString abs = QFileInfo(p.audioPath).isAbsolute()
                                ? p.audioPath
                                : QDir(projectDir).filePath(p.audioPath);
        if (!QFileInfo::exists(abs))
            issues.push_back({LintIssue::Error, QStringLiteral("media.audio"),
                              QStringLiteral("音轨缺失：%1").arg(abs)});
    }

    // 歌词：时间正向、id 唯一。
    QSet<QString> lyricIds;
    for (int i = 0; i < p.lyrics.size(); ++i) {
        const Lyric &l = p.lyrics[i];
        const QString where = QStringLiteral("lyrics[%1]").arg(i);
        if (l.end <= l.start + kEps)
            issues.push_back({LintIssue::Error, where,
                              QStringLiteral("行结束时间不晚于开始（%1 → %2）")
                                  .arg(l.start, 0, 'f', 2)
                                  .arg(l.end, 0, 'f', 2)});
        if (l.text.trimmed().isEmpty() && l.syllables.isEmpty())
            issues.push_back({LintIssue::Warning, where, QStringLiteral("空歌词行")});
        if (!l.id.isEmpty() && lyricIds.contains(l.id))
            issues.push_back({LintIssue::Error, where,
                              QStringLiteral("歌词 id 重复：%1").arg(l.id)});
        lyricIds.insert(l.id);
        for (int k = 0; k < l.syllables.size(); ++k)
            if (l.syllables[k].end <= l.syllables[k].start)
                issues.push_back({LintIssue::Warning, where,
                                  QStringLiteral("syllables[%1] 时长非正").arg(k)});
    }

    // 镜头：逐条校验，再按时间排序查接缝。
    for (int i = 0; i < p.shots.size(); ++i) {
        const Shot &s = p.shots[i];
        const QString where = shotWhere(s, i);
        const double dur = s.end - s.start;
        if (dur <= kEps) {
            issues.push_back({LintIssue::Error, where,
                              QStringLiteral("镜头时长非正（%1）").arg(dur)});
            continue;
        }
        for (const QString &ref : s.lyricRefs)
            if (!lyricIds.contains(ref))
                issues.push_back({LintIssue::Error, where,
                                  QStringLiteral("lyric_refs 指向不存在的歌词 %1").arg(ref)});

        const QVector<QVector<PartRef> *> layers = {
            const_cast<QVector<PartRef> *>(&s.background), const_cast<QVector<PartRef> *>(&s.text),
            const_cast<QVector<PartRef> *>(&s.decor), const_cast<QVector<PartRef> *>(&s.overlay)};
        static const char *kLayerNames[] = {"background", "text", "decor", "overlay"};
        for (int li = 0; li < layers.size(); ++li)
            for (const PartRef &part : *layers[li]) {
                checkPart(part, QStringLiteral("%1.%2").arg(where, kLayerNames[li]), projectDir,
                          issues);
                if (part.part == QLatin1String("text")
                    && part.params.value(QLatin1String("text")).toString().isEmpty())
                    issues.push_back({LintIssue::Warning,
                                      QStringLiteral("%1.%2").arg(where, kLayerNames[li]),
                                      QStringLiteral("文字部件内容为空")});
            }
        if (s.hasEntrance)
            checkPart(s.entrance, where + QStringLiteral(".entrance"), projectDir, issues);
        if (s.hasHold)
            checkPart(s.hold, where + QStringLiteral(".hold"), projectDir, issues);
        if (s.hasCamera)
            checkPart(s.camera, where + QStringLiteral(".camera"), projectDir, issues);
        if (s.hasTransition) {
            checkPart(s.transition, where + QStringLiteral(".transition"), projectDir, issues);
            const double inDur = s.transition.params.value(QLatin1String("inDur")).toDouble();
            const double outDur = s.transition.params.value(QLatin1String("outDur")).toDouble();
            if (inDur + outDur > dur + kEps)
                issues.push_back({LintIssue::Warning, where + QStringLiteral(".transition"),
                                  QStringLiteral("转场总时长 %1s 超过镜头时长 %2s")
                                      .arg(inDur + outDur, 0, 'f', 2)
                                      .arg(dur, 0, 'f', 2)});
        }
    }

    // 接缝：重叠 = 前一个镜头的尾巴被盖住；空隙 = 黑屏。
    // 只看同一条 track：多轨叠加本来就是靠不同 track 层叠，跨轨重叠不是问题。
    QHash<int, QVector<int>> byTrack;
    for (int i = 0; i < p.shots.size(); ++i)
        byTrack[p.shots[i].track].append(i);
    for (auto it = byTrack.begin(); it != byTrack.end(); ++it) {
        QVector<int> &order = it.value();
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return p.shots[a].start < p.shots[b].start; });
        for (int k = 1; k < order.size(); ++k) {
            const Shot &prev = p.shots[order[k - 1]];
            const Shot &cur = p.shots[order[k]];
            const double gap = cur.start - prev.end;
            if (gap > kEps)
                issues.push_back({LintIssue::Warning, shotWhere(cur, order[k]),
                                  QStringLiteral("track %1 上与上一镜之间有 %2s 空隙（会是黑场）")
                                      .arg(cur.track)
                                      .arg(gap, 0, 'f', 3)});
            else if (gap < -kEps)
                issues.push_back({LintIssue::Warning, shotWhere(cur, order[k]),
                                  QStringLiteral("track %1 上与上一镜重叠 %2s（前者的尾巴看不见）")
                                      .arg(cur.track)
                                      .arg(-gap, 0, 'f', 3)});
        }
    }

    return issues;
}

QString lintSummary(const QVector<LintIssue> &issues)
{
    int errors = 0;
    for (const LintIssue &i : issues)
        if (i.level == LintIssue::Error)
            ++errors;
    if (issues.isEmpty())
        return QStringLiteral("工程体检通过");
    return QStringLiteral("工程体检：%1 个错误，%2 个提示").arg(errors).arg(issues.size() - errors);
}

} // namespace sf
