#include "sf_project.h"

#include "lume.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sf {

namespace {

QString jstr(const QJsonObject &o, const char *k, const QString &dflt = QString())
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isString() ? v.toString() : dflt;
}

double jnum(const QJsonObject &o, const char *k, double dflt = 0)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toDouble() : dflt;
}

bool jbool(const QJsonObject &o, const char *k, bool dflt = false)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isBool() ? v.toBool() : dflt;
}

PartRef toPartRef(const QJsonObject &o)
{
    PartRef p;
    p.part = jstr(o, "part");
    p.params = o.value(QLatin1String("params")).toObject();
    p.valid = !p.part.isEmpty();
    return p;
}

QVector<PartRef> toPartList(const QJsonValue &v)
{
    QVector<PartRef> out;
    for (const QJsonValue &e : v.toArray())
        out.push_back(toPartRef(e.toObject()));
    return out;
}

bool readOptionalPart(const QJsonObject &motion, const char *key, PartRef &out)
{
    const QJsonValue v = motion.value(QLatin1String(key));
    if (v.isNull() || !v.isObject())
        return false;
    out = toPartRef(v.toObject());
    return out.valid;
}

// ── 序列化（Project::toJson / save） ─────────────────────────────────────────
//
// 与上面的解析函数一一对应：toPartRef ↔ partToJson、readOptionalPart ↔ partToJson
// 的 null 分支。改解析时把这里一起改，否则字段会静默丢掉。

/** QColor → CSS 字符串。8 位时必须是 RRGGBBAA（Qt 的 HexArgb 是 AARRGGBB，不能用）。 */
QString colorText(const QColor &c)
{
    const QColor v = c.isValid() ? c : QColor(Qt::black);
    if (v.alpha() == 255)
        return v.name(QColor::HexRgb).toLower();
    return QStringLiteral("#%1%2%3%4")
        .arg(v.red(), 2, 16, QLatin1Char('0'))
        .arg(v.green(), 2, 16, QLatin1Char('0'))
        .arg(v.blue(), 2, 16, QLatin1Char('0'))
        .arg(v.alpha(), 2, 16, QLatin1Char('0'));
}

/** 最短且能原样读回的十进制写法：5.618 不会变成 5.6180000000000001，1280 不会变成 1.28e+03。 */
QString numText(double v)
{
    if (!std::isfinite(v))
        return QStringLiteral("0");
    if (v == 0)
        return QStringLiteral("0");
    // 整数值一律写整数：JSON 里 "fps": 30 比 3e+01 可读得多，也避免工程被人误读成浮点
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        return QString::number(qint64(v));
    // 其余用定点补到能原样读回的最短位数（'g' 会在 0.0001 以下自动转科学计数法）
    for (int prec = 1; prec <= 17; ++prec) {
        const QString s = QString::number(v, 'f', prec);
        if (s.toDouble() == v)
            return s;
    }
    return QString::number(v, 'g', 17);
}

QString quoteStr(const QString &s)
{
    QString out;
    out.reserve(s.size() + 2);
    out += QLatin1Char('"');
    for (const QChar c : s) {
        switch (c.unicode()) {
        case u'"':
            out += QLatin1String("\\\"");
            break;
        case u'\\':
            out += QLatin1String("\\\\");
            break;
        case u'\b':
            out += QLatin1String("\\b");
            break;
        case u'\f':
            out += QLatin1String("\\f");
            break;
        case u'\n':
            out += QLatin1String("\\n");
            break;
        case u'\r':
            out += QLatin1String("\\r");
            break;
        case u'\t':
            out += QLatin1String("\\t");
            break;
        default:
            // 只转义控制字符；中文原样输出（UTF-8，不转 \uXXXX）
            if (c.unicode() < 0x20)
                out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    out += QLatin1Char('"');
    return out;
}

/** 2 空格缩进的 JSON 写出器（QJsonDocument::Indented 是 4 空格，且会重排数组）。 */
void writeJson(const QJsonValue &v, int depth, QString &out)
{
    const QString pad(depth * 2, QLatin1Char(' '));
    const QString padIn((depth + 1) * 2, QLatin1Char(' '));

    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.isEmpty()) {
            out += QLatin1String("{}");
            return;
        }
        const QStringList keys = o.keys();
        out += QLatin1String("{\n");
        for (int i = 0; i < keys.size(); ++i) {
            out += padIn + quoteStr(keys.at(i)) + QLatin1String(": ");
            writeJson(o.value(keys.at(i)), depth + 1, out);
            out += (i + 1 < keys.size()) ? QLatin1String(",\n") : QLatin1String("\n");
        }
        out += pad + QLatin1Char('}');
        return;
    }
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.isEmpty()) {
            out += QLatin1String("[]");
            return;
        }
        out += QLatin1String("[\n");
        for (int i = 0; i < a.size(); ++i) {
            out += padIn;
            writeJson(a.at(i), depth + 1, out);
            out += (i + 1 < a.size()) ? QLatin1String(",\n") : QLatin1String("\n");
        }
        out += pad + QLatin1Char(']');
        return;
    }

    switch (v.type()) {
    case QJsonValue::Bool:
        out += v.toBool() ? QLatin1String("true") : QLatin1String("false");
        break;
    case QJsonValue::Double:
        out += numText(v.toDouble());
        break;
    case QJsonValue::String:
        out += quoteStr(v.toString());
        break;
    default:
        out += QLatin1String("null");
        break;
    }
}

/** 把 src 里「模型没建模」的键补回 out。out 已有的键与 known 里的键一律不覆盖。 */
QJsonObject keepUnknown(const QJsonObject &out, const QJsonObject &src, const QStringList &known)
{
    if (src.isEmpty())
        return out;
    QJsonObject merged = out;
    for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
        if (known.contains(it.key()) || merged.contains(it.key()))
            continue;
        merged.insert(it.key(), it.value());
    }
    return merged;
}

/** PartRef → JSON。无效引用写成 null（反向解析即 readOptionalPart 的 false 分支）。 */
QJsonValue partToJson(const PartRef &p)
{
    if (!p.valid || p.part.isEmpty())
        return QJsonValue(QJsonValue::Null);
    QJsonObject o;
    o.insert(QStringLiteral("part"), p.part);
    if (!p.params.isEmpty())
        o.insert(QStringLiteral("params"), p.params);
    return o;
}

QJsonArray partsToJson(const QVector<PartRef> &list)
{
    QJsonArray a;
    for (const PartRef &p : list) {
        const QJsonValue v = partToJson(p);
        if (!v.isNull())
            a.append(v);
    }
    return a;
}

/** 样本工程里的 export 块；rawSource 里有时优先用原来的。 */
QJsonObject defaultExport()
{
    QJsonObject video;
    video.insert(QStringLiteral("codec"), QStringLiteral("h264"));
    video.insert(QStringLiteral("crf"), 18);
    video.insert(QStringLiteral("preset"), QStringLiteral("medium"));
    video.insert(QStringLiteral("encoder"), QStringLiteral("auto"));

    QJsonObject gif;
    gif.insert(QStringLiteral("fps"), 12);
    gif.insert(QStringLiteral("width"), 640);
    gif.insert(QStringLiteral("dither"), QStringLiteral("sierra2_4a"));

    QJsonObject apng;
    apng.insert(QStringLiteral("fps"), 24);
    apng.insert(QStringLiteral("colors"), 256);

    QJsonObject o;
    o.insert(QStringLiteral("preset"), QStringLiteral("web-720p"));
    o.insert(QStringLiteral("video"), video);
    o.insert(QStringLiteral("gif"), gif);
    o.insert(QStringLiteral("apng"), apng);
    return o;
}

/** id → 原始 JSON 对象的索引（用于把 locked / exit 之类的未建模字段认领回去）。 */
QHash<QString, QJsonObject> indexById(const QJsonArray &arr)
{
    QHash<QString, QJsonObject> map;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QLatin1String("id")).toString();
        if (!id.isEmpty() && !map.contains(id))
            map.insert(id, o);
    }
    return map;
}

} // namespace

bool Project::load(const QString &filePath, QString *error)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开文件：%1").arg(filePath);
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (doc.isNull() || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("JSON 解析失败（offset %1）：%2")
                         .arg(pe.offset)
                         .arg(pe.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (jstr(root, "format") != QStringLiteral("ShizukuFlow")) {
        if (error)
            *error = QStringLiteral("不是 ShizukuFlow 工程（format=%1）").arg(jstr(root, "format"));
        return false;
    }

    path = filePath;
    rawSource = root; // 原样留底，save 时用来回填模型没建模的字段
    styles.clear();
    lyrics.clear();
    shots.clear();
    assets.clear();

    // ── meta
    const QJsonObject meta = root.value(QLatin1String("meta")).toObject();
    title = jstr(meta, "title", QStringLiteral("未命名工程"));
    artist = jstr(meta, "artist");
    accent = lume::parseColor(jstr(meta, "accent", QStringLiteral("#7fd4ff")), QColor("#7fd4ff"));

    // ── canvas
    const QJsonObject cv = root.value(QLatin1String("canvas")).toObject();
    canvas.width = int(jnum(cv, "width", 1920));
    canvas.height = int(jnum(cv, "height", 1080));
    canvas.fps = std::max(1, int(jnum(cv, "fps", 30)));
    canvas.background = lume::parseColor(jstr(cv, "background", QStringLiteral("#000000")), Qt::black);
    const QJsonObject sa = cv.value(QLatin1String("safe_area")).toObject();
    canvas.safeAreaEnabled = jbool(sa, "enabled");
    canvas.safeAreaMargin = jnum(sa, "margin", 0.05);

    // ── media
    const QJsonObject media = root.value(QLatin1String("media")).toObject();
    const QJsonObject audio = media.value(QLatin1String("audio")).toObject();
    audioPath = jstr(audio, "path");
    audioDuration = jnum(audio, "duration", 0);

    // ── timing
    const QJsonObject tm = root.value(QLatin1String("timing")).toObject();
    bpm = jnum(tm, "bpm", 120);
    if (bpm <= 0)
        bpm = 120;
    offset = jnum(tm, "offset", 0);

    // ── styles
    for (const QJsonValue &v : root.value(QLatin1String("styles")).toArray()) {
        const QJsonObject o = v.toObject();
        StyleDef s;
        s.id = jstr(o, "id");
        s.font = jstr(o, "font", QStringLiteral("Noto Serif SC"));
        s.weight = int(jnum(o, "weight", 900));
        s.size = jnum(o, "size", 96);
        s.letterSpacing = jnum(o, "letter_spacing", 0);
        s.lineHeight = jnum(o, "line_height", 1.1);
        s.fill = lume::parseColor(jstr(o, "fill", QStringLiteral("#ffffff")), Qt::white);
        s.y = jnum(o, "y", 0.58);
        if (o.contains(QLatin1String("stroke"))) {
            const QJsonObject st = o.value(QLatin1String("stroke")).toObject();
            s.hasStroke = true;
            s.strokeColor = lume::parseColor(jstr(st, "color", QStringLiteral("#000000")), Qt::black);
            s.strokeWidth = jnum(st, "width", 4);
        }
        if (o.contains(QLatin1String("shadow"))) {
            const QJsonObject sh = o.value(QLatin1String("shadow")).toObject();
            s.hasShadow = true;
            s.shadowColor =
                lume::parseColor(jstr(sh, "color", QStringLiteral("rgba(0,0,0,0.5)")),
                                 QColor(0, 0, 0, 128));
            s.shadowBlur = jnum(sh, "blur", 10);
            const QJsonArray off = sh.value(QLatin1String("offset")).toArray();
            s.shadowOffX = off.size() > 0 ? off.at(0).toDouble() : 0;
            s.shadowOffY = off.size() > 1 ? off.at(1).toDouble() : 4;
        }
        styles.push_back(s);
    }

    // ── lyrics
    for (const QJsonValue &v : root.value(QLatin1String("lyrics")).toArray()) {
        const QJsonObject o = v.toObject();
        Lyric l;
        l.id = jstr(o, "id");
        l.start = jnum(o, "start", 0);
        l.end = jnum(o, "end", 0);
        l.text = jstr(o, "text");
        l.style = jstr(o, "style");
        for (const QJsonValue &sv : o.value(QLatin1String("syllables")).toArray()) {
            const QJsonObject so = sv.toObject();
            Syllable sy;
            sy.text = jstr(so, "text");
            sy.start = jnum(so, "start", l.start);
            sy.end = jnum(so, "end", l.end);
            l.syllables.push_back(sy);
        }
        lyrics.push_back(l);
    }

    // ── shots
    for (const QJsonValue &v : root.value(QLatin1String("shots")).toArray()) {
        const QJsonObject o = v.toObject();
        Shot s;
        s.id = jstr(o, "id");
        s.start = jnum(o, "start", 0);
        s.end = jnum(o, "end", 0);
        s.track = int(jnum(o, "track", 0));
        s.seed = int(jnum(o, "seed", 0));
        for (const QJsonValue &rv : o.value(QLatin1String("lyric_refs")).toArray())
            s.lyricRefs << rv.toString();

        const QJsonObject layers = o.value(QLatin1String("layers")).toObject();
        s.background = toPartList(layers.value(QLatin1String("background")));
        s.text = toPartList(layers.value(QLatin1String("text")));
        s.decor = toPartList(layers.value(QLatin1String("decor")));
        s.overlay = toPartList(layers.value(QLatin1String("overlay")));

        const QJsonObject motion = o.value(QLatin1String("motion")).toObject();
        s.hasEntrance = readOptionalPart(motion, "entrance", s.entrance);
        s.hasHold = readOptionalPart(motion, "hold", s.hold);
        s.hasCamera = readOptionalPart(motion, "camera", s.camera);
        s.hasTransition = readOptionalPart(o, "transition", s.transition);

        shots.push_back(s);
    }

    std::sort(shots.begin(), shots.end(), [](const Shot &a, const Shot &b) {
        return a.track != b.track ? a.track < b.track : a.start < b.start;
    });

    // ── assets（渲染层要按 id 取图片/字体，所以必须有模型，不能只做 JSON 透传）
    for (const QJsonValue &v : root.value(QLatin1String("assets")).toArray()) {
        const QJsonObject o = v.toObject();
        Asset a;
        a.id = jstr(o, "id");
        a.kind = jstr(o, "kind");
        a.path = jstr(o, "path");
        a.sha1 = jstr(o, "sha1");
        assets.push_back(a);
    }

    return true;
}

QString Project::toJson() const
{
    const QJsonObject src = rawSource;
    const auto known = [](std::initializer_list<const char *> keys) {
        QStringList out;
        for (const char *k : keys)
            out << QString::fromLatin1(k);
        return out;
    };
    const auto optionalPart = [](const PartRef &p, bool has) {
        return has ? partToJson(p) : QJsonValue(QJsonValue::Null);
    };

    // ── meta
    QJsonObject meta;
    meta.insert(QStringLiteral("title"), title);
    meta.insert(QStringLiteral("artist"), artist);
    meta.insert(QStringLiteral("modified"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    meta.insert(QStringLiteral("accent"), colorText(accent));
    meta = keepUnknown(meta, src.value(QLatin1String("meta")).toObject(),
                       known({"title", "artist", "modified", "accent"}));

    // ── canvas
    QJsonObject safeArea;
    safeArea.insert(QStringLiteral("enabled"), canvas.safeAreaEnabled);
    safeArea.insert(QStringLiteral("margin"), canvas.safeAreaMargin);

    QJsonObject cv;
    cv.insert(QStringLiteral("width"), canvas.width);
    cv.insert(QStringLiteral("height"), canvas.height);
    cv.insert(QStringLiteral("fps"), canvas.fps);
    cv.insert(QStringLiteral("background"), colorText(canvas.background));
    cv.insert(QStringLiteral("safe_area"), safeArea);
    cv = keepUnknown(cv, src.value(QLatin1String("canvas")).toObject(),
                     known({"width", "height", "fps", "background", "safe_area"}));

    // ── media（sha1 / kind 之类由 rawSource 回填）
    const QJsonObject srcMedia = src.value(QLatin1String("media")).toObject();
    QJsonObject audio;
    audio.insert(QStringLiteral("path"), audioPath);
    audio.insert(QStringLiteral("duration"), audioDuration);
    audio = keepUnknown(audio, srcMedia.value(QLatin1String("audio")).toObject(),
                        known({"path", "duration"}));

    QJsonObject media;
    media.insert(QStringLiteral("audio"), audio);
    media = keepUnknown(media, srcMedia, known({"audio"}));

    // ── timing（beats / downbeats / source 由 rawSource 回填）
    QJsonObject tm;
    tm.insert(QStringLiteral("bpm"), bpm);
    tm.insert(QStringLiteral("offset"), offset);
    tm = keepUnknown(tm, src.value(QLatin1String("timing")).toObject(), known({"bpm", "offset"}));

    // ── styles
    const QHash<QString, QJsonObject> rawStyles =
        indexById(src.value(QLatin1String("styles")).toArray());
    QJsonArray stylesArr;
    for (const StyleDef &s : styles) {
        const QJsonObject rawStyle = rawStyles.value(s.id);
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("font"), s.font);
        o.insert(QStringLiteral("weight"), s.weight);
        o.insert(QStringLiteral("size"), s.size);
        o.insert(QStringLiteral("letter_spacing"), s.letterSpacing);
        o.insert(QStringLiteral("line_height"), s.lineHeight);
        o.insert(QStringLiteral("fill"), colorText(s.fill));
        o.insert(QStringLiteral("y"), s.y);

        // 只有真的存在才写 stroke / shadow —— 解析侧是「有键 ⇒ hasStroke = true」
        if (s.hasStroke) {
            QJsonObject st;
            st.insert(QStringLiteral("color"), colorText(s.strokeColor));
            st.insert(QStringLiteral("width"), s.strokeWidth);
            st = keepUnknown(st, rawStyle.value(QLatin1String("stroke")).toObject(),
                             known({"color", "width"}));
            o.insert(QStringLiteral("stroke"), st);
        }
        if (s.hasShadow) {
            QJsonArray off;
            off.append(s.shadowOffX);
            off.append(s.shadowOffY);
            QJsonObject sh;
            sh.insert(QStringLiteral("offset"), off);
            sh.insert(QStringLiteral("blur"), s.shadowBlur);
            sh.insert(QStringLiteral("color"), colorText(s.shadowColor));
            sh = keepUnknown(sh, rawStyle.value(QLatin1String("shadow")).toObject(),
                             known({"offset", "blur", "color"}));
            o.insert(QStringLiteral("shadow"), sh);
        }

        o = keepUnknown(o, rawStyle,
                        known({"id", "font", "weight", "size", "letter_spacing", "line_height",
                               "fill", "y", "stroke", "shadow"}));
        stylesArr.append(o);
    }

    // ── lyrics
    const QHash<QString, QJsonObject> rawLyrics =
        indexById(src.value(QLatin1String("lyrics")).toArray());
    QJsonArray lyricsArr;
    for (const Lyric &l : lyrics) {
        QJsonArray syl;
        for (const Syllable &sy : l.syllables) {
            QJsonObject so;
            so.insert(QStringLiteral("text"), sy.text);
            so.insert(QStringLiteral("start"), sy.start);
            so.insert(QStringLiteral("end"), sy.end);
            syl.append(so);
        }
        QJsonObject o;
        o.insert(QStringLiteral("id"), l.id);
        o.insert(QStringLiteral("start"), l.start);
        o.insert(QStringLiteral("end"), l.end);
        o.insert(QStringLiteral("text"), l.text);
        o.insert(QStringLiteral("syllables"), syl);
        o.insert(QStringLiteral("style"), l.style);
        o = keepUnknown(o, rawLyrics.value(l.id),
                        known({"id", "start", "end", "text", "syllables", "style"}));
        lyricsArr.append(o);
    }

    // ── shots（locked / motion.exit 之类由 rawSource 按 id 认领回来）
    const QHash<QString, QJsonObject> rawShots =
        indexById(src.value(QLatin1String("shots")).toArray());
    QJsonArray shotsArr;
    for (const Shot &s : shots) {
        const QJsonObject rawShot = rawShots.value(s.id);

        QJsonObject layers;
        layers.insert(QStringLiteral("background"), partsToJson(s.background));
        layers.insert(QStringLiteral("text"), partsToJson(s.text));
        layers.insert(QStringLiteral("decor"), partsToJson(s.decor));
        layers.insert(QStringLiteral("overlay"), partsToJson(s.overlay));
        layers = keepUnknown(layers, rawShot.value(QLatin1String("layers")).toObject(),
                             known({"background", "text", "decor", "overlay"}));

        // exit 没有建模，靠 keepUnknown 从 motion 里带回来
        QJsonObject motion;
        motion.insert(QStringLiteral("entrance"), optionalPart(s.entrance, s.hasEntrance));
        motion.insert(QStringLiteral("hold"), optionalPart(s.hold, s.hasHold));
        motion.insert(QStringLiteral("camera"), optionalPart(s.camera, s.hasCamera));
        motion = keepUnknown(motion, rawShot.value(QLatin1String("motion")).toObject(),
                             known({"entrance", "hold", "camera"}));

        QJsonArray refs;
        for (const QString &r : s.lyricRefs)
            refs.append(r);

        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("start"), s.start);
        o.insert(QStringLiteral("end"), s.end);
        o.insert(QStringLiteral("track"), s.track);
        o.insert(QStringLiteral("lyric_refs"), refs);
        o.insert(QStringLiteral("layers"), layers);
        o.insert(QStringLiteral("motion"), motion);
        o.insert(QStringLiteral("transition"), optionalPart(s.transition, s.hasTransition));
        o.insert(QStringLiteral("seed"), s.seed);
        o = keepUnknown(o, rawShot,
                        known({"id", "start", "end", "track", "lyric_refs", "layers", "motion",
                               "transition", "seed"}));
        shotsArr.append(o);
    }

    // ── assets（模型是唯一真相；每个资源里没建模的键仍从 rawSource 补回）
    const QHash<QString, QJsonObject> rawAssets =
        indexById(src.value(QLatin1String("assets")).toArray());
    QJsonArray assetsArr;
    for (const Asset &a : assets) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), a.id);
        o.insert(QStringLiteral("kind"), a.kind);
        o.insert(QStringLiteral("path"), a.path);
        o.insert(QStringLiteral("sha1"), a.sha1);
        o = keepUnknown(o, rawAssets.value(a.id),
                        known({"id", "kind", "path", "sha1"}));
        assetsArr.append(o);
    }

    // ── 根
    //
    // QJsonObject 的键是有序的（按字母），直接序列化会让 .sf 以 "assets" 开头，
    // 与样本文件的阅读顺序差太远。根这一层按规范顺序自己拼，嵌套层仍交给 writeJson。
    const QJsonValue srcExport = src.value(QLatin1String("export"));
    const QJsonValue exp = srcExport.isObject() ? srcExport : QJsonValue(defaultExport());

    QStringList chunks;
    const auto push = [&chunks](const QString &key, const QJsonValue &v) {
        QString s = QStringLiteral("  \"") + key + QStringLiteral("\": ");
        writeJson(v, 1, s);
        chunks << s;
    };
    push(QStringLiteral("format"), QJsonValue(QStringLiteral("ShizukuFlow")));
    push(QStringLiteral("version"), QJsonValue(1));
    push(QStringLiteral("meta"), meta);
    push(QStringLiteral("canvas"), cv);
    push(QStringLiteral("media"), media);
    push(QStringLiteral("timing"), tm);
    push(QStringLiteral("styles"), stylesArr);
    push(QStringLiteral("lyrics"), lyricsArr);
    push(QStringLiteral("shots"), shotsArr);
    push(QStringLiteral("assets"), assetsArr);
    push(QStringLiteral("export"), exp);

    // 根上模型没建模的键（原样保留，不丢）
    const QStringList canonicalRoot = known({"format", "version", "meta", "canvas", "media",
                                             "timing", "styles", "lyrics", "shots", "assets",
                                             "export"});
    for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
        if (canonicalRoot.contains(it.key()))
            continue;
        push(it.key(), it.value());
    }

    QString out;
    out.reserve(96 * 1024);
    out += QLatin1String("{\n");
    out += chunks.join(QLatin1String(",\n"));
    out += QLatin1String("\n}\n");
    return out;
}

bool Project::save(const QString &filePath, QString *error) const
{
    const QByteArray bytes = toJson().toUtf8();

    // QSaveFile：先写同目录临时文件、commit 时才 rename —— 中途崩溃不会留半个工程
    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入文件：%1（%2）").arg(filePath, f.errorString());
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        if (error)
            *error = QStringLiteral("写入不完整：%1").arg(f.errorString());
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("保存失败：%1").arg(f.errorString());
        return false;
    }
    return true;
}

double Project::duration() const
{
    double end = 0;
    for (const Shot &s : shots)
        end = std::max(end, s.end);
    return std::max(end, audioDuration);
}

int Project::frameCount() const
{
    return int(std::lround(duration() * canvas.fps));
}

QVector<double> Project::shotStarts() const
{
    QVector<double> out;
    out.reserve(shots.size());
    for (const Shot &s : shots)
        out << s.start;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const StyleDef *Project::style(const QString &id) const
{
    for (const StyleDef &s : styles)
        if (s.id == id)
            return &s;
    return styles.isEmpty() ? nullptr : &styles.first();
}

int Project::shotIndexAt(double t) const
{
    int best = -1;
    for (int i = 0; i < shots.size(); ++i) {
        const Shot &s = shots[i];
        if (t >= s.start && t < s.end)
            return i;
    }
    for (int i = 0; i < shots.size(); ++i)
        if (shots[i].start <= t)
            best = i;
    return best;
}

const Lyric *Project::lyric(const QString &id) const
{
    for (const Lyric &l : lyrics)
        if (l.id == id)
            return &l;
    return nullptr;
}

const Asset *Project::asset(const QString &id) const
{
    for (const Asset &a : assets)
        if (a.id == id)
            return &a;
    return nullptr;
}

QString Project::assetPath(const QString &id) const
{
    const Asset *a = asset(id);
    if (!a || a->path.isEmpty())
        return QString();

    const QFileInfo fi(a->path);
    if (fi.isAbsolute())
        return fi.absoluteFilePath();

    // 相对路径按「.sf 所在目录」解析；工程还没落盘时退回当前工作目录
    const QString base = path.isEmpty() ? QDir::currentPath() : QFileInfo(path).absolutePath();
    return QDir(base).absoluteFilePath(a->path);
}

} // namespace sf
