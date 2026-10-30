/**
 * 工程体检
 *
 * 渲染内核遇到坏数据一律「静默跳过」：图片找不到就画个空，时间区间重叠就后者盖前者。
 * 对成片来说这是对的（导出不能中断），但对创作来说是灾难 —— 一个拼错的路径会变成
 * 一整段黑屏，而没人会去逐帧找。
 *
 * 所以把「可疑之处」集中报出来，给编辑器和 CLI 共用：导出前跑一遍，比事后抽帧靠谱。
 */
#pragma once

#include "sf_project.h"

#include <QString>
#include <QVector>

namespace sf {

struct LintIssue
{
    enum Level { Error, Warning };
    Level level = Warning;
    QString where;    // "shot#12 (S013 @12.50s)" / "lyrics[3]"
    QString message;
};

/** `projectDir` 用于解析相对的图片/视频路径（通常是 .sf 所在目录）。 */
QVector<LintIssue> lintProject(const Project &project, const QString &projectDir);

/** 一行摘要，CLI 与状态栏都用它。 */
QString lintSummary(const QVector<LintIssue> &issues);

} // namespace sf
