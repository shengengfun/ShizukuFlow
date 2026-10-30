/**
 * LRC 歌词双向转换
 *
 * 生产流程里歌词几乎都先落在 .lrc（网易云/音乐库导出、Aegisub 转出、手打）。
 * 工程内部的 `lyrics[]` 是它的超集（多一段逐字 `syllables`），所以两头都要能走：
 * 导入时尽量把 `<mm:ss.xx>` 的增强格式吃成逐字，导回时再吐出去，方便交给别的软件。
 *
 * 只处理「时间 + 文本」，不碰样式：样式是工程自己的事，.lrc 没有这个概念。
 */
#pragma once

#include "sf_project.h"

#include <QString>

namespace sf {

struct LrcLoadResult
{
    QVector<Lyric> lyrics;
    QString title;
    QString artist;
    double offsetMs = 0;   // [offset:] 标签，正数表示歌词提前
    QString error;
    bool ok() const { return error.isEmpty(); }
};

/**
 * 解析 .lrc 文本。编码自动识别（UTF-8 带/不带 BOM，回退 GB18030）——
 * 中文歌词文件在 GBK 里存了几十年，硬要 UTF-8 会读出一堆问号。
 */
LrcLoadResult parseLrc(const QByteArray &raw);

/** 读文件并写进 `project.lyrics`（替换原有歌词，id 重新编号为 L001…）。 */
bool importLrc(const QString &path, Project &project, QString *error = nullptr);

/** 导出成 .lrc 文本（UTF-8 无 BOM，CRLF —— 大部分播放器按行读，CRLF 最保险）。 */
QString toLrc(const Project &project);

bool exportLrc(const Project &project, const QString &path, QString *error = nullptr);

} // namespace sf
