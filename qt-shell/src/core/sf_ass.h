/**
 * ASS 字幕导出
 *
 * 为什么需要它：工程里已经有一份逐字时间戳（`lyrics[].syllables`），但压制流程
 * （Aegisub / ass 滤镜 / 岚珠工具箱）认的是 ASS。让字幕从渲染引擎里「再打一遍」
 * 是没有意义的重复劳动，直接从同一份数据生成字幕，两边的歌词就永远对得上。
 *
 * 只做导出，不做导入：ASS 是别人家的格式，回灌会带来样式语义的泥潭。
 */
#pragma once

#include "sf_project.h"

#include <QString>

namespace sf {

struct AssExportOptions
{
    /** 写 `{\k}` 卡拉OK 标签（有逐字时间戳时才有内容可写）。 */
    bool karaoke = true;
    /** 行进出加淡入淡出 `\fad(120,120)`。 */
    bool fade = true;
    /** 画面上用的停靠点，与渲染里的 style.y 对齐（0=顶 1=底）。 */
    bool alignToStyleY = true;
};

/** 生成整份 .ass 文本（不含 BOM，UTF-8）。 */
QString toAss(const Project &project, const AssExportOptions &opt = AssExportOptions());

/** 落盘。写 UTF-8 BOM —— Aegisub 与部分压制脚本对无 BOM 的中文靠猜。 */
bool exportAss(const Project &project, const QString &path, QString *error = nullptr,
               const AssExportOptions &opt = AssExportOptions());

} // namespace sf
