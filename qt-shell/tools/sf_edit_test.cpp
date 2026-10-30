/**
 * sf_edit_test —— 编辑命令层的无界面单元测试
 *
 * 不引入任何测试框架：CHECK 宏 + 失败计数，退出码 0/1。
 * 每个用例都用一份全新的内存工程，互不污染。
 *
 * 初始工程（duration = 30s，由 audioDuration 撑开）：
 *   镜头 track0: S001 [0,5)  S002 [5,10)  S003 [10,15)
 *   歌词:       L001 [0,7)   L002 [6,12)   ← 故意重叠（合唱）
 */
#include "core/sf_edit.h"
#include "core/sf_project.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_pass;                                                                              \
        } else {                                                                                   \
            ++g_fail;                                                                              \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                         \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b)                                                                           \
    do {                                                                                           \
        const double va = (a);                                                                     \
        const double vb = (b);                                                                     \
        if (std::fabs(va - vb) < 1e-6) {                                                           \
            ++g_pass;                                                                              \
        } else {                                                                                   \
            ++g_fail;                                                                              \
            std::printf("  FAIL  %s:%d  %s = %.4f != %.4f\n", __FILE__, __LINE__, #a, va, vb);     \
        }                                                                                          \
    } while (0)

namespace {

void section(const char *name)
{
    std::printf("\n== %s\n", name);
}

QString num(double v)
{
    return QString::number(v, 'f', 5);
}

QString partSig(const QVector<sf::PartRef> &v)
{
    QStringList parts;
    for (const sf::PartRef &p : v)
        parts << p.part + (p.valid ? QStringLiteral("+") : QStringLiteral("-"));
    return QStringLiteral("%1{%2}").arg(v.size()).arg(parts.join(QLatin1Char(',')));
}

QString motionSig(const sf::PartRef &p, bool has)
{
    return (has ? QStringLiteral("1:") : QStringLiteral("0:")) + p.part;
}

/** 工程的完整文本指纹：字段级 + 顺序级，用来做「逐字段一致」的断言。 */
QString signature(const sf::Project &p)
{
    QStringList lines;
    lines << QStringLiteral("duration=") + num(p.duration());
    for (const sf::Shot &s : p.shots) {
        QStringList f;
        f << s.id << num(s.start) << num(s.end) << QString::number(s.track)
          << QString::number(s.seed) << s.lyricRefs.join(QLatin1Char(','))
          << partSig(s.background) << partSig(s.text) << partSig(s.decor)
          << partSig(s.overlay) << motionSig(s.entrance, s.hasEntrance)
          << motionSig(s.hold, s.hasHold) << motionSig(s.camera, s.hasCamera)
          << motionSig(s.transition, s.hasTransition);
        lines << QStringLiteral("shot|") + f.join(QLatin1Char('|'));
    }
    for (const sf::Lyric &l : p.lyrics) {
        QStringList syl;
        for (const sf::Syllable &sy : l.syllables)
            syl << sy.text + QLatin1Char(':') + num(sy.start) + QLatin1Char('-') + num(sy.end);
        QStringList f;
        f << l.id << num(l.start) << num(l.end) << l.text << l.style
          << syl.join(QLatin1Char(','));
        lines << QStringLiteral("lyric|") + f.join(QLatin1Char('|'));
    }
    return lines.join(QLatin1Char('\n'));
}

sf::Shot makeShot(const QString &id, double start, double end, int track = 0)
{
    sf::Shot s;
    s.id = id;
    s.start = start;
    s.end = end;
    s.track = track;
    s.seed = 100 + int(start);
    return s;
}

sf::Lyric makeLyric(const QString &id, double start, double end, const QString &text)
{
    sf::Lyric l;
    l.id = id;
    l.start = start;
    l.end = end;
    l.text = text;
    l.style = QStringLiteral("main");
    sf::Syllable sy;
    sy.text = text;
    sy.start = start;
    sy.end = end;
    l.syllables.push_back(sy);
    return l;
}

sf::Project makeProject()
{
    sf::Project p;
    p.audioDuration = 30; // 撑开 duration()，让镜头后面有可用的空位
    p.shots.push_back(makeShot(QStringLiteral("S001"), 0, 5));
    p.shots.push_back(makeShot(QStringLiteral("S002"), 5, 10));
    p.shots.push_back(makeShot(QStringLiteral("S003"), 10, 15));
    p.lyrics.push_back(makeLyric(QStringLiteral("L001"), 0, 7, QStringLiteral("一")));
    p.lyrics.push_back(makeLyric(QStringLiteral("L002"), 6, 12, QStringLiteral("二")));
    return p;
}

const sf::Shot *findShot(const sf::Project &p, const QString &id)
{
    for (const sf::Shot &s : p.shots)
        if (s.id == id)
            return &s;
    return nullptr;
}

bool anyOverlap(const sf::Project &p)
{
    for (int i = 0; i < p.shots.size(); ++i) {
        for (int j = i + 1; j < p.shots.size(); ++j) {
            const sf::Shot &a = p.shots[i];
            const sf::Shot &b = p.shots[j];
            if (a.track != b.track)
                continue;
            if (a.start < b.end - 1e-6 && b.start < a.end - 1e-6)
                return true;
        }
    }
    return false;
}

sf::EditCmd moveCmd(const QString &id, double a)
{
    return sf::EditCmd{sf::EditKind::ShotMove, id, a, 0};
}

// ─────────────────────────────────────────────────────────────────────────────

/** 1. 移动到合法位置 → ok，start 变了，undo 后完全还原 */
void testMoveLegal()
{
    section("1) move to a free slot, then undo");
    sf::Project p = makeProject();
    const QString before = signature(p);
    sf::ProjectEditor ed(&p);

    const sf::EditResult r = ed.apply(moveCmd(QStringLiteral("S001"), 20));
    CHECK(r.ok);
    CHECK(r.inverse.kind == sf::EditKind::ShotMove);
    CHECK(r.inverse.a == 0);
    CHECK(ed.undoLabel() == QStringLiteral("移动镜头 S001"));
    const sf::Shot *s = findShot(p, QStringLiteral("S001"));
    CHECK(s != nullptr);
    CHECK_NEAR(s->start, 20);
    CHECK_NEAR(s->end, 25); // 时长不变
    CHECK(ed.undoDepth() == 1);
    CHECK(!anyOverlap(p));

    CHECK(ed.undo());
    CHECK(signature(p) == before);
    CHECK(!ed.canUndo());
    CHECK(ed.canRedo());
    CHECK(ed.undoDepth() == 0);

    CHECK(ed.redo());
    const sf::Shot *s2 = findShot(p, QStringLiteral("S001"));
    CHECK(s2 != nullptr && std::fabs(s2->start - 20) < 1e-6);
}

/** 2. 移动到会重叠的位置 → 被推到空位，且不产生重叠 */
void testMoveClamped()
{
    section("2) move onto an occupied slot -> snapped, never overlapping");
    sf::Project p = makeProject();
    sf::ProjectEditor ed(&p);

    // S002 [5,10) → 12：与 S003 [10,15) 撞上，向右推到 15
    sf::EditResult r = ed.apply(moveCmd(QStringLiteral("S002"), 12));
    CHECK(r.ok);
    CHECK(!r.note.isEmpty()); // 有 clamp 说明
    const sf::Shot *s = findShot(p, QStringLiteral("S002"));
    CHECK(s != nullptr);
    CHECK(s->start > 12 - 1e-6); // 只能被推走，不能原地落下
    CHECK_NEAR(s->end - s->start, 5);
    CHECK(!anyOverlap(p));
    CHECK(ed.undo());

    // S003 [10,15) → 26：整体越过 duration(30)，先夹到 25，正好落进空位
    r = ed.apply(moveCmd(QStringLiteral("S003"), 26));
    CHECK(r.ok);
    const sf::Shot *s3 = findShot(p, QStringLiteral("S003"));
    CHECK(s3 != nullptr);
    CHECK(s3->end <= p.duration() + 1e-6);
    CHECK_NEAR(s3->start, 25);
    CHECK(!anyOverlap(p));
    CHECK(ed.undo());

    CHECK(signature(p) == signature(makeProject()));
}

/** 3. trim 入点越界（超过 end）→ 失败且工程未变 */
void testTrimInInvalid()
{
    section("3) trim-in past the out point -> fail, project untouched");
    sf::Project p = makeProject();
    const QString before = signature(p);
    sf::ProjectEditor ed(&p);

    // S002 = [5,10)，出点上限 9.85s；9.9 越界
    const sf::EditResult r = ed.apply(sf::EditCmd{sf::EditKind::ShotTrimIn, QStringLiteral("S002"), 9.9, 0});
    CHECK(!r.ok);
    CHECK(!r.note.isEmpty());
    CHECK(r.inverse.kind == sf::EditKind::None);
    CHECK(signature(p) == before);
    CHECK(!ed.canUndo());

    // 入点侵入前一个镜头 [0,5) 也必须被拒
    const sf::EditResult r2 =
        ed.apply(sf::EditCmd{sf::EditKind::ShotTrimIn, QStringLiteral("S002"), 3.0, 0});
    CHECK(!r2.ok);
    CHECK(signature(p) == before);

    // 合法入点则生效，且 undo 精确回滚
    const sf::EditResult r3 =
        ed.apply(sf::EditCmd{sf::EditKind::ShotTrimIn, QStringLiteral("S002"), 6.0, 0});
    CHECK(r3.ok);
    CHECK(ed.undoLabel() == QStringLiteral("裁切入点 S002"));
    const sf::Shot *s = findShot(p, QStringLiteral("S002"));
    CHECK(s != nullptr && std::fabs(s->start - 6.0) < 1e-6);
    CHECK(ed.undo());
    CHECK(signature(p) == before);
}

/** 4. 删除镜头 → undo 后回到原位置、原顺序、原内容 */
void testRemoveUndo()
{
    section("4) remove a shot, undo restores position/order/content");
    sf::Project p = makeProject();
    // 给 S002 加点内容，验证副本是整份还原而不是重建一个空镜头
    p.shots[1].lyricRefs << QStringLiteral("L001");
    sf::PartRef bg;
    bg.part = QStringLiteral("gradient");
    bg.valid = true;
    p.shots[1].background.push_back(bg);
    p.shots[1].hasTransition = true;
    p.shots[1].transition.part = QStringLiteral("fade");
    const QString before = signature(p);

    sf::ProjectEditor ed(&p);
    const sf::EditResult r = ed.apply(sf::EditCmd{sf::EditKind::ShotRemove, QStringLiteral("S002"), 0, 0});
    CHECK(r.ok);
    CHECK(ed.undoLabel() == QStringLiteral("删除镜头 S002"));
    CHECK(p.shots.size() == 2);
    CHECK(findShot(p, QStringLiteral("S002")) == nullptr);
    CHECK(r.inverse.kind == sf::EditKind::ShotAdd);

    CHECK(ed.undo());
    CHECK(p.shots.size() == 3);
    CHECK(signature(p) == before); // 位置、顺序、图层、转场一起回来
    CHECK(p.shots[1].id == QStringLiteral("S002"));

    // redo 再删一次，然后再 undo —— 反向数据必须在 redo 时刷新，不能只成功一次
    CHECK(ed.redo());
    CHECK(p.shots.size() == 2);
    CHECK(ed.undo());
    CHECK(signature(p) == before);
}

/** 5. ShotAdd 后再 undo → 镜头数回到原值 */
void testAddUndo()
{
    section("5) add a shot, undo -> shot count back to original");
    sf::Project p = makeProject();
    const QString before = signature(p);
    sf::ProjectEditor ed(&p);

    const sf::EditResult r = ed.apply(sf::EditCmd{sf::EditKind::ShotAdd, QString(), 16, 3});
    CHECK(r.ok);
    CHECK(ed.undoLabel() == QStringLiteral("新建镜头"));
    CHECK(p.shots.size() == 4);
    const sf::Shot *s = findShot(p, QStringLiteral("S004")); // S + 最大编号 + 1
    CHECK(s != nullptr);
    CHECK(s != nullptr && s->track == 0);
    CHECK(s != nullptr && s->background.isEmpty() && s->text.isEmpty() && s->decor.isEmpty()
          && s->overlay.isEmpty());
    CHECK(s != nullptr && !s->hasEntrance && !s->hasHold && !s->hasCamera && !s->hasTransition);
    CHECK(!anyOverlap(p));

    CHECK(ed.undo());
    CHECK(p.shots.size() == 3);
    CHECK(signature(p) == before);

    // 时长下限：b 太小就按 0.5s 造
    const sf::EditResult r2 = ed.apply(sf::EditCmd{sf::EditKind::ShotAdd, QString(), 16, 0.1});
    CHECK(r2.ok);
    const sf::Shot *s2 = findShot(p, QStringLiteral("S004"));
    CHECK(s2 != nullptr && s2->end - s2->start >= 0.5 - 1e-6);
}

/** 6. 连续 apply 三条命令 → undo 三次 → 与初始逐字段一致 */
void testThreeCommands()
{
    section("6) three commands, three undos -> field-identical to initial");
    sf::Project p = makeProject();
    const QString before = signature(p);
    sf::ProjectEditor ed(&p);

    CHECK(ed.apply(moveCmd(QStringLiteral("S001"), 20)).ok);
    CHECK(ed.apply(sf::EditCmd{sf::EditKind::ShotTrimIn, QStringLiteral("S001"), 21, 0}).ok);
    CHECK(ed.apply(sf::EditCmd{sf::EditKind::ShotTrimOut, QStringLiteral("S002"), 0, 9}).ok);
    CHECK(ed.undoDepth() == 3);
    CHECK(signature(p) != before);

    CHECK(ed.undo());
    CHECK(ed.undo());
    CHECK(ed.undo());
    CHECK(!ed.canUndo());
    CHECK(ed.undoDepth() == 0);
    CHECK(signature(p) == before);

    // 三条 redo 应该回到第三条命令之后的状态
    CHECK(ed.redo());
    CHECK(ed.redo());
    CHECK(ed.redo());
    CHECK(!ed.canRedo());
    CHECK(ed.undoDepth() == 3);
    CHECK(ed.undo());
    CHECK(ed.undo());
    CHECK(ed.undo());
    CHECK(signature(p) == before);
}

/** 7. 歌词 trim 不受「不重叠」约束（合唱会重叠） */
void testLyricOverlapAllowed()
{
    section("7) lyric trim ignores overlap with other lyrics");
    sf::Project p = makeProject();
    sf::ProjectEditor ed(&p);

    // L002 [6,12) 的入点拉到 5 → 与 L001 [0,7) 大段重叠，也必须成功
    const sf::EditResult r =
        ed.apply(sf::EditCmd{sf::EditKind::LyricTrimIn, QStringLiteral("L002"), 5, 0});
    CHECK(r.ok);
    CHECK(ed.undoLabel() == QStringLiteral("裁切歌词入点 L002"));
    const sf::Lyric *l2 = p.lyric(QStringLiteral("L002"));
    CHECK(l2 != nullptr && std::fabs(l2->start - 5.0) < 1e-6);
    const sf::Lyric *l1 = p.lyric(QStringLiteral("L001"));
    CHECK(l1 != nullptr && std::fabs(l1->start - 0.0) < 1e-6 && std::fabs(l1->end - 7.0) < 1e-6);

    // 出点延长到 13 也允许（不撞任何歌词的规则），但不得超过工程时长
    CHECK(ed.apply(sf::EditCmd{sf::EditKind::LyricTrimOut, QStringLiteral("L002"), 0, 13}).ok);
    CHECK(!ed.apply(sf::EditCmd{sf::EditKind::LyricTrimOut, QStringLiteral("L002"), 0, 31}).ok);

    // 移动也不受重叠限制
    const sf::EditResult mv =
        ed.apply(sf::EditCmd{sf::EditKind::LyricMove, QStringLiteral("L001"), 3, 0});
    CHECK(mv.ok);
    CHECK(p.lyric(QStringLiteral("L001")) != nullptr
          && std::fabs(p.lyric(QStringLiteral("L001"))->start - 3.0) < 1e-6);

    CHECK(ed.undo());
    CHECK(ed.undo());
    CHECK(ed.undo());
    CHECK(signature(p) == signature(makeProject()));
}

// ── 8) 落盘往返 ───────────────────────────────────────────────────────────────

QString jsonCompact(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

bool partsEqual(const QVector<sf::PartRef> &x, const QVector<sf::PartRef> &y)
{
    if (x.size() != y.size())
        return false;
    for (int i = 0; i < x.size(); ++i) {
        if (x.at(i).part != y.at(i).part || x.at(i).valid != y.at(i).valid)
            return false;
        if (jsonCompact(x.at(i).params) != jsonCompact(y.at(i).params))
            return false;
    }
    return true;
}

QString partDeep(const sf::PartRef &p, bool has)
{
    if (!has)
        return QStringLiteral("-");
    return QStringLiteral("1:") + p.part + QLatin1Char('{') + jsonCompact(p.params)
           + QLatin1Char('}');
}

/** 比 signature() 更狠的指纹：连 params 里的每个键、每个音节、样式每个字段都比。 */
QString deepSignature(const sf::Project &p)
{
    const auto n = [](double v) { return QString::number(v, 'g', 17); };
    QStringList lines;
    lines << QStringList{QStringLiteral("meta"), p.title, p.artist,
                         p.accent.name(QColor::HexArgb)}
                 .join(QLatin1Char('|'));
    lines << QStringList{QStringLiteral("canvas"), QString::number(p.canvas.width),
                         QString::number(p.canvas.height), QString::number(p.canvas.fps),
                         p.canvas.background.name(QColor::HexArgb),
                         p.canvas.safeAreaEnabled ? QStringLiteral("1") : QStringLiteral("0"),
                         n(p.canvas.safeAreaMargin)}
                 .join(QLatin1Char('|'));
    lines << QStringList{QStringLiteral("media"), p.audioPath, n(p.audioDuration),
                         QStringLiteral("bpm"), n(p.bpm), n(p.offset)}
                 .join(QLatin1Char('|'));
    for (const sf::StyleDef &s : p.styles) {
        lines << QStringList{QStringLiteral("style"),
                             s.id,
                             s.font,
                             QString::number(s.weight),
                             n(s.size),
                             n(s.letterSpacing),
                             n(s.lineHeight),
                             s.fill.name(QColor::HexArgb),
                             n(s.y),
                             s.hasStroke ? QStringLiteral("1") : QStringLiteral("0"),
                             s.strokeColor.name(QColor::HexArgb),
                             n(s.strokeWidth),
                             s.hasShadow ? QStringLiteral("1") : QStringLiteral("0"),
                             s.shadowColor.name(QColor::HexArgb),
                             n(s.shadowBlur),
                             n(s.shadowOffX),
                             n(s.shadowOffY)}
                     .join(QLatin1Char('|'));
    }
    for (const sf::Lyric &l : p.lyrics) {
        QStringList syl;
        for (const sf::Syllable &sy : l.syllables)
            syl << sy.text + QLatin1Char('/') + n(sy.start) + QLatin1Char('/') + n(sy.end);
        lines << QStringList{QStringLiteral("lyric"), l.id, n(l.start), n(l.end), l.text, l.style,
                             QString::number(l.syllables.size()), syl.join(QLatin1Char(';'))}
                     .join(QLatin1Char('|'));
    }
    for (const sf::Shot &s : p.shots) {
        lines << QStringList{QStringLiteral("shot"),
                             s.id,
                             n(s.start),
                             n(s.end),
                             QString::number(s.track),
                             QString::number(s.seed),
                             s.lyricRefs.join(QLatin1Char(',')),
                             QString::number(s.background.size()),
                             QString::number(s.text.size()),
                             QString::number(s.decor.size()),
                             QString::number(s.overlay.size()),
                             partDeep(s.entrance, s.hasEntrance),
                             partDeep(s.hold, s.hasHold),
                             partDeep(s.camera, s.hasCamera),
                             partDeep(s.transition, s.hasTransition)}
                     .join(QLatin1Char('|'));
        const auto listDeep = [&lines](const char *tag, const QVector<sf::PartRef> &v) {
            for (const sf::PartRef &pr : v)
                lines << QString::fromLatin1(tag) + QLatin1Char('|') + pr.part + QLatin1Char('{')
                             + jsonCompact(pr.params) + QLatin1Char('}');
        };
        listDeep("bg", s.background);
        listDeep("tx", s.text);
        listDeep("dc", s.decor);
        listDeep("ov", s.overlay);
    }
    for (const sf::Asset &as : p.assets)
        lines << QStringList{QStringLiteral("asset"), as.id, as.kind, as.path, as.sha1}
                     .join(QLatin1Char('|'));
    return lines.join(QLatin1Char('\n'));
}

void printDiff(const QString &x, const QString &y)
{
    const QStringList a = x.split(QLatin1Char('\n'));
    const QStringList b = y.split(QLatin1Char('\n'));
    const int n = std::max(a.size(), b.size());
    int shown = 0;
    for (int i = 0; i < n && shown < 8; ++i) {
        const QString la = i < a.size() ? a.at(i) : QStringLiteral("<缺行>");
        const QString lb = i < b.size() ? b.at(i) : QStringLiteral("<缺行>");
        if (la == lb)
            continue;
        std::printf("    第 %d 行不同:\n      A: %s\n      B: %s\n", i + 1, la.toUtf8().constData(),
                    lb.toUtf8().constData());
        ++shown;
    }
}

/** 9. 部件命令 + 批量撤销：插入/删除/层序/换轨都要能精确回滚。 */
void testPartCommands()
{
    section("9) part commands (insert/remove/move/track) + batch undo");

    // 每个子用例一份新工程，避免上一段的撤销历史干扰判断
    const auto seedParts = [](sf::Project &p) {
        sf::PartRef bg;
        bg.part = QStringLiteral("image");
        bg.valid = true;
        bg.params.insert(QStringLiteral("path"), QStringLiteral("assets/bg.png"));
        p.shots[0].background.push_back(bg);

        sf::PartRef pt;
        pt.part = QStringLiteral("portrait");
        pt.valid = true;
        pt.params.insert(QStringLiteral("x"), 0.5);
        p.shots[0].background.push_back(pt);

        sf::PartRef dc;
        dc.part = QStringLiteral("bokeh");
        dc.valid = true;
        p.shots[0].decor.push_back(dc);
    };

    const auto partName = [](const sf::PartRef &p) { return p.part; };

    // ── ① 插入 / 删除：params 与数组位置都要原样回来
    {
        sf::Project p = makeProject();
        seedParts(p);
        const QString before = deepSignature(p);
        sf::ProjectEditor ed(&p);

        QJsonObject pl;
        pl.insert(QStringLiteral("part"), QStringLiteral("flash"));
        pl.insert(QStringLiteral("params"),
                  QJsonObject{{QStringLiteral("kind"), QStringLiteral("flash")}});
        const sf::EditResult r =
            ed.apply(sf::EditCmd{sf::EditKind::PartInsert, QStringLiteral("S001"), 0, 0, 0,
                                 QStringLiteral("decor"), pl});
        CHECK(r.ok);
        CHECK(p.shots[0].decor.size() == 2);
        CHECK(partName(p.shots[0].decor[0]) == QStringLiteral("flash"));
        CHECK(p.shots[0].decor[0].params.value(QStringLiteral("kind")).toString()
              == QStringLiteral("flash"));
        CHECK(r.inverse.kind == sf::EditKind::PartRemove);
        CHECK(r.inverse.layer == QStringLiteral("decor"));
        CHECK(ed.undo());
        CHECK(deepSignature(p) == before);

        // 删中间那个带 params 的部件，undo 要把 params 一起带回来
        const sf::EditResult rm =
            ed.apply(sf::EditCmd{sf::EditKind::PartRemove, QStringLiteral("S001"), 1, 0, 0,
                                 QStringLiteral("background"), QJsonObject()});
        CHECK(rm.ok);
        CHECK(p.shots[0].background.size() == 1);
        CHECK(rm.inverse.kind == sf::EditKind::PartInsert);
        CHECK(ed.undo());
        CHECK(deepSignature(p) == before);
        CHECK(jsonCompact(p.shots[0].background.at(1).params).contains(QStringLiteral("0.5")));

        // 越界 / 层名写错：必须失败且不动工程
        CHECK(!ed.apply(sf::EditCmd{sf::EditKind::PartRemove, QStringLiteral("S001"), 9, 0, 0,
                                    QStringLiteral("decor"), QJsonObject()})
                   .ok);
        CHECK(!ed.apply(sf::EditCmd{sf::EditKind::PartRemove, QStringLiteral("S001"), 0, 0, 0,
                                    QStringLiteral("Background"), QJsonObject()})
                   .ok);
        CHECK(!ed.apply(sf::EditCmd{sf::EditKind::PartInsert, QStringLiteral("S001"), 0, 0, 0,
                                    QStringLiteral("decor"), QJsonObject()})
                   .ok);
        CHECK(deepSignature(p) == before);
        CHECK(!ed.canUndo());
    }

    // ── ② 层序：from → to 之后倒着走回来必须一模一样
    {
        sf::Project p = makeProject();
        seedParts(p);
        const QString before = deepSignature(p);
        sf::ProjectEditor ed(&p);

        CHECK(ed.apply(sf::EditCmd{sf::EditKind::PartMove, QStringLiteral("S001"), 0, 1, 0,
                                   QStringLiteral("background"), QJsonObject()})
                  .ok);
        CHECK(partName(p.shots[0].background.at(0)) == QStringLiteral("portrait"));
        CHECK(partName(p.shots[0].background.at(1)) == QStringLiteral("image"));
        CHECK(deepSignature(p) != before);
        CHECK(ed.undo());
        CHECK(deepSignature(p) == before);

        // 原地不动算失败（不压空撤销项）
        CHECK(!ed.apply(sf::EditCmd{sf::EditKind::PartMove, QStringLiteral("S001"), 0, 0, 0,
                                    QStringLiteral("background"), QJsonObject()})
                   .ok);
        CHECK(ed.undoDepth() == 0);
    }

    // ── ③ 批量：整批成功只压一条撤销；有一条失败则工程原样不动
    {
        sf::Project p = makeProject();
        seedParts(p);
        const QString before = deepSignature(p);
        sf::ProjectEditor ed(&p);

        const auto mkIns = [](int at, const QString &name) {
            QJsonObject pl;
            pl.insert(QStringLiteral("part"), name);
            return sf::EditCmd{sf::EditKind::PartInsert, QStringLiteral("S001"), double(at), 0, 0,
                               QStringLiteral("overlay"), pl};
        };
        const auto mkRm = [](int at) {
            return sf::EditCmd{sf::EditKind::PartRemove, QStringLiteral("S001"), double(at), 0, 0,
                               QStringLiteral("background"), QJsonObject()};
        };

        // 先验失败回滚：第二条是坏命令
        QVector<sf::EditCmd> bad;
        bad << mkIns(0, QStringLiteral("glitch")) << mkIns(1, QString());
        CHECK(!ed.applyBatch(bad, QStringLiteral("坏批次")));
        CHECK(deepSignature(p) == before);
        CHECK(ed.undoDepth() == 0);

        // 正常批次：背景两个部件整体搬到覆盖层
        QVector<sf::EditCmd> batch;
        batch << mkRm(1) << mkRm(0) << mkIns(0, QStringLiteral("image"))
              << mkIns(1, QStringLiteral("portrait"));
        CHECK(ed.applyBatch(batch, QStringLiteral("移动部件到覆盖层")));
        CHECK(ed.undoDepth() == 1);
        CHECK(ed.undoLabel() == QStringLiteral("移动部件到覆盖层"));
        CHECK(p.shots[0].background.isEmpty());
        CHECK(p.shots[0].overlay.size() == 2);
        CHECK(partName(p.shots[0].overlay.at(0)) == QStringLiteral("image"));
        CHECK(partName(p.shots[0].overlay.at(1)) == QStringLiteral("portrait"));

        CHECK(ed.undo()); // 一条撤销退回整批
        CHECK(deepSignature(p) == before);
        CHECK(ed.redo());
        CHECK(p.shots[0].background.isEmpty());
        CHECK(ed.undo());
        CHECK(deepSignature(p) == before);
    }

    // ── ④ 换轨：同 track 不重叠的约束在目标轨同样生效
    {
        sf::Project p = makeProject();
        const QString before = deepSignature(p);
        sf::ProjectEditor ed(&p);

        CHECK(ed.apply(sf::EditCmd{sf::EditKind::ShotTrack, QStringLiteral("S001"), 1, 0, 0,
                                   QString(), QJsonObject()})
                  .ok);
        const sf::Shot *s = findShot(p, QStringLiteral("S001"));
        CHECK(s != nullptr && s->track == 1);
        CHECK(s != nullptr && std::fabs(s->start - 0.0) < 1e-6);

        CHECK(ed.undo());
        s = findShot(p, QStringLiteral("S001"));
        CHECK(s != nullptr && s->track == 0);
        CHECK(deepSignature(p) == before);

        // 指定起点：track 1 上放到 20s
        QJsonObject pl;
        pl.insert(QStringLiteral("start"), 20.0);
        CHECK(ed.apply(sf::EditCmd{sf::EditKind::ShotTrack, QStringLiteral("S001"), 1, 0, 0,
                                   QString(), pl})
                  .ok);
        s = findShot(p, QStringLiteral("S001"));
        CHECK(s != nullptr && std::fabs(s->start - 20.0) < 1e-6);
        CHECK(s != nullptr && std::fabs(s->end - 25.0) < 1e-6);
        CHECK(ed.undo());
        CHECK(deepSignature(p) == before);
    }
}

/** 找真实样本工程：从 qt-shell 目录（以及 exe 目录）向上找 projects/。 */
QString findSampleProject()
{
    const QString name = QStringLiteral("私は雨.sf");
    const QString appDir = QDir::currentPath();
    const QStringList roots = {
        appDir,
        appDir + QStringLiteral("/.."),
        appDir + QStringLiteral("/../.."),
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + QStringLiteral("/.."),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../.."),
    };
    for (const QString &r : roots) {
        const QString cand = QDir(r).absoluteFilePath(QStringLiteral("projects/") + name);
        if (QFileInfo::exists(cand))
            return QFileInfo(cand).absoluteFilePath();
    }
    return QString();
}

void testRoundTrip()
{
    section("8) save/load round trip (real project file)");
    const QString src = findSampleProject();
    if (src.isEmpty()) {
        std::printf("  SKIP  找不到 projects/私は雨.sf\n");
        return;
    }
    std::printf("  样本: %s\n", src.toUtf8().constData());

    sf::Project a;
    QString err;
    if (!a.load(src, &err)) {
        std::printf("  FAIL  载入失败: %s\n", err.toUtf8().constData());
        ++g_fail;
        return;
    }
    std::printf("  载入: 镜头 %d / 歌词 %d / 样式 %d / 时长 %.3fs\n", int(a.shots.size()),
                int(a.lyrics.size()), int(a.styles.size()), a.duration());
    CHECK(!a.shots.isEmpty());
    CHECK(!a.lyrics.isEmpty());
    CHECK(!a.styles.isEmpty());

    // ── JSON 文本形态：2 空格缩进、顶层顺序、中文不转义
    const QString text = a.toJson();
    CHECK(text.startsWith(QStringLiteral("{\n  \"format\": \"ShizukuFlow\",\n  \"version\": 1,\n"
                                         "  \"meta\": {")));
    CHECK(text.contains(QStringLiteral("\"title\": \"私は雨\"")));
    CHECK(!text.contains(QStringLiteral("\\u79c1"))); // 私 不该被转成 \uXXXX
    CHECK(text.contains(QStringLiteral("\n  \"canvas\": {\n    \"background\": ")));
    CHECK(text.contains(QStringLiteral("\"width\": 1280")));   // 整数不写成 1.28e+03
    CHECK(text.contains(QStringLiteral("\"fps\": 30")));
    CHECK(!text.contains(QStringLiteral("e+03")));
    CHECK(text.contains(QStringLiteral("\n  \"shots\": [\n    {\n      \"end\": ")));
    CHECK(text.endsWith(QStringLiteral("  }\n}\n")));

    // ── ① 写盘 → 再 load
    const QString tmp1 = QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("sf_rt_a.sf"));
    const QString tmp2 = QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("sf_rt_b.sf"));
    if (!a.save(tmp1, &err)) {
        std::printf("  FAIL  保存失败: %s\n", err.toUtf8().constData());
        ++g_fail;
        return;
    }
    CHECK(QFileInfo::exists(tmp1));
    CHECK(QFileInfo(tmp1).size() > 1000);

    sf::Project b;
    if (!b.load(tmp1, &err)) {
        std::printf("  FAIL  回读失败: %s\n", err.toUtf8().constData());
        ++g_fail;
        QFile::remove(tmp1);
        return;
    }

    // ── ② 整体指纹
    const QString sigA = deepSignature(a);
    const QString sigB = deepSignature(b);
    std::printf("  [指纹] 整体逐字段  %s\n", sigA == sigB ? "一致" : "不一致");
    if (sigA != sigB)
        printDiff(sigA, sigB);
    CHECK(sigA == sigB);

    // ── ③ 分项覆盖断言
    CHECK(a.shots.size() == b.shots.size());
    int shotOk = 0;
    const int shotN = std::min(int(a.shots.size()), int(b.shots.size()));
    for (int i = 0; i < shotN; ++i) {
        const sf::Shot &x = a.shots.at(i);
        const sf::Shot &y = b.shots.at(i);
        const bool eq = x.id == y.id && std::fabs(x.start - y.start) < 1e-9
                        && std::fabs(x.end - y.end) < 1e-9 && x.track == y.track
                        && x.seed == y.seed && x.lyricRefs == y.lyricRefs
                        && partsEqual(x.background, y.background)
                        && partsEqual(x.text, y.text) && partsEqual(x.decor, y.decor)
                        && partsEqual(x.overlay, y.overlay)
                        && x.hasEntrance == y.hasEntrance
                        && partDeep(x.entrance, x.hasEntrance) == partDeep(y.entrance, y.hasEntrance)
                        && x.hasHold == y.hasHold
                        && partDeep(x.hold, x.hasHold) == partDeep(y.hold, y.hasHold)
                        && x.hasCamera == y.hasCamera
                        && partDeep(x.camera, x.hasCamera) == partDeep(y.camera, y.hasCamera)
                        && x.hasTransition == y.hasTransition
                        && partDeep(x.transition, x.hasTransition)
                               == partDeep(y.transition, y.hasTransition);
        if (eq) {
            ++shotOk;
        } else {
            std::printf("    shot 不一致: %s\n", x.id.toUtf8().constData());
        }
    }
    std::printf("  [镜头] %d/%d 逐字段一致（id/start/end/track/lyric_refs/4 图层/"
                "motion×4/transition/seed）\n",
                shotOk, int(a.shots.size()));
    CHECK(shotOk == int(a.shots.size()));

    CHECK(a.lyrics.size() == b.lyrics.size());
    int lyricOk = 0;
    int sylTotal = 0;
    int sylOk = 0;
    const int lyricN = std::min(int(a.lyrics.size()), int(b.lyrics.size()));
    for (int i = 0; i < lyricN; ++i) {
        const sf::Lyric &x = a.lyrics.at(i);
        const sf::Lyric &y = b.lyrics.at(i);
        bool eq = x.id == y.id && std::fabs(x.start - y.start) < 1e-9
                  && std::fabs(x.end - y.end) < 1e-9 && x.text == y.text && x.style == y.style
                  && x.syllables.size() == y.syllables.size();
        const int sn = std::min(int(x.syllables.size()), int(y.syllables.size()));
        for (int k = 0; k < sn; ++k) {
            const sf::Syllable &p = x.syllables.at(k);
            const sf::Syllable &q = y.syllables.at(k);
            const bool ok = p.text == q.text && std::fabs(p.start - q.start) < 1e-9
                            && std::fabs(p.end - q.end) < 1e-9;
            ++sylTotal;
            if (ok)
                ++sylOk;
            else
                eq = false;
        }
        if (eq) {
            ++lyricOk;
        } else {
            std::printf("    lyric 不一致: %s\n", x.id.toUtf8().constData());
        }
    }
    std::printf("  [歌词] %d/%d 逐字段一致，音节 %d/%d 一致\n", lyricOk, int(a.lyrics.size()),
                sylOk, sylTotal);
    CHECK(lyricOk == int(a.lyrics.size()));
    CHECK(sylOk == sylTotal);

    CHECK(a.styles.size() == b.styles.size());
    int styleOk = 0;
    const int styleN = std::min(int(a.styles.size()), int(b.styles.size()));
    for (int i = 0; i < styleN; ++i) {
        const sf::StyleDef &x = a.styles.at(i);
        const sf::StyleDef &y = b.styles.at(i);
        const bool eq = x.id == y.id && x.font == y.font && x.weight == y.weight
                        && std::fabs(x.size - y.size) < 1e-9
                        && std::fabs(x.letterSpacing - y.letterSpacing) < 1e-9
                        && std::fabs(x.lineHeight - y.lineHeight) < 1e-9 && x.fill == y.fill
                        && std::fabs(x.y - y.y) < 1e-9 && x.hasStroke == y.hasStroke
                        && x.strokeColor == y.strokeColor
                        && std::fabs(x.strokeWidth - y.strokeWidth) < 1e-9
                        && x.hasShadow == y.hasShadow && x.shadowColor == y.shadowColor
                        && std::fabs(x.shadowBlur - y.shadowBlur) < 1e-9
                        && std::fabs(x.shadowOffX - y.shadowOffX) < 1e-9
                        && std::fabs(x.shadowOffY - y.shadowOffY) < 1e-9;
        if (eq) {
            ++styleOk;
        } else {
            std::printf("    style 不一致: %s\n", x.id.toUtf8().constData());
        }
    }
    std::printf("  [样式] %d/%d 逐字段一致（font/weight/size/spacing/lineHeight/fill/stroke/"
                "shadow/y）\n",
                styleOk, int(a.styles.size()));
    CHECK(styleOk == int(a.styles.size()));

    const bool canvasOk = a.canvas.width == b.canvas.width && a.canvas.height == b.canvas.height
                          && a.canvas.fps == b.canvas.fps
                          && a.canvas.background == b.canvas.background
                          && a.canvas.safeAreaEnabled == b.canvas.safeAreaEnabled
                          && std::fabs(a.canvas.safeAreaMargin - b.canvas.safeAreaMargin) < 1e-9;
    const bool mediaOk = a.audioPath == b.audioPath
                         && std::fabs(a.audioDuration - b.audioDuration) < 1e-9;
    const bool timingOk =
        std::fabs(a.bpm - b.bpm) < 1e-9 && std::fabs(a.offset - b.offset) < 1e-9;
    const bool metaOk =
        a.title == b.title && a.artist == b.artist && a.accent == b.accent;
    std::printf("  [画布] 宽高/fps/背景/安全区  %s\n", canvasOk ? "一致" : "不一致");
    std::printf("  [媒体] audio.path/duration  %s（%s, %.3fs）\n", mediaOk ? "一致" : "不一致",
                b.audioPath.toUtf8().constData(), b.audioDuration);
    std::printf("  [节拍] bpm/offset  %s（%g / %g）\n", timingOk ? "一致" : "不一致", b.bpm,
                b.offset);
    std::printf("  [元信息] title/artist/accent  %s（%s）\n", metaOk ? "一致" : "不一致",
                b.title.toUtf8().constData());
    CHECK(canvasOk);
    CHECK(mediaOk);
    CHECK(timingOk);
    CHECK(metaOk);

    // ── ④ 二次往返（save → load → save）：确认不会越存越少
    CHECK(b.rawSource.value(QStringLiteral("export")).isObject());
    CHECK(b.rawSource.value(QStringLiteral("assets")).isArray());
    QString err2;
    CHECK(b.save(tmp2, &err2));
    sf::Project c;
    CHECK(c.load(tmp2, &err2));
    const bool stable = deepSignature(c) == sigA;
    std::printf("  [二次往返] load→save→load→save 后指纹  %s\n", stable ? "一致" : "不一致");
    CHECK(stable);
    // 未建模字段靠 rawSource 保住：locked / motion.exit / meta.created 都不能丢
    CHECK(b.toJson().contains(QStringLiteral("\"locked\"")));
    CHECK(b.toJson().contains(QStringLiteral("\"exit\":")));
    CHECK(b.toJson().contains(QStringLiteral("\"created\":")));

    QFile::remove(tmp1);
    QFile::remove(tmp2);

    // ── ⑤ 写不进去的路径要老实返回 false
    QString werr;
    const bool bad = a.save(QStringLiteral("Z:/__不存在__/x.sf"), &werr);
    std::printf("  [错误路径] save 返回 %s，error=「%s」\n", bad ? "true" : "false",
                werr.toUtf8().constData());
    CHECK(!bad);
    CHECK(!werr.isEmpty());

    // ── ⑥ assets：模型能解析、能按 id 取、相对路径能拼成绝对路径
    const QString adir = QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("sf_asset_rt"));
    QDir().mkpath(adir);
    const QString afile = adir + QStringLiteral("/asset.sf");

    sf::Project pa;
    pa.title = QStringLiteral("assets 测试");
    pa.canvas.width = 640;
    pa.canvas.height = 360;
    sf::Asset i1;
    i1.id = QStringLiteral("A001");
    i1.kind = QStringLiteral("image");
    i1.path = QStringLiteral("assets/bg_01.png");
    i1.sha1 = QStringLiteral("7d0f9a12c4");
    sf::Asset i2;
    i2.id = QStringLiteral("A002");
    i2.kind = QStringLiteral("font");
    i2.path = QStringLiteral("C:/fonts/x.ttf");
    sf::Asset i3;
    i3.id = QStringLiteral("A003");
    i3.kind = QStringLiteral("video"); // path 空 → assetPath 返回空串
    pa.assets.push_back(i1);
    pa.assets.push_back(i2);
    pa.assets.push_back(i3);

    QString aerr;
    CHECK(pa.save(afile, &aerr));

    // 落盘内容里真的有 assets（不是被模型丢了）
    QFile af(afile);
    CHECK(af.open(QIODevice::ReadOnly));
    const QByteArray araw = af.readAll();
    af.close();
    CHECK(araw.contains("\"assets\": ["));
    CHECK(araw.contains("assets/bg_01.png"));

    sf::Project pb;
    CHECK(pb.load(afile, &aerr));
    CHECK(pb.assets.size() == 3);
    const sf::Asset *ga = pb.asset(QStringLiteral("A001"));
    CHECK(ga != nullptr);
    CHECK(ga != nullptr && ga->kind == QStringLiteral("image"));
    CHECK(ga != nullptr && ga->sha1 == QStringLiteral("7d0f9a12c4"));
    CHECK(pb.asset(QStringLiteral("A002")) != nullptr);
    CHECK(pb.asset(QStringLiteral("A003")) != nullptr);

    pa.path = afile; // 模拟「工程已打开」：解析基准 = .sf 所在目录
    std::printf("  [资源] A001 解析为 %s\n", pb.assetPath(QStringLiteral("A001")).toUtf8().constData());
    CHECK(pb.assetPath(QStringLiteral("A001")) == pa.assetPath(QStringLiteral("A001")));
    CHECK(pb.assetPath(QStringLiteral("A001"))
          == QDir(adir).absoluteFilePath(QStringLiteral("assets/bg_01.png")));
    CHECK(pb.assetPath(QStringLiteral("A002")) == QStringLiteral("C:/fonts/x.ttf"));
    CHECK(pb.assetPath(QStringLiteral("A003")).isEmpty());
    CHECK(pb.asset(QStringLiteral("不存在")) == nullptr);
    CHECK(pb.assetPath(QStringLiteral("不存在")).isEmpty());

    QDir(adir).removeRecursively();
}

} // namespace

int main()
{
    std::printf("sf_edit_test —— 编辑命令层单元测试\n");

    testMoveLegal();
    testMoveClamped();
    testTrimInInvalid();
    testRemoveUndo();
    testAddUndo();
    testThreeCommands();
    testLyricOverlapAllowed();
    testPartCommands();
    testRoundTrip();

    std::printf("\n----------------------------------------\n");
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    std::printf("%s\n", g_fail == 0 ? "RESULT: PASS" : "RESULT: FAIL");
    return g_fail == 0 ? 0 : 1;
}
