/**
 * ShizukuFlow 工程模型（.sf 文件）
 *
 * .sf 是纯 JSON：format/version/meta/canvas/media/timing/styles/lyrics/shots/assets/export。
 * 本文件只做「解析 + 索引」，不含任何渲染逻辑 —— 渲染在 sf_render。
 */
#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sf {

struct PartRef
{
    QString part;
    QJsonObject params;
    bool valid = false;
};

struct Shot
{
    QString id;
    double start = 0;
    double end = 0;
    int track = 0;
    QStringList lyricRefs;

    QVector<PartRef> background;
    QVector<PartRef> text;
    QVector<PartRef> decor;
    QVector<PartRef> overlay;

    bool hasEntrance = false;
    PartRef entrance;
    bool hasHold = false;
    PartRef hold;
    bool hasCamera = false;
    PartRef camera;
    bool hasTransition = false;
    PartRef transition;

    int seed = 0;
};

struct Syllable
{
    QString text;
    double start = 0;
    double end = 0;
};

struct Lyric
{
    QString id;
    double start = 0;
    double end = 0;
    QString text;
    QVector<Syllable> syllables;
    QString style;
};

struct StyleDef
{
    QString id;
    QString font = QStringLiteral("Noto Serif SC");
    int weight = 900;
    double size = 96;
    double letterSpacing = 0;
    double lineHeight = 1.1;
    QColor fill = Qt::white;
    double y = 0.58;

    bool hasStroke = false;
    QColor strokeColor = Qt::black;
    double strokeWidth = 4;

    bool hasShadow = false;
    QColor shadowColor = QColor(0, 0, 0, 128);
    double shadowBlur = 10;
    double shadowOffX = 0;
    double shadowOffY = 4;
};

struct CanvasDef
{
    int width = 1920;
    int height = 1080;
    int fps = 30;
    QColor background = Qt::black;
    bool safeAreaEnabled = false;
    double safeAreaMargin = 0.05;
};

/** 工程引用的外部资源（图片/视频/字体）。path 存相对 .sf 的路径，读取时再拼绝对路径。 */
struct Asset
{
    QString id;   // "A001"
    QString kind; // "image" / "video" / "font"
    QString path; // 相对 .sf 的路径，如 "assets/bg_01.png"
    QString sha1;
};

struct Project
{
    QString path;
    QString title = QStringLiteral("未命名工程");
    QString artist;
    QColor accent = QColor("#7fd4ff");

    CanvasDef canvas;
    QString audioPath;
    double audioDuration = 0;

    double bpm = 120;
    double offset = 0;

    QVector<StyleDef> styles;
    QVector<Lyric> lyrics;
    QVector<Shot> shots;
    QVector<Asset> assets;

    bool load(const QString &path, QString *error = nullptr);

    /** 序列化回 .sf（与 load 严格对称）。失败时写 error。 */
    bool save(const QString &path, QString *error = nullptr) const;

    /** 序列化成 JSON 字符串（测试与调试用）。 */
    QString toJson() const;

    /** 末尾镜头 end（音频时长更长则取音频）。 */
    double duration() const;
    int frameCount() const;
    QVector<double> shotStarts() const;
    const StyleDef *style(const QString &id) const;
    /** 当前时刻所在镜头的下标（按 start 升序），无则 -1。 */
    int shotIndexAt(double t) const;
    const Lyric *lyric(const QString &id) const;

    /** 按 id 找资源；找不到返回 nullptr。 */
    const Asset *asset(const QString &id) const;
    /** 把 asset 的 path 解析成绝对路径（相对 .sf 所在目录）。空则返回空串。 */
    QString assetPath(const QString &id) const;

    /** load 时原样留下的根 JSON。save 用它回填模型没建模的字段
        （meta.created / generator、shot.locked、motion.exit、timing.beats、
        media.reference、export 预设…），避免「读进来再写回去就少东西」。
        空对象表示没有可回填的原始数据（新建工程即如此）。 */
    QJsonObject rawSource;
};

} // namespace sf
