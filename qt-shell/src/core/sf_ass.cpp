#include "sf_ass.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace sf {
namespace {

/** ASS 的时间戳：H:MM:SS.cc，小时不补零，厘秒两位。 */
QString assTime(double seconds)
{
    const long long cs = std::llround(std::max(0.0, seconds) * 100.0);
    const long long h = cs / 360000;
    const long long m = (cs / 6000) % 60;
    const long long s = (cs / 100) % 60;
    const long long c = cs % 100;
    return QString::asprintf("%lld:%02lld:%02lld.%02lld", h, m, s, c);
}

/** QColor → &HAABBGGRR。注意 ASS 的 alpha 是「反的」：00 不透明，FF 全透明。 */
QString assColor(const QColor &c)
{
    const QColor rgb = c.toRgb();
    const int a = 255 - rgb.alpha();
    return QString::asprintf("&H%02X%02X%02X%02X", a, rgb.blue(), rgb.green(), rgb.red());
}

/** 花括号是 ASS 的覆盖块边界，正文里出现会吃掉后面的字；换行得写成 \N。 */
QString assText(const QString &raw)
{
    QString s = raw;
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char('{'), QStringLiteral("\\{"));
    s.replace(QLatin1Char('}'), QStringLiteral("\\}"));
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\\N"));
    s.replace(QLatin1Char('\n'), QStringLiteral("\\N"));
    s.replace(QLatin1Char('\r'), QStringLiteral("\\N"));
    return s;
}

QString styleLine(const StyleDef &st, const CanvasDef &cv, const QString &name)
{
    // Bold 在 ASS 里是布尔（-1/0），只有「粗/不粗」两档；把 weight 的连续性丢在这里，
    // 因为字幕渲染器本来也只认这两档。
    const int bold = st.weight >= 700 ? -1 : 0;
    const int outline = st.hasStroke ? std::max(1, int(std::lround(st.strokeWidth))) : 0;
    const int shadow = st.hasShadow ? std::max(1, int(std::lround(st.shadowOffY))) : 0;
    // 字号按画布高度归一：渲染里 size 是「画布高 1080 时的像素」，换画布要跟着缩放。
    const double size = st.size * (cv.height > 0 ? cv.height / 1080.0 : 1.0);
    return QStringLiteral("Style: %1,%2,%3,%4,%4,%5,%6,%7,0,0,0,100,100,%8,0,1,%9,%10,5,0,0,0,1")
        .arg(name, st.font)
        .arg(QString::number(std::max(1, int(std::lround(size)))))
        .arg(assColor(st.fill))
        .arg(assColor(st.hasStroke ? st.strokeColor : QColor(0, 0, 0, 0)))
        .arg(assColor(st.hasShadow ? st.shadowColor : QColor(0, 0, 0, 128)))
        .arg(bold)
        .arg(QString::number(st.letterSpacing, 'f', 1))
        .arg(outline)
        .arg(shadow);
}

/** 取歌词对应的样式：优先 id 命中，否则第一条，再否则内置兜底。 */
StyleDef styleFor(const Project &p, const Lyric &l)
{
    if (const StyleDef *s = p.style(l.style))
        return *s;
    if (!p.styles.isEmpty())
        return p.styles.first();
    StyleDef d;
    d.id = QStringLiteral("Default");
    return d;
}

QString styleNameOf(const Project &p, const Lyric &l)
{
    const StyleDef s = styleFor(p, l);
    return s.id.isEmpty() ? QStringLiteral("Default") : s.id;
}

} // namespace

QString toAss(const Project &p, const AssExportOptions &opt)
{
    QString out;
    out.reserve(4096 + p.lyrics.size() * 96);
    out += QStringLiteral("[Script Info]\n");
    out += QStringLiteral("; 由 ShizukuFlow 导出 —— 歌词时间戳与画面同源，不会漂\n");
    out += QStringLiteral("Title: %1\n").arg(p.title.isEmpty() ? QStringLiteral("untitled") : p.title);
    if (!p.artist.isEmpty())
        out += QStringLiteral("Original Script: %1\n").arg(p.artist);
    out += QStringLiteral("ScriptType: v4.00+\n");
    out += QStringLiteral("WrapStyle: 2\n"); // 2 = 不自动换行，交给创作者
    out += QStringLiteral("ScaledBorderAndShadow: yes\n");
    out += QStringLiteral("YCbCr Matrix: TV.709\n");
    out += QStringLiteral("PlayResX: %1\n").arg(p.canvas.width);
    out += QStringLiteral("PlayResY: %1\n").arg(p.canvas.height);
    out += QStringLiteral("\n[V4+ Styles]\n");
    out += QStringLiteral("Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                          "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
                          "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
                          "MarginL, MarginR, MarginV, Encoding\n");

    // 样式表：工程里声明过的全给上，哪怕这一行没用到 —— 字幕组改样式时不用回头翻工程。
    QStringList names;
    if (p.styles.isEmpty()) {
        StyleDef d;
        d.id = QStringLiteral("Default");
        out += styleLine(d, p.canvas, d.id) + QLatin1Char('\n');
        names << d.id;
    } else {
        for (const StyleDef &st : p.styles) {
            const QString name = st.id.isEmpty() ? QStringLiteral("Default") : st.id;
            out += styleLine(st, p.canvas, name) + QLatin1Char('\n');
            names << name;
        }
    }
    Q_UNUSED(names)

    out += QStringLiteral("\n[Events]\n");
    out += QStringLiteral("Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
                          "Effect, Text\n");

    const double H = p.canvas.height > 0 ? p.canvas.height : 1080.0;
    const double W = p.canvas.width > 0 ? p.canvas.width : 1920.0;

    for (const Lyric &l : p.lyrics) {
        const double end = l.end > l.start ? l.end : l.start;
        const StyleDef st = styleFor(p, l);

        // 定位：渲染里歌词中心落在 (0.5, style.y)，这里用 an5+\pos 复现同一位置。
        QString head = QStringLiteral("{\\an5");
        if (opt.alignToStyleY)
            head += QStringLiteral("\\pos(%1,%2)")
                        .arg(QString::number(W * 0.5, 'f', 0), QString::number(H * st.y, 'f', 0));
        if (opt.fade)
            head += QStringLiteral("\\fad(120,120)");
        head += QLatin1Char('}');

        QString body;
        const bool hasK = opt.karaoke && !l.syllables.isEmpty();
        if (hasK) {
            for (const Syllable &sy : l.syllables) {
                const int cs = int(std::lround(std::max(0.0, sy.end - sy.start) * 100.0));
                // 空音节也要占位：它的时长属于整行，丢掉会让整行提前收尾。
                body += QStringLiteral("{\\k%1}%2").arg(cs).arg(assText(sy.text));
            }
            if (body.isEmpty())
                body = assText(l.text);
        } else {
            body = assText(l.text);
        }

        out += QStringLiteral("Dialogue: 0,%1,%2,%3,,0,0,0,,%4%5\n")
                   .arg(assTime(l.start), assTime(end), styleNameOf(p, l), head, body);
    }
    return out;
}

bool exportAss(const Project &p, const QString &path, QString *error, const AssExportOptions &opt)
{
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("输出路径为空");
        return false;
    }
    // QSaveFile：先写临时文件再原子替换，导出中途崩掉不会留半份字幕。
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QByteArray bom("\xEF\xBB\xBF", 3);
    f.write(bom);
    f.write(toAss(p, opt).toUtf8());
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace sf
