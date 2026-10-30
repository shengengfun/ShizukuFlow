/**
 * ShizukuFlow 编辑层（原生侧）
 *
 * 为什么需要这一层：现在 Qt 壳只是个「只读渲染器」—— 时间轴只能 seek，
 * 镜头改不了时长、挪不了位置、删不掉。要变成非编，必须有「可撤销的编辑命令」。
 *
 * 设计约束（与 app/src/state/commands.ts 的 EditCmd 对齐）：
 *   - 命令是纯数据，不持有指针/闭包 → 将来能序列化跨进程序列化给 WebView 面板
 *   - 每条命令都能生成一条**反向命令**用于 undo，而不是存整份工程快照
 *     （工程可能上百镜头，快照式 undo 在拖动时会吃光内存）
 *   - 不碰 UI：本层只依赖 Project，可无头单元测试
 *
 * 时间约束：同一 track 上的镜头不允许重叠。所有涉及时间的命令内部做 clamp。
 */
#pragma once

#include "core/sf_project.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

namespace sf {

/** 编辑命令的种类。字段按 type 取用，未用到的保持默认值。 */
enum class EditKind
{
    None = 0,
    ShotMove,    // 整体平移：a = 新 start（end 同步平移，时长不变）
    ShotTrimIn,  // 改入点：a = 新 start
    ShotTrimOut, // 改出点：b = 新 end
    ShotAdd,     // 在 a 处插入空镜头，b = 时长
    ShotRemove,  // 删除 id 指定的镜头
    LyricMove,   // 歌词平移：a = 新 start
    LyricTrimIn, // 歌词入点：a = 新 start
    LyricTrimOut,// 歌词出点：b = 新 end
    ShotText,    // 改文字：path 留空，value 是新文本（作用于该镜头的歌词？见实现注释）

    // ── track / 部件（时间轴上"拖到另一条轨""层内换位""整组增删"用）──
    // 层一律用名字（background/decor/text/overlay），不用序号：
    // portrait/image 都在 background 里，用数字层号会把"层"和"下标"搅成一个槽位。
    ShotTrack,   // id = 镜头；a = 新 track（取整）；payload.start（可选）= 指定起点
    PartMove,    // id = 镜头；layer = 层名；a = from 下标；b = to 下标
    PartRemove,  // id = 镜头；layer；a = 下标
    PartInsert,  // id = 镜头；layer；a = 下标；payload = {part, params}
};

struct EditCmd
{
    EditKind kind = EditKind::None;
    QString id;    // 目标镜头 / 歌词 id；ShotAdd 时忽略
    double a = 0;  // 时间参数（语义随 kind 变化）；部件命令里是下标
    double b = 0;  // 时间参数（语义随 kind 变化）；部件命令里是目标下标
    double c = 0;  // 预留（当前未使用）
    QString layer;       // "background" / "decor" / "text" / "overlay"；空 = 不涉及层
    QJsonObject payload; // PartInsert = {part, params}；ShotTrack 撤销时带 start
};

/** 一次编辑的结果。ok 为 false 时工程未被改动。 */
struct EditResult
{
    bool ok = false;
    EditCmd inverse;     // 用于回滚；kind == None 表示无需回滚
    QString label;       // 撤销项显示名，如「移动镜头 S003」
    QString note;        // 被 clamp 时的说明（界面可提示）
};

/**
 * 工程编辑器：持有对 Project 的只读指针 + 撤销栈。
 *
 * 注意：不接管 Project 的所有权。调用方保证 Project 生命周期长于本对象。
 * 每次 apply 成功后，调用方需自行触发重绘（本层不发信号，保持无 Qt 依赖）。
 */
class ProjectEditor
{
public:
    explicit ProjectEditor(Project *project = nullptr);

    void setProject(Project *p);
    Project *project() const { return m_p; }

    /** 执行一条命令。成功时压入撤销栈并清空重做栈。 */
    EditResult apply(const EditCmd &cmd);

    /** 批量执行：整批成功才压**一条**撤销（框选多个部件整组操作）；
        任一条失败 → 已执行的按逆序回滚，工程回到调用前的状态。 */
    bool applyBatch(const QVector<EditCmd> &cmds, const QString &label);

    bool undo();
    bool redo();
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }

    QString undoLabel() const;
    QString redoLabel() const;
    int undoDepth() const { return int(m_undo.size()); }

    /** 清空历史（打开新工程后调用）。 */
    void clearHistory();

private:
    struct Entry
    {
        // 一条撤销项 = 一组命令（单条命令也走这里，正好给 applyBatch 复用）。
        // cmds 用于 redo，inverses 与 cmds 一一对应、倒序执行用于 undo。
        QVector<EditCmd> cmds;
        QVector<EditCmd> inverses;
        QString label;
    };

    EditResult doApply(const EditCmd &cmd);

    Project *m_p = nullptr;
    QVector<Entry> m_undo;
    QVector<Entry> m_redo;
};

/** 供 UI 复用的时间工具：给定同 track 的其他镜头，把 [s,e] 夹进空位。
    返回 false 表示无处可放（区间被占满）。 */
bool clampIntoFreeSlot(const Project &p, int track, const QString &ignoreShotId, double *s,
                       double *e);

} // namespace sf
