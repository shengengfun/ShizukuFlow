#include "sf_lrc.h"

#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace sf {
namespace {

/** 时间标签：[mm:ss.xx] / [mm:ss:xx] / [mm:ss]。 */
const QRegularExpression &timeTagRe()
{
    static const QRegularExpression re(QStringLiteral("^\\[(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?\\]"));
    return re;
}

/** 逐字标签：<mm:ss.xx>。 */
const QRegularExpression &wordTagRe()
{
    static const QRegularExpression re(QStringLiteral("<(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?>"));
    return re;
}

double tagSeconds(const QRegularExpressionMatch &m)
{
    double sec = m.captured(1).toInt() * 60.0 + m.captured(2).toInt();
    const QString frac = m.captured(3);
    if (!frac.isEmpty()) {
        // 两位 = 厘秒，三位 = 毫秒。不按位数区分的话 [00:01.5] 会被当成 5 毫秒。
        sec += frac.toInt() / std::pow(10.0, frac.size());
    }
    return sec;
}

/** 解码：UTF-8 严格优先，失败回退系统 ANSI（中文 Windows 上就是 GBK）。 */
QString decodeText(const QByteArray &raw)
{
    QByteArray body = raw;
    if (body.startsWith("\xEF\xBB\xBF"))
        body.remove(0, 3);

    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString s = utf8(body);
    if (!utf8.hasError())
        return s;

    QStringDecoder ansi(QStringConverter::System, QStringConverter::Flag::Stateless);
    s = ansi(body);
    if (!ansi.hasError())
        return s;
    return QString::fromLatin1(body);
}

QString lrcTime(double seconds)
{
    const long long cs = std::max<long long>(0, std::llround(seconds * 100.0));
    return QString::asprintf("%02lld:%02lld.%02lld", cs / 6000, (cs / 100) % 60, cs % 100);
}

} // namespace

LrcLoadResult parseLrc(const QByteArray &raw)
{
    LrcLoadResult res;
    const QString text = decodeText(raw);
    if (text.trimmed().isEmpty()) {
        res.error = QStringLiteral("空文件");
        return res;
    }

    // 先收集 [时间, 文本]，同一个文本可能挂多个时间标签（副歌复用）。
    struct Entry
    {
        double at;
        QString text;
    };
    QVector<Entry> entries;
    const auto &tag = timeTagRe();
    const auto &word = wordTagRe();

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        // 元数据标签：[ti:] [ar:] [al:] [by:] [offset:] —— 不是时间，单独收。
        if (line.startsWith(QLatin1Char('[')) && !tag.match(line).hasMatch()) {
            const int colon = line.indexOf(QLatin1Char(':'));
            const int close = line.lastIndexOf(QLatin1Char(']'));
            if (colon > 1 && close > colon) {
                const QString key = line.mid(1, colon - 1).trimmed().toLower();
                const QString val = line.mid(colon + 1, close - colon - 1).trimmed();
                if (key == QLatin1String("ti"))
                    res.title = val;
                else if (key == QLatin1String("ar"))
                    res.artist = val;
                else if (key == QLatin1String("offset"))
                    res.offsetMs = val.toDouble();
            }
            continue;
        }

        QVector<double> times;
        while (true) {
            const auto m = tag.match(line);
            if (!m.hasMatch())
                break;
            times.push_back(tagSeconds(m));
            line = line.mid(m.capturedLength());
        }
        if (times.isEmpty())
            continue;

        QString body = line.trimmed();
        // 空行歌词（纯间奏标记）也保留 —— 它代表「这一段时间不显示歌词」，
        // 丢掉会让后一行提前顶上来。
        for (double t : times) {
            Entry e;
            e.at = t;
            e.text = body;
            entries.push_back(e);
        }
    }

    if (entries.isEmpty()) {
        res.error = QStringLiteral("没有解析到任何带时间标签的歌词行");
        return res;
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &a, const Entry &b) { return a.at < b.at; });

    const double shift = -res.offsetMs / 1000.0;
    res.lyrics.reserve(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        Lyric l;
        l.start = std::max(0.0, entries[i].at + shift);
        l.text = entries[i].text;
        l.id = QString::asprintf("L%03d", i + 1);

        // 增强格式：<mm:ss.xx>字<mm:ss.xx>字 —— 拆成逐字时间。
        const QString &body = entries[i].text;
        QRegularExpressionMatchIterator hits = word.globalMatch(body);
        QVector<QRegularExpressionMatch> marks;
        while (hits.hasNext())
            marks.push_back(hits.next());
        if (!marks.isEmpty()) {
            QString plain;
            for (int k = 0; k < marks.size(); ++k) {
                const int from = marks[k].capturedEnd();
                const int to = (k + 1 < marks.size()) ? marks[k + 1].capturedStart() : body.size();
                Syllable s;
                s.text = body.mid(from, to - from);
                s.start = std::max(0.0, tagSeconds(marks[k]) + shift);
                plain += s.text;
                l.syllables.push_back(s);
            }
            l.text = plain;
        }
        res.lyrics.push_back(l);
    }

    // 行尾：取下一行的起点。最后一行没有下一行，按 4 秒兜底（.lrc 本身不记结束时间）。
    for (int i = 0; i < res.lyrics.size(); ++i) {
        Lyric &l = res.lyrics[i];
        l.end = (i + 1 < res.lyrics.size()) ? res.lyrics[i + 1].start : l.start + 4.0;
        if (l.end <= l.start + 0.05)
            l.end = l.start + 0.05;
        for (int k = 0; k < l.syllables.size(); ++k) {
            Syllable &s = l.syllables[k];
            s.end = (k + 1 < l.syllables.size()) ? l.syllables[k + 1].start : l.end;
            if (s.end <= s.start)
                s.end = s.start + 0.02;
        }
    }
    return res;
}

bool importLrc(const QString &path, Project &project, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const LrcLoadResult res = parseLrc(f.readAll());
    if (!res.ok()) {
        if (error)
            *error = res.error;
        return false;
    }
    project.lyrics = res.lyrics;
    if (project.title.isEmpty())
        project.title = res.title;
    if (project.artist.isEmpty())
        project.artist = res.artist;
    return true;
}

QString toLrc(const Project &p)
{
    QString out;
    out += QStringLiteral("[ti:%1]\r\n")
               .arg(p.title.isEmpty() ? QStringLiteral("untitled") : p.title);
    if (!p.artist.isEmpty())
        out += QStringLiteral("[ar:%1]\r\n").arg(p.artist);
    out += QStringLiteral("[by:ShizukuFlow]\r\n");
    out += QStringLiteral("[offset:0]\r\n");

    for (const Lyric &l : p.lyrics) {
        if (l.syllables.isEmpty()) {
            out += QStringLiteral("[%1]%2\r\n").arg(lrcTime(l.start), l.text);
            continue;
        }
        // 逐字用增强格式：每个字前面挂自身起点，播放器不认也能退化成整行。
        QString line = QStringLiteral("[%1]").arg(lrcTime(l.start));
        for (const Syllable &s : l.syllables)
            line += QStringLiteral("<%1>%2").arg(lrcTime(s.start), s.text);
        out += line + QStringLiteral("\r\n");
    }
    return out;
}

bool exportLrc(const Project &p, const QString &path, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(toLrc(p).toUtf8());
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace sf
