#include "core/sf_presets.h"

#include <QJsonArray>

namespace sf {
namespace {

PartPreset P(const QString &label, const QString &group, const QString &layer, const QString &part,
             const QJsonObject &params)
{
    return PartPreset{label, group, layer, part, params};
}

QJsonObject bg(const QString &kind, const QString &color, const QString &color2,
               const QString &accent, double seed, double angle = 90)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("color"), color);
    o.insert(QStringLiteral("color2"), color2);
    o.insert(QStringLiteral("accent"), accent);
    o.insert(QStringLiteral("seed"), seed);
    o.insert(QStringLiteral("angle"), angle);
    return o;
}

QJsonObject textParams(const QString &text, double size, double x, double y)
{
    QJsonObject o;
    o.insert(QStringLiteral("text"), text);
    o.insert(QStringLiteral("size"), size);
    o.insert(QStringLiteral("weight"), 900);
    o.insert(QStringLiteral("x"), x);
    o.insert(QStringLiteral("y"), y);
    o.insert(QStringLiteral("align"), QStringLiteral("center"));
    o.insert(QStringLiteral("fill"), QStringLiteral("#ffffff"));
    return o;
}

} // namespace

QVector<PartPreset> partPresets()
{
    QVector<PartPreset> v;

    // ── 背景：铺满整幅，插进去立刻能当底 ──────────────────────────────────
    v << P(QStringLiteral("渐变底"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("gradient"), QStringLiteral("#0b1020"),
                                            QStringLiteral("#241033"), QStringLiteral("#ffffff"), 1,
                                            100));
    v << P(QStringLiteral("极光"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("aurora"), QStringLiteral("#04060d"),
                                            QStringLiteral("#0d1a2e"), QStringLiteral("#7fd4ff"), 2,
                                            20));
    v << P(QStringLiteral("散景灯"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("bokeh"), QStringLiteral("#07070d"),
                                            QStringLiteral("#120a1a"), QStringLiteral("#ffd6ef"), 5));
    v << P(QStringLiteral("网格"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("grid"), QStringLiteral("#08080e"),
                                            QStringLiteral("#101018"), QStringLiteral("#6fe3ff"), 1));
    v << P(QStringLiteral("放射线"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("rays"), QStringLiteral("#0a0a12"),
                                            QStringLiteral("#140a14"), QStringLiteral("#ffffff"), 3));
    v << P(QStringLiteral("斜纹"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("stripes"), QStringLiteral("#0a0a10"),
                                            QStringLiteral("#16121e"), QStringLiteral("#ffffff"), 4,
                                            60));
    v << P(QStringLiteral("半调网"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("halftone"), QStringLiteral("#08080c"),
                                            QStringLiteral("#120e18"), QStringLiteral("#d8d8ff"), 6));
    v << P(QStringLiteral("扫描线"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("scanlines"), QStringLiteral("#06060a"),
                                            QStringLiteral("#0e0e16"), QStringLiteral("#9fe8ff"), 7));
    v << P(QStringLiteral("胶片颗粒底"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("noise"), QStringLiteral("#0a0a0c"),
                                            QStringLiteral("#14141a"), QStringLiteral("#ffffff"), 8));
    v << P(QStringLiteral("和纸"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), bg(QStringLiteral("paper"), QStringLiteral("#1b1712"),
                                            QStringLiteral("#2a2119"), QStringLiteral("#d8c39a"), 9));

    QJsonObject solid;
    solid.insert(QStringLiteral("kind"), QStringLiteral("solid"));
    solid.insert(QStringLiteral("color"), QStringLiteral("#08080e"));
    v << P(QStringLiteral("纯色底"), QStringLiteral("背景"), QStringLiteral("background"),
           QStringLiteral("background"), solid);

    // ── 装饰：在文字之下（decor 绘制在 background 之后、text 之前）──────────
    QJsonObject frame;
    frame.insert(QStringLiteral("margin"), 0.055);
    frame.insert(QStringLiteral("width"), 2.0);
    frame.insert(QStringLiteral("cross"), 14.0);
    v << P(QStringLiteral("细线框"), QStringLiteral("装饰"), QStringLiteral("decor"),
           QStringLiteral("frameLines"), frame);

    QJsonObject cross;
    cross.insert(QStringLiteral("margin"), 0.03);
    cross.insert(QStringLiteral("width"), 2.0);
    cross.insert(QStringLiteral("cross"), 30.0);
    v << P(QStringLiteral("十字准星"), QStringLiteral("装饰"), QStringLiteral("decor"),
           QStringLiteral("frameLines"), cross);

    QJsonObject rings;
    rings.insert(QStringLiteral("every"), 4);
    rings.insert(QStringLiteral("maxR"), 0.75);
    rings.insert(QStringLiteral("width"), 2.5);
    rings.insert(QStringLiteral("color"), QStringLiteral("#8fd8ff"));
    v << P(QStringLiteral("节拍圆环"), QStringLiteral("装饰"), QStringLiteral("decor"),
           QStringLiteral("beatRings"), rings);

    QJsonObject bars;
    bars.insert(QStringLiteral("n"), 48);
    bars.insert(QStringLiteral("maxH"), 0.16);
    bars.insert(QStringLiteral("color"), QStringLiteral("rgba(255,255,255,0.18)"));
    v << P(QStringLiteral("频谱条"), QStringLiteral("装饰"), QStringLiteral("decor"),
           QStringLiteral("bars"), bars);

    QJsonObject dots;
    dots.insert(QStringLiteral("n"), 40);
    dots.insert(QStringLiteral("size"), 2.5);
    dots.insert(QStringLiteral("speed"), 1.0);
    dots.insert(QStringLiteral("color"), QStringLiteral("#ffffff"));
    v << P(QStringLiteral("漂浮粒子"), QStringLiteral("装饰"), QStringLiteral("decor"),
           QStringLiteral("particles"), dots);

    QJsonObject flash;
    flash.insert(QStringLiteral("at"), 0);
    flash.insert(QStringLiteral("dur"), 0.2);
    flash.insert(QStringLiteral("maxAlpha"), 0.9);
    v << P(QStringLiteral("闪白"), QStringLiteral("装饰"), QStringLiteral("decor"),
           QStringLiteral("flash"), flash);

    // ── 覆盖：在文字之上 ─────────────────────────────────────────────────
    QJsonObject vig;
    vig.insert(QStringLiteral("kind"), QStringLiteral("vignette"));
    vig.insert(QStringLiteral("color"), QStringLiteral("rgba(0,0,0,0.55)"));
    v << P(QStringLiteral("暗角"), QStringLiteral("覆盖"), QStringLiteral("overlay"),
           QStringLiteral("overlay"), vig);

    QJsonObject grain;
    grain.insert(QStringLiteral("kind"), QStringLiteral("grain"));
    grain.insert(QStringLiteral("amount"), 420);
    grain.insert(QStringLiteral("alpha"), 0.05);
    v << P(QStringLiteral("胶片颗粒"), QStringLiteral("覆盖"), QStringLiteral("overlay"),
           QStringLiteral("overlay"), grain);

    // ── 文字：插进去就是可编辑的排版件 ───────────────────────────────────
    v << P(QStringLiteral("主标题"), QStringLiteral("文字"), QStringLiteral("text"),
           QStringLiteral("text"),
           textParams(QStringLiteral("主标题"), 160, 0.5, 0.42));

    QJsonObject vert = textParams(QStringLiteral("縦書き"), 80, 0.85, 0.5);
    vert.insert(QStringLiteral("vertical"), true);
    v << P(QStringLiteral("竖排和文"), QStringLiteral("文字"), QStringLiteral("text"),
           QStringLiteral("text"), vert);

    QJsonObject tilt = textParams(QStringLiteral("副标题"), 56, 0.3, 0.72);
    tilt.insert(QStringLiteral("weight"), 700);
    tilt.insert(QStringLiteral("rotate"), -8);
    v << P(QStringLiteral("斜切副标"), QStringLiteral("文字"), QStringLiteral("text"),
           QStringLiteral("text"), tilt);

    QJsonObject outline = textParams(QStringLiteral("描边字"), 128, 0.5, 0.5);
    QJsonObject stroke;
    stroke.insert(QStringLiteral("color"), QStringLiteral("#000000"));
    stroke.insert(QStringLiteral("width"), 8);
    outline.insert(QStringLiteral("stroke"), stroke);
    v << P(QStringLiteral("描边大字"), QStringLiteral("文字"), QStringLiteral("text"),
           QStringLiteral("text"), outline);

    QJsonObject echo = textParams(QStringLiteral("回声字"), 120, 0.5, 0.5);
    QJsonObject ec;
    ec.insert(QStringLiteral("n"), 3);
    ec.insert(QStringLiteral("dx"), 10);
    ec.insert(QStringLiteral("decay"), 0.35);
    echo.insert(QStringLiteral("echo"), ec);
    v << P(QStringLiteral("重影字"), QStringLiteral("文字"), QStringLiteral("text"),
           QStringLiteral("text"), echo);

    return v;
}

PartPreset presetByLabel(const QString &label)
{
    for (const PartPreset &p : partPresets())
        if (p.label == label)
            return p;
    return PartPreset{};
}

} // namespace sf
