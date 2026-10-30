#pragma once
//
// 部件库预设表。
//
// 左侧「演出部件库」原来是一张 71 条的排版占位表：名字好看，但拖进工程什么也不会发生 ——
// 因为那 71 个名字并不是引擎真的认识的部件。这里把「引擎真的能画出来的东西」整理成
// 一份可插入的配方表，供界面直接消费：每条 = 一个部件名 + 一套能看效果的默认参数。
//
// 约定：
//   layer 决定插进 shot.layers 的哪一层，而不只是数组位置 ——
//   background 在文字之下、overlay 在文字之上，插错层就等于把字盖住了。
//
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace sf {

struct PartPreset
{
    QString label;      // 界面显示名
    QString group;      // 分组（背景 / 装饰 / 覆盖 / 文字）
    QString layer;      // shot.layers 的键：background / text / decor / overlay
    QString part;       // 引擎部件名
    QJsonObject params; // 可直接用的默认参数
};

/** 所有可插入的预设（顺序即界面顺序）。 */
QVector<PartPreset> partPresets();

/** 按 label 精确查找；找不到返回空（`label` 为空表示未命中）。 */
PartPreset presetByLabel(const QString &label);

} // namespace sf
