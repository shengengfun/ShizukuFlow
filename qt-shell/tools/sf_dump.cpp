/**
 * sf_dump —— 无头渲染自检工具
 *
 * 用法：
 *   sf_dump <project.sf> <t秒> <out.png> [宽 高]
 *   sf_dump <project.sf> --info
 *
 * 用于验证「工程 → 内核 → 一帧 PNG」这条链路，不需要任何界面。
 */
#include "core/sf_ass.h"
#include "core/sf_audio.h"
#include "core/sf_lint.h"
#include "core/sf_lrc.h"
#include "core/sf_project.h"
#include "core/sf_render.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static void dumpDraws(const std::vector<lume::Draw> &ds, int depth)
{
    for (const auto &d : ds) {
        fprintf(stdout, "%*skind=%d x=%.1f y=%.1f w=%.1f h=%.1f alpha=%.2f\n", depth * 2, "",
                int(d.kind), d.x, d.y, d.w, d.h, d.alpha);
        if (d.kind == lume::Draw::Text && d.layout) {
            fprintf(stdout, "%*s  text totalWidth=%.1f boxes=%d fillA=%d stroke=%d shadow=%d/%.1f\n",
                    depth * 2, "", d.layout->totalWidth, d.layout->boxes.size(),
                    d.style.fill.alpha(), int(d.style.hasStroke), int(d.style.hasShadow),
                    d.style.shadowBlur);
        }
        dumpDraws(d.sub, depth + 1);
    }
}

/** 跑一组时刻，输出单帧耗时分布与缓存增量。 */
static int runBench(QTextStream &out, sf::Project &project, const std::vector<double> &times,
                    int W, int H)
{
    if (times.empty())
        return 2;

    sf::FrameRenderer r;
    r.setProject(&project);

    // 预热一次：字体加载 + 首帧字形烘焙不该算进稳态耗时
    QImage scratch;
    r.renderInto(scratch, times.front(), W, H);
    const auto s0 = r.cacheStats();
    const auto b0 = r.bgStats();

    std::vector<double> cost;
    std::vector<double> compileCost;
    cost.reserve(times.size());
    compileCost.reserve(times.size());
    for (double t : times) {
        // 引擎自己上报编译耗时，不额外 compile 一次——否则会把缓存烘热，测出的数字偏乐观。
        // 复用同一块缓冲：这条路径与预览窗完全一致。
        double compileMs = 0;
        QElapsedTimer clock;
        clock.start();
        r.renderInto(scratch, t, W, H, &compileMs);
        cost.push_back(clock.nsecsElapsed() / 1e6);
        compileCost.push_back(compileMs);
        if (scratch.isNull()) {
            out << "渲染失败\n";
            return 1;
        }
    }

    const auto s1 = r.cacheStats();
    const auto b1 = r.bgStats();
    std::sort(cost.begin(), cost.end());
    const auto pct = [&cost](double p) {
        const size_t i = std::min(cost.size() - 1, size_t(double(cost.size()) * p));
        return cost[i];
    };
    double sum = 0;
    for (double v : cost)
        sum += v;

    const int iw = W > 0 ? W : project.canvas.width;
    const int ih = H > 0 ? H : project.canvas.height;
    const double avg = sum / double(cost.size());
    const double budget = 1000.0 / std::max(1, project.canvas.fps);

    out << QString::asprintf("帧数 %d   画幅 %dx%d   %.2fs → %.2fs\n", int(times.size()), iw, ih,
                             times.front(), times.back());
    out << QString::asprintf("单帧   min %.2f  avg %.2f  p50 %.2f  p95 %.2f  max %.2f (ms)\n",
                             cost.front(), avg, pct(0.50), pct(0.95), cost.back());
    {
        double csum = 0;
        for (double v : compileCost)
            csum += v;
        std::sort(compileCost.begin(), compileCost.end());
        const size_t ci = std::min(compileCost.size() - 1, size_t(double(compileCost.size()) * 0.5));
        out << QString::asprintf("拆分   编译含烘焙 avg %.2f / p50 %.2f  |  绘制+分配 avg %.2f (ms)\n",
                                 csum / double(compileCost.size()), compileCost[ci],
                                 avg - csum / double(compileCost.size()));
    }
    const qint64 hits = s1.glyphHits - s0.glyphHits;
    const qint64 miss = s1.glyphMisses - s0.glyphMisses;
    const qint64 totalGlyph = hits + miss;
    out << QString::asprintf("字形   命中 %lld / 未命中 %lld", hits, miss);
    if (totalGlyph > 0)
        out << QString::asprintf("  → %.1f%%", 100.0 * double(hits) / double(totalGlyph));
    out << QString::asprintf("   缓存 %d 条\n", s1.glyphEntries);
    out << QString::asprintf("背景   烘焙 %lld 次 / 缓存 %d 条\n", b1.renders - b0.renders,
                             b1.entries);
    out << QString::asprintf("预算   %dfps = %.1fms/帧  → 余量 %.0f%%\n", project.canvas.fps,
                             budget, 100.0 * (1.0 - avg / budget));
    return 0;
}

/** 工程里的音轨路径可能是相对的（相对 .sf 所在目录），解析成绝对路径。
    返回空串表示「没有可用音轨」——调用方据此决定 -an 还是 mux。 */
static QString resolveAudioPath(const sf::Project &p, const QString &override)
{
    // 显式给的路径按「当前工作目录」解析 —— 它是用户在命令行里敲的。
    if (!override.isEmpty()) {
        const QFileInfo fi(override);
        const QString abs = fi.isAbsolute() ? override : fi.absoluteFilePath();
        return QFileInfo::exists(abs) ? QFileInfo(abs).absoluteFilePath() : QString();
    }
    // 工程里存的路径按「.sf 所在目录」解析 —— .sf 里存的是相对工程的路径。
    if (p.audioPath.isEmpty())
        return QString();
    const QFileInfo fi(p.audioPath);
    if (fi.isAbsolute())
        return fi.exists() ? fi.absoluteFilePath() : QString();
    const QFileInfo proj(p.path);
    const QString cand = QDir(proj.absolutePath()).absoluteFilePath(p.audioPath);
    return QFileInfo::exists(cand) ? cand : QString();
}

/** 定位 ffmpeg：显式参数 → 环境变量 → 与本 exe 相对的常见位置 → PATH。 */
static QString findFfmpeg(const QString &explicitPath)
{
    if (!explicitPath.isEmpty())
        return explicitPath;
    const QByteArray env = qgetenv("SHIZUKUFLOW_FFMPEG");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        dir + QStringLiteral("/tools/ffmpeg.exe"),
        dir + QStringLiteral("/../../app/src-tauri/tools/ffmpeg.exe"),
        dir + QStringLiteral("/../app/src-tauri/tools/ffmpeg.exe"),
    };
    for (const QString &c : candidates) {
        const QFileInfo fi(c);
        if (fi.exists() && fi.isFile())
            return fi.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

/** 写一帧进 ffmpeg 的 stdin。
    QImage 的 bytesPerLine 可能带行尾填充，ffmpeg 只认紧排数据，所以按行校验。 */
static bool writeFrame(QProcess &ff, const QImage &img)
{
    const int rowBytes = img.width() * 4;
    const int bpl = img.bytesPerLine();
    if (bpl == rowBytes)
        return ff.write(reinterpret_cast<const char *>(img.constBits()),
                        qint64(rowBytes) * img.height()) >= 0;
    for (int y = 0; y < img.height(); ++y)
        if (ff.write(reinterpret_cast<const char *>(img.constScanLine(y)), rowBytes) < 0)
            return false;
    return true;
}

/** 输出格式：由扩展名决定。写错的扩展名不该悄悄出一份 H.264。 */
enum class ExportKind { Mp4, Gif, Webp, PngSeq };

static ExportKind exportKindOf(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("gif"))
        return ExportKind::Gif;
    if (ext == QLatin1String("webp"))
        return ExportKind::Webp;
    // 没扩展名 = 当成目录 → PNG 序列（交给 AviUtl / AE 之类的下游继续加工）
    if (ext.isEmpty() || path.endsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('\\')))
        return ExportKind::PngSeq;
    return ExportKind::Mp4;
}

/**
 * 并行导出 MP4：把区间按帧号切成 N 段，各起一个 sf_dump 子进程去渲染，
 * 最后用 concat demuxer 无缝拼接，再单独 mux 音轨。
 *
 * 为什么是「进程」并行而不是线程：渲染内核里有共享的字体/图片/视频帧缓存，
 * 做成线程安全要付全局锁的代价，反而比多进程慢；而且进程级并行天然能吃到
 * 多核，中途某个分片崩了也不会带死整条流水线，重跑该分片即可。
 */
static int runExportParallel(QTextStream &out, const sf::Project &project, const QString &outPath,
                             const QString &ff, double fps, int ow, int oh, double start,
                             double end, int jobs, const QString &audio, bool mute,
                             const QString &cacheDir)
{
    const int total = std::max(1, int(std::ceil((end - start) * fps)));
    jobs = std::clamp(jobs, 1, std::min(16, total));

    QTemporaryDir tmp(QDir::tempPath() + QStringLiteral("/sf_export_XXXXXX"));
    if (!tmp.isValid()) {
        out << "无法创建临时目录\n";
        return 1;
    }
    const QString projAbs = QFileInfo(project.path).absoluteFilePath();
    const QString self = QCoreApplication::applicationFilePath();

    // 帧号切分：用整数边界算，避免各分片累计的浮点误差啃掉一帧。
    QVector<int> bound(jobs + 1);
    for (int k = 0; k <= jobs; ++k)
        bound[k] = int(qint64(total) * k / jobs);

    // 增量缓存：每个分片的「指纹」只包含落在它时间范围内的镜头与歌词。
    // 改一个镜头 → 只有覆盖它的分片失效，其余直接复用上次的 mp4。
    const bool useCache = !cacheDir.isEmpty();
    // 缓存路径必须绝对：拼接清单落在临时目录里，ffmpeg 会以清单所在目录为基准解析
    // 相对路径，相对缓存目录会导致「文件明明在却找不到」。
    QString cacheAbs = useCache ? QFileInfo(cacheDir).absoluteFilePath() : QString();
    QJsonObject full;
    if (useCache) {
        if (!QDir().mkpath(cacheAbs)) {
            out << "无法创建缓存目录：" << cacheAbs << "\n";
            return 1;
        }
        full = QJsonDocument::fromJson(project.toJson().toUtf8()).object();
    }

    struct Slice
    {
        int f0 = 0;
        int f1 = 0;
        double t0 = 0;
        double t1 = 0;
        QString target;
        QString part;
        bool cached = false;
        std::unique_ptr<QProcess> proc;
    };
    std::vector<Slice> slices;
    int hits = 0;
    for (int k = 0; k < jobs; ++k) {
        if (bound[k + 1] - bound[k] <= 0)
            continue;
        Slice s;
        s.f0 = bound[k];
        s.f1 = bound[k + 1];
        s.t0 = start + double(s.f0) / fps;
        // 上界减半帧：runExport 用 ceil 算帧数，半帧余量保证它恰好切出 n 帧而不是 n+1。
        s.t1 = start + (double(s.f1) - 0.5) / fps;

        if (useCache) {
            const QJsonArray allShots = full.value(QStringLiteral("shots")).toArray();
            const QJsonArray allLyrics = full.value(QStringLiteral("lyrics")).toArray();
            const double a = s.t0;
            const double b = s.t1 + 0.5 / fps;
            QJsonArray shots;
            QJsonArray lyrics;
            for (const QJsonValue &v : allShots) {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("start")).toDouble() < b
                    && o.value(QStringLiteral("end")).toDouble() > a)
                    shots.append(v);
            }
            for (const QJsonValue &v : allLyrics) {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("start")).toDouble() < b
                    && o.value(QStringLiteral("end")).toDouble() > a)
                    lyrics.append(v);
            }
            QJsonObject key;
            key[QStringLiteral("v")] = 2;
            key[QStringLiteral("canvas")] = full.value(QStringLiteral("canvas"));
            key[QStringLiteral("styles")] = full.value(QStringLiteral("styles"));
            key[QStringLiteral("fps")] = fps;
            key[QStringLiteral("w")] = ow;
            key[QStringLiteral("h")] = oh;
            key[QStringLiteral("f0")] = s.f0;
            key[QStringLiteral("f1")] = s.f1;
            key[QStringLiteral("shots")] = shots;
            key[QStringLiteral("lyrics")] = lyrics;
            const QByteArray h =
                QCryptographicHash::hash(QJsonDocument(key).toJson(QJsonDocument::Compact),
                                         QCryptographicHash::Sha1)
                    .toHex();
            const QString base = QString::fromLatin1(h).left(20);
            s.target = QDir(cacheAbs).filePath(base + QStringLiteral(".mp4"));
            s.part = QDir(cacheAbs).filePath(base + QStringLiteral(".part.mp4"));
            s.cached = QFileInfo(s.target).size() > 1024;
            if (s.cached)
                ++hits;
        } else {
            s.target = tmp.filePath(QStringLiteral("seg_%1.mp4").arg(k, 2, 10, QLatin1Char('0')));
            s.part = s.target;
        }

        if (!s.cached) {
            auto p = std::make_unique<QProcess>();
            p->setProgram(self);
            p->setArguments({projAbs, QStringLiteral("--export"), s.part, QStringLiteral("--fps"),
                             QString::number(fps, 'f', 3), QStringLiteral("--size"),
                             QStringLiteral("%1x%2").arg(ow).arg(oh), QStringLiteral("--range"),
                             QString::number(s.t0, 'f', 4), QString::number(s.t1, 'f', 4),
                             QStringLiteral("--no-audio"), QStringLiteral("--ffmpeg"), ff});
            p->setProcessChannelMode(QProcess::SeparateChannels);
            p->start();
            s.proc = std::move(p);
        }
        slices.push_back(std::move(s));
    }

    out << "并行导出：" << slices.size() << " 个分片 / " << total << " 帧";
    if (useCache)
        out << "（缓存命中 " << hits << "）";
    out << "\n";
    out.flush();
    QElapsedTimer clock;
    clock.start();
    for (size_t i = 0; i < slices.size(); ++i) {
        std::unique_ptr<QProcess> &p = slices[i].proc;
        if (!p)
            continue;
        if (!p->waitForStarted(10000)) {
            out << "分片 " << i << " 启动失败: " << p->errorString() << "\n";
            return 1;
        }
    }
    for (size_t i = 0; i < slices.size(); ++i) {
        Slice &s = slices[i];
        if (!s.proc)
            continue;
        // 超时按「整段时长 × 4」给，单分片不该比整段还慢。
        if (!s.proc->waitForFinished(int((end - start) * 4000) + 120000)) {
            out << "分片 " << i << " 超时\n";
            s.proc->kill();
            return 1;
        }
        if (s.proc->exitCode() != 0) {
            out << "分片 " << i << " 失败：\n"
                << QString::fromLocal8Bit(s.proc->readAllStandardError()) << "\n";
            return 1;
        }
        if (s.part != s.target) {
            // 先写 .part 再改名：中途被打断的缓存文件永远不会被当成成品读回来。
            QFile::remove(s.target);
            if (!QFile::rename(s.part, s.target)) {
                out << "缓存落盘失败: " << s.target << "\n";
                return 1;
            }
        }
        out << "  分片 " << i + 1 << "/" << slices.size() << " 完成\n";
        out.flush();
    }

    QStringList segs;
    for (const Slice &s : slices)
        segs << s.target;

    // 拼接：各段编码参数完全一致，concat + copy 不重新编码，几毫秒完成。
    const QString listPath = tmp.filePath(QStringLiteral("list.txt"));
    {
        QFile lf(listPath);
        if (!lf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            out << "无法写拼接清单\n";
            return 1;
        }
        QTextStream ls(&lf);
        for (const QString &s : segs)
            ls << "file '" << QDir::fromNativeSeparators(s) << "'\n";
    }

    const bool withAudio = !mute && !audio.isEmpty();
    const QString joined = withAudio ? tmp.filePath(QStringLiteral("joined.mp4")) : outPath;

    QStringList cc;
    cc << "-y" << "-f" << "concat" << "-safe" << "0" << "-i" << listPath << "-c" << "copy";
    if (withAudio)
        cc << "-an";
    cc << "-movflags" << "+faststart" << joined;

    QProcess cat;
    cat.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    cat.start(ff, cc);
    if (!cat.waitForStarted(10000) || !cat.waitForFinished(600000) || cat.exitCode() != 0) {
        out << "拼接失败（ffmpeg 退出码 " << cat.exitCode() << "）\n";
        return 1;
    }

    if (withAudio) {
        // 音轨单独 mux 一次：分片阶段全部 -an，声画对位只在最后这一步做，
        // 少一个分片就少一处可能错位的 seek。
        QStringList mux;
        mux << "-y" << "-i" << joined;
        if (start > 0.0005)
            mux << "-ss" << QString::number(start, 'f', 3);
        mux << "-i" << audio << "-c:v" << "copy" << "-c:a" << "aac" << "-b:a" << "192k" << "-ar"
            << "48000" << "-shortest" << "-movflags" << "+faststart" << outPath;
        QProcess mx;
        mx.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        mx.start(ff, mux);
        if (!mx.waitForStarted(10000) || !mx.waitForFinished(600000) || mx.exitCode() != 0) {
            out << "混音失败（ffmpeg 退出码 " << mx.exitCode() << "）\n";
            return 1;
        }
    }

    const double secs = clock.elapsed() / 1000.0;
    const qint64 bytes = QFileInfo(outPath).size();
    if (bytes <= 0) {
        out << "导出失败：产物为空\n";
        return 1;
    }
    out << "完成 " << outPath << "（MP4 · " << segs.size() << " 路并行" << (useCache ? "，增量缓存" : "")
        << "）\n";
    out << QString::asprintf("  %dx%d @%.2ffps  %d 帧  %.2fs → %.2fs\n", ow, oh, fps, total, start,
                             end);
    out << "  音轨 " << (withAudio ? QFileInfo(audio).fileName() : QStringLiteral("（无）")) << "\n";
    out << QString::asprintf("  用时 %.2fs（%.1f 帧/秒，%.2fx 实时）  体积 %.1f MB\n", secs,
                             total / std::max(0.001, secs), (total / std::max(0.001, secs)) / fps,
                             bytes / 1048576.0);
    return 0;
}

/**
 * 全片（或指定区间）导出：内核逐帧渲染 → 管道喂 ffmpeg → 编码 / 或直接落 PNG 序列。
 *
 * 与 Tauri 版的关键差别：不落盘、不过 IPC。帧在内存里直接进 ffmpeg 的 stdin，
 * 省掉「每帧一张 JPEG 写磁盘、再被 ffmpeg 读回来」两次往返。
 *
 * 格式按扩展名分派：.mp4/.mov/.mkv → H.264 + AAC；.gif → 调色板量化动图；
 * .webp → 动画 WebP；目录 → PNG 序列。
 */
static int runExport(QTextStream &out, sf::Project &project, const QString &outPath, double fpsIn,
                     int W, int H, double t0, double t1, const QString &ffmpegPath,
                     const QString &audioOverride, bool mute, int jobs = 1,
                     const QString &cacheDir = QString())
{
    const ExportKind kind = exportKindOf(outPath);
    const bool seq = kind == ExportKind::PngSeq;

    // PNG 序列纯 Qt 落盘，用不到 ffmpeg。别为了个不需要转码的格式把整个导出卡住。
    QString ff;
    if (!seq) {
        ff = findFfmpeg(ffmpegPath);
        if (ff.isEmpty() || !QFileInfo::exists(ff)) {
            out << "找不到 ffmpeg：用 --ffmpeg <路径> 指定，或设 SHIZUKUFLOW_FFMPEG\n";
            return 1;
        }
    }

    // 显式指定的音轨不存在时必须报错，不能静默出一段无声视频
    if (!mute && !audioOverride.isEmpty() && !QFileInfo::exists(audioOverride)) {
        out << "指定的音轨不存在：" << audioOverride << "\n";
        return 1;
    }
    // GIF / WebP / PNG 序列没有音轨概念，音轨由下游合成。这一层直接掐掉，
    // 免得用户以为选了个 .gif 还能带声音。
    const bool audioCapable = kind == ExportKind::Mp4;
    const QString audio =
        (audioCapable && !mute) ? resolveAudioPath(project, audioOverride) : QString();
    const bool hasAudio = !audio.isEmpty();
    if (audioCapable && !mute && !hasAudio && !project.audioPath.isEmpty()) {
        out << "警告：工程引用的音轨找不到（" << project.audioPath << "），本次导出无声\n";
    }

    const double fps = fpsIn > 0 ? fpsIn : (project.canvas.fps > 0 ? project.canvas.fps : 30.0);
    const int ow = W > 0 ? W : project.canvas.width;
    const int oh = H > 0 ? H : project.canvas.height;
    const double dur = project.duration() > 0 ? project.duration() : 10.0;
    const double start = std::clamp(t0, 0.0, dur);
    const double end = std::clamp((t1 > t0 ? t1 : dur), start, dur);
    const int total = std::max(1, int(std::ceil((end - start) * fps)));

    // 分片并行只对 MP4 有意义：动图/序列本身就是逐帧落盘，多开进程只会抢 IO。
    if (jobs > 1 && kind == ExportKind::Mp4)
        return runExportParallel(out, project, outPath, ff, fps, ow, oh, start, end, jobs, audio,
                                 mute, cacheDir);

    // PNG 序列先兜住目录（序列是"很多文件"，不是单个产物，体积在循环里累加）
    QDir seqDir;
    if (seq) {
        seqDir = QDir(outPath);
        if (!seqDir.exists() && !QDir().mkpath(seqDir.absolutePath())) {
            out << "无法创建输出目录：" << outPath << "\n";
            return 1;
        }
    }
    if (kind == ExportKind::Gif && fps > 30)
        out << "提示：GIF 在 " << fps << "fps 下体积会很大，建议 --fps 15~20 或加 --size\n";

    QProcess ffProc;
    if (!seq) {
        QStringList ffArgs;
        ffArgs << "-y" << "-f" << "rawvideo" << "-pix_fmt" << "rgba" << "-s"
               << QStringLiteral("%1x%2").arg(ow).arg(oh) << "-r" << QString::number(fps) << "-i"
               << "-";
        if (hasAudio) {
            // 输入侧 seek，与视频帧从同一时刻起算，否则声画错位。
            // 必须放在 -i 之前（快速 seek）；FLAC 这类无损容器能精确到样本。
            if (start > 0.0005)
                ffArgs << "-ss" << QString::number(start, 'f', 3);
            ffArgs << "-i" << audio;
        }
        switch (kind) {
        case ExportKind::Mp4:
            ffArgs << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "18"
                   << "-pix_fmt" << "yuv420p";
            if (hasAudio)
                // -ar 48000：源可能是 96k（FLAC 常见），但 AAC 在高采样率下播放器兼容性差，
                // 平台侧多半也会重采样。统一 48k 是交付标准，听不出差别。
                ffArgs << "-c:a" << "aac" << "-b:a" << "192k" << "-ar" << "48000" << "-shortest";
            else
                ffArgs << "-an";
            ffArgs << "-movflags" << "+faststart";
            break;
        case ExportKind::Gif:
            // 单遍调色板：split 出一支统计颜色、另一支拿去上色。
            // stats_mode=diff 只统计变化区域，静态背景不会白占 256 色。
            ffArgs << "-vf"
                   << "split[a][b];[a]palettegen=max_colors=256:stats_mode=diff[p];"
                      "[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle";
            ffArgs << "-loop" << "0" << "-an";
            break;
        case ExportKind::Webp:
            ffArgs << "-c:v" << "libwebp" << "-lossless" << "0" << "-q:v" << "82"
                   << "-compression_level" << "4" << "-loop" << "0" << "-an";
            break;
        case ExportKind::PngSeq:
            break;
        }
        ffArgs << outPath;

        ffProc.setProgram(ff);
        ffProc.setArguments(ffArgs);
        ffProc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        ffProc.start();
        if (!ffProc.waitForStarted(10000)) {
            out << "ffmpeg 启动失败: " << ffProc.errorString() << "\n";
            return 1;
        }
    }

    sf::FrameRenderer r;
    r.setProject(&project);
    QImage scratch;
    r.renderInto(scratch, start, ow, oh); // 预热：字体加载与首帧烘焙不进稳态耗时

    QElapsedTimer clock;
    clock.start();
    int written = 0;
    qint64 seqBytes = 0;
    const int step = std::max(1, total / 10);
    for (int i = 0; i < total; ++i) {
        // 把帧时刻钉到 fps 网格上。分片导出的起点不是 0，直接累加 i/fps 会把浮点误差
        // 带进「恰好落在镜头切点」的那一帧：时而算进前一个镜头（画出场闪白），时而算进
        // 后一个镜头。对齐网格后串行/并行/任意切片算出的 double 完全一致。
        const double t = std::round((start + i / fps) * fps) / fps;
        r.renderInto(scratch, t, ow, oh);
        if (seq) {
            const QString f = seqDir.filePath(QString::asprintf("frame_%05d.png", i));
            if (!scratch.save(f, "PNG")) {
                out << "写帧失败（第 " << i << " 帧）\n";
                return 1;
            }
            seqBytes += QFileInfo(f).size();
        } else {
            // 统一转 RGBA8888：ARGB32_Premultiplied 在小端字节序是 BGRA 且预乘 alpha，
            // 直接喂 ffmpeg 会在半透明处发暗。一帧转一次，开销远小于编码。
            const QImage rgba = scratch.convertToFormat(QImage::Format_RGBA8888);
            if (rgba.isNull() || !writeFrame(ffProc, rgba)) {
                out << "写帧失败（第 " << i << " 帧）\n";
                ffProc.kill();
                ffProc.waitForFinished(3000);
                return 1;
            }
            // 背压：ffmpeg 读得慢时不要无限堆内存
            if (ffProc.bytesToWrite() > 64LL * 1024 * 1024)
                ffProc.waitForBytesWritten(30000);
        }
        ++written;
        if (i % step == 0)
            out << QString::asprintf("  %d/%d 帧  (%.0f%%)\n", i + 1, total,
                                     100.0 * (i + 1) / total);
        out.flush();
    }

    const double secs = clock.elapsed() / 1000.0;
    qint64 bytes = seqBytes;
    if (!seq) {
        ffProc.closeWriteChannel();
        if (!ffProc.waitForFinished(600000)) {
            out << "ffmpeg 超时未结束\n";
            ffProc.kill();
            return 1;
        }
        bytes = QFileInfo(outPath).size();
        if (ffProc.exitCode() != 0 || bytes <= 0) {
            out << "导出失败，ffmpeg 退出码 " << ffProc.exitCode() << "\n";
            return 1;
        }
    }

    static const char *kKindName[] = {"MP4", "GIF", "WebP", "PNG 序列"};
    out << "完成 " << outPath << "（" << kKindName[int(kind)] << "）\n";
    out << QString::asprintf("  %dx%d @%.2ffps  %d 帧  %.2fs → %.2fs\n", ow, oh, fps, written,
                             start, end);
    if (audioCapable)
        out << "  音轨 " << (hasAudio ? QFileInfo(audio).fileName() : QStringLiteral("（无）"))
            << "\n";
    else
        out << "  音轨 （该格式不带音轨）\n";
    out << QString::asprintf("  用时 %.2fs（%.1f 帧/秒，%.2fx 实时）  体积 %.1f MB\n", secs,
                             written / std::max(0.001, secs), (written / std::max(0.001, secs)) / fps,
                             bytes / 1048576.0);
    return 0;
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QTextStream out(stdout);

    const QStringList args = app.arguments();
    if (args.size() < 3) {
        out << "用法: sf_dump <project.sf> <t|--info|--draws|--bench|--export> [参数]\n"
               "  --bench [帧数] [w h]   稳态耗时与缓存命中率\n"
               "  --export <out> [--fps N] [--size WxH] [--range t0 t1]\n"
               "                     [--audio <path>] [--no-audio] [--ffmpeg path]\n"
               "                     [--jobs N] [--cache <dir>]\n"
               "                        内核逐帧渲染 → 管道喂 ffmpeg（不落盘、不过 IPC）\n"
               "                        --jobs N：MP4 按帧切片给 N 个进程并行渲染再拼接\n"
               "                        --cache <dir>：分片按内容指纹缓存，重导只渲染改动过的段\n"
               "                        格式按扩展名：.mp4/.mov → H.264+AAC，.gif → 动图，\n"
               "                        .webp → 动画 WebP，无扩展名/目录 → PNG 序列\n"
               "                        音轨默认取工程 media.audio.path，--audio 可覆盖\n"
               "  --ass <out.ass> [--no-karaoke] [--no-fade]\n"
               "                        歌词导出 ASS 字幕（时间戳与画面同源，可直接进压制流程）\n"
               "  --audio [--peaks-png out.png] [--audio-file path] [--ffmpeg path]\n"
               "                        真实波形峰值 + BPM/首拍检测（时间轴画波形就靠它）\n"
               "  --lrc <out.lrc>       歌词导出 LRC（逐字用增强格式 <mm:ss.xx>）\n"
               "  --lrc-import <in.lrc> <out.sf>\n"
               "                        读入 LRC 替换歌词后另存工程\n"
               "  --still <t> [out.png] [--size WxH]\n"
               "                        单帧静帧（封面 / 调画面时确认某一时刻）\n"
               "  --contact out.png [--cols N] [--rows N] [--cell WxH] [--from t] [--to t]\n"
               "                        联系表：整片铺成一张网格图，审片用\n"
               "  --thumbs <dir> [--every 秒] [--cell WxH] [--range t0 t1]\n"
               "                        时间轴胶片条缩略图（thumb_<毫秒>.png）\n";
        return 2;
    }

    sf::Project project;
    QString err;
    if (!project.load(args[1], &err)) {
        out << "载入失败: " << err << "\n";
        return 1;
    }

    if (args[2] == QLatin1String("--info")) {
        out << "标题   : " << project.title << " / " << project.artist << "\n";
        out << "画幅   : " << project.canvas.width << "x" << project.canvas.height << " @"
            << project.canvas.fps << "fps\n";
        out << "时长   : " << project.duration() << "s (" << project.frameCount() << " 帧)\n";
        out << "镜头   : " << project.shots.size() << "  歌词: " << project.lyrics.size()
            << "  样式: " << project.styles.size() << "\n";
        out << "BPM    : " << project.bpm << "  偏移 " << project.offset << "\n";
        return 0;
    }

    if (args[2] == QLatin1String("--lrc")) {
        if (args.size() < 4) {
            out << "用法: sf_dump <project.sf> --lrc <out.lrc>\n";
            return 2;
        }
        if (!sf::exportLrc(project, args[3], &err)) {
            out << "导出 LRC 失败: " << err << "\n";
            return 1;
        }
        int words = 0;
        for (const sf::Lyric &l : project.lyrics)
            if (!l.syllables.isEmpty())
                ++words;
        out << "已写入 " << args[3] << "  " << project.lyrics.size() << " 行（其中 " << words
            << " 行带逐字时间）\n";
        return 0;
    }

    if (args[2] == QLatin1String("--lrc-import")) {
        // 把 .lrc 灌进当前工程并另存一份：不覆盖原工程，改坏了还能退。
        if (args.size() < 5) {
            out << "用法: sf_dump <project.sf> --lrc-import <in.lrc> <out.sf>\n";
            return 2;
        }
        const int before = project.lyrics.size();
        if (!sf::importLrc(args[3], project, &err)) {
            out << "导入 LRC 失败: " << err << "\n";
            return 1;
        }
        if (!project.save(args[4], &err)) {
            out << "写工程失败: " << err << "\n";
            return 1;
        }
        out << "已导入 " << project.lyrics.size() << " 行歌词（原 " << before << " 行）→ " << args[4]
            << "\n";
        return 0;
    }

    if (args[2] == QLatin1String("--still")) {
        // 单帧静帧：出封面、出素材、以及调画面时快速确认某一时刻到底长什么样。
        if (args.size() < 4) {
            out << "用法: sf_dump <project.sf> --still <t> [out.png] [--size WxH]\n";
            return 2;
        }
        const double t = args[3].toDouble();
        QString path = QStringLiteral("still.png");
        int W = 0;
        int H = 0;
        for (int i = 4; i < args.size(); ++i) {
            if (args[i] == QLatin1String("--size") && i + 1 < args.size()) {
                const QStringList wh = args[++i].split(QLatin1Char('x'));
                if (wh.size() == 2) {
                    W = wh[0].toInt();
                    H = wh[1].toInt();
                }
            } else if (!args[i].startsWith(QLatin1Char('-'))) {
                path = args[i];
            }
        }
        sf::FrameRenderer r;
        r.setProject(&project);
        QImage img;
        r.renderInto(img, t, W, H);
        if (img.isNull() || !img.save(path, "PNG")) {
            out << "写静帧失败: " << path << "\n";
            return 1;
        }
        out << QString::asprintf("静帧 t=%.3f  %dx%d → %s\n", t, img.width(), img.height(),
                                 qUtf8Printable(path));
        return 0;
    }

    if (args[2] == QLatin1String("--lint")) {
        // 导出前自检：坏路径/空镜头/时间重叠在这里报出来，而不是变成成片里的黑屏。
        bool strict = false;
        for (int i = 3; i < args.size(); ++i)
            if (args[i] == QLatin1String("--strict"))
                strict = true;
        const QVector<sf::LintIssue> issues =
            sf::lintProject(project, QFileInfo(project.path).absolutePath());
        for (const sf::LintIssue &i : issues) {
            const char *tag = i.level == sf::LintIssue::Error ? "ERROR" : "WARN ";
            out << tag << "  " << i.where << "  " << i.message << "\n";
        }
        out << sf::lintSummary(issues) << "\n";
        int errors = 0;
        for (const sf::LintIssue &i : issues)
            if (i.level == sf::LintIssue::Error)
                ++errors;
        return (strict && !issues.isEmpty()) || errors > 0 ? 1 : 0;
    }

    if (args[2] == QLatin1String("--thumbs")) {
        // 时间轴胶片条：按固定间隔抽小图，文件名直接带毫秒时间，UI 不用再读清单。
        if (args.size() < 4) {
            out << "用法: sf_dump <project.sf> --thumbs <dir> [--every 秒] [--cell WxH] "
                   "[--range t0 t1]\n";
            return 2;
        }
        const QString dirPath = args[3];
        double every = 2.0;
        int cw = 192;
        int ch = 108;
        const double dur = project.duration() > 0 ? project.duration() : 10.0;
        double from = 0.0;
        double to = dur;
        for (int i = 4; i < args.size(); ++i) {
            const QString a = args[i];
            if (i + 1 >= args.size())
                break;
            if (a == QLatin1String("--every"))
                every = std::max(0.1, args[++i].toDouble());
            else if (a == QLatin1String("--range") && i + 2 < args.size()) {
                from = args[++i].toDouble();
                to = args[++i].toDouble();
            } else if (a == QLatin1String("--cell")) {
                const QStringList wh = args[++i].split(QLatin1Char('x'));
                if (wh.size() == 2) {
                    cw = std::max(16, wh[0].toInt());
                    ch = std::max(9, wh[1].toInt());
                }
            }
        }
        if (!QDir().mkpath(dirPath)) {
            out << "无法创建缩略图目录：" << dirPath << "\n";
            return 1;
        }
        sf::FrameRenderer r;
        r.setProject(&project);
        QImage cell;
        int n = 0;
        for (double t = from; t < to; t += every) {
            r.renderInto(cell, t, cw, ch);
            const QString f =
                QDir(dirPath).filePath(QStringLiteral("thumb_%1.png").arg(qint64(t * 1000), 7, 10,
                                                                       QLatin1Char('0')));
            if (!cell.save(f, "PNG"))
                continue;
            ++n;
        }
        out << "缩略图 " << n << " 张（" << cw << "x" << ch << "，每 " << every << "s）→ " << dirPath
            << "\n";
        return 0;
    }

    if (args[2] == QLatin1String("--contact")) {
        // 联系表（contact sheet）：整片按网格铺开，一眼看完整场戏的明暗与节奏。
        // 审片时比拖时间轴快得多，也方便直接把一张图发给别人。
        if (args.size() < 4) {
            out << "用法: sf_dump <project.sf> --contact out.png [--cols N] [--rows N] "
                   "[--cell WxH] [--from t] [--to t]\n";
            return 2;
        }
        QString path = args[3];
        int cols = 6;
        int rows = 4;
        int cw = 320;
        int ch = 180;
        double from = 0.0;
        const double dur = project.duration();
        double to = dur > 0 ? dur : 10.0;
        for (int i = 4; i < args.size(); ++i) {
            const QString a = args[i];
            if (i + 1 >= args.size())
                break;
            if (a == QLatin1String("--cols"))
                cols = std::clamp(args[++i].toInt(), 1, 32);
            else if (a == QLatin1String("--rows"))
                rows = std::clamp(args[++i].toInt(), 1, 32);
            else if (a == QLatin1String("--from"))
                from = args[++i].toDouble();
            else if (a == QLatin1String("--to"))
                to = args[++i].toDouble();
            else if (a == QLatin1String("--cell")) {
                const QStringList wh = args[++i].split(QLatin1Char('x'));
                if (wh.size() == 2) {
                    cw = std::max(16, wh[0].toInt());
                    ch = std::max(9, wh[1].toInt());
                }
            }
        }
        if (to <= from)
            to = from + 1.0;
        const int n = cols * rows;
        const int gap = 2;
        QImage sheet(cols * cw + (cols - 1) * gap, rows * ch + (rows - 1) * gap,
                     QImage::Format_RGB32);
        sheet.fill(QColor(0, 0, 0));
        sf::FrameRenderer r;
        r.setProject(&project);
        QImage cell;
        QPainter g(&sheet);
        for (int k = 0; k < n; ++k) {
            // 每个格子取区间中点，避免第 0 格永远是 t=0 的黑场。
            const double t = from + (to - from) * (k + 0.5) / n;
            r.renderInto(cell, t, cw, ch);
            const int x = (k % cols) * (cw + gap);
            const int y = (k / cols) * (ch + gap);
            g.drawImage(x, y, cell);
            g.setPen(QColor(255, 255, 255, 200));
            QFont f = g.font();
            f.setPixelSize(std::max(10, ch / 12));
            g.setFont(f);
            const int mins = int(t) / 60;
            const int secs = int(t) % 60;
            g.drawText(QRect(x + 4, y + ch - ch / 8 - 4, cw - 8, ch / 8),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QString::asprintf("%02d:%02d", mins, secs));
        }
        g.end();
        if (!sheet.save(path, "PNG")) {
            out << "写联系表失败: " << path << "\n";
            return 1;
        }
        out << QString::asprintf("联系表 %dx%d 格（%d×%d / 格，%.2fs–%.2fs）→ %s\n", cols, rows, cw,
                                 ch, from, to, qUtf8Printable(path));
        return 0;
    }

    if (args[2] == QLatin1String("--draws")) {
        const double td = args.size() > 3 ? args[3].toDouble() : 0.0;
        sf::FrameRenderer r;
        r.setProject(&project);
        std::vector<lume::Draw> draws;
        r.compile(td, draws, project.canvas.width, project.canvas.height);
        fprintf(stdout, "== t=%.2f  %d top-level draws ==\n", td, int(draws.size()));
        dumpDraws(draws, 0);
        return 0;
    }

    if (args[2] == QLatin1String("--bench") || args[2] == QLatin1String("--bench-seq")) {
        // 稳态帧耗时与缓存命中率。优化前后跑同一条命令，数字才可比。
        //   --bench     <帧数> [w h]        全片均匀抽样（近似冷缓存，测镜头切换成本）
        //   --bench-seq <帧数> <起始秒> [w h] 连续走帧（真实播放路径）
        const bool seq = args[2] == QLatin1String("--bench-seq");
        const int frames = args.size() > 3 ? std::max(1, args[3].toInt()) : 120;
        int argi = 4;
        double from = 0;
        if (seq && args.size() > argi)
            from = args[argi++].toDouble();
        const int W = args.size() > argi ? args[argi].toInt() : 0;
        const int H = args.size() > argi + 1 ? args[argi + 1].toInt() : 0;

        const double fps = project.canvas.fps > 0 ? project.canvas.fps : 30.0;
        const double dur = project.duration() > 0 ? project.duration() : 10.0;
        std::vector<double> times;
        times.reserve(size_t(frames));
        for (int i = 0; i < frames; ++i)
            times.push_back(seq ? from + i / fps : dur * (i + 1) / frames);

        out << (seq ? "模式   连续播放\n" : "模式   全片抽样\n");
        return runBench(out, project, times, W, H);
    }

    if (args[2] == QLatin1String("--audio")) {
        QString ffPath;
        QString auPath;
        QString pngPath;
        for (int i = 3; i < args.size(); ++i) {
            if (args[i] == QLatin1String("--ffmpeg") && i + 1 < args.size())
                ffPath = args[++i];
            else if (args[i] == QLatin1String("--audio-file") && i + 1 < args.size())
                auPath = args[++i];
            else if (args[i] == QLatin1String("--peaks-png") && i + 1 < args.size())
                pngPath = args[++i];
        }
        const QString audio = resolveAudioPath(project, auPath);
        if (audio.isEmpty()) {
            out << "工程没有可用音轨\n";
            return 1;
        }
        const sf::AudioPeaks peaks = sf::computePeaks(audio, findFfmpeg(ffPath), &err);
        if (peaks.isEmpty()) {
            out << "音频分析失败: " << err << "\n";
            return 1;
        }
        const sf::BeatInfo beat = sf::detectBeats(peaks);
        out << "音轨 " << QFileInfo(audio).fileName() << "\n";
        out << QString::asprintf("  时长 %.3fs  峰值桶 %d（%d 桶/秒）\n", peaks.duration,
                                 int(peaks.high.size()), sf::AudioPeaks::bucketsPerSecond);
        if (beat.bpm > 0)
            out << QString::asprintf("  BPM %.2f  首拍偏移 %.3fs  置信度 %.2f\n", beat.bpm,
                                     beat.offset, beat.confidence);
        else
            out << "  BPM 未测出（音轨太短或过于平坦）\n";
        if (!pngPath.isEmpty()) {
            // 波形 + 拍点网格的对照图：看两件事——包络是否和歌的段落对得上、
            // 竖线是否压在鼓点上。命令行也能出图，出问题不用开界面猜。
            const int W = 1600;
            const int H = 260;
            QImage img(W, H, QImage::Format_RGB32);
            img.fill(QColor(16, 18, 22));
            QPainter g(&img);
            g.setRenderHint(QPainter::Antialiasing, false);
            const int n = peaks.high.size();
            const double sx = n > 0 ? double(W) / n : 1.0;
            g.setPen(QColor(96, 196, 255));
            for (int x = 0; x < W; ++x) {
                const int a = std::min(n - 1, int(x / sx));
                const int b = std::max(a + 1, std::min(n, int((x + 1) / sx)));
                float lo = 0.0f;
                float hi = 0.0f;
                for (int i = a; i < b; ++i) {
                    lo = std::min(lo, peaks.low[i]);
                    hi = std::max(hi, peaks.high[i]);
                }
                const int y0 = int(H * 0.5 * (1.0 - hi));
                const int y1 = int(H * 0.5 * (1.0 - lo));
                g.drawLine(x, y0, x, std::max(y0 + 1, y1));
            }
            if (beat.bpm > 0) {
                g.setPen(QColor(255, 180, 80, 150));
                const double period = 60.0 / beat.bpm;
                for (double t = beat.offset; t < peaks.duration; t += period)
                    g.drawLine(int(t / peaks.duration * W), 0, int(t / peaks.duration * W), H);
            }
            g.end();
            if (!img.save(pngPath, "PNG")) {
                out << "写波形图失败: " << pngPath << "\n";
                return 1;
            }
            out << "  波形图 " << pngPath << "\n";
        }
        return 0;
    }

    if (args[2] == QLatin1String("--ass")) {
        if (args.size() < 4) {
            out << "用法: sf_dump <project.sf> --ass <out.ass> [--no-karaoke] [--no-fade]\n";
            return 2;
        }
        sf::AssExportOptions opt;
        for (int i = 4; i < args.size(); ++i) {
            if (args[i] == QLatin1String("--no-karaoke"))
                opt.karaoke = false;
            else if (args[i] == QLatin1String("--no-fade"))
                opt.fade = false;
        }
        if (!sf::exportAss(project, args[3], &err, opt)) {
            out << "导出字幕失败: " << err << "\n";
            return 1;
        }
        int karaokeLines = 0;
        for (const sf::Lyric &l : project.lyrics)
            if (opt.karaoke && !l.syllables.isEmpty())
                ++karaokeLines;
        out << "已写入 " << args[3] << "  " << project.lyrics.size() << " 行歌词（其中 "
            << karaokeLines << " 行带逐字卡拉OK）\n";
        return 0;
    }

    if (args[2] == QLatin1String("--export")) {
        if (args.size() < 4) {
            out << "用法: sf_dump <project.sf> --export <out> [--fps N] [--size WxH] "
                   "[--range t0 t1] [--audio <path>] [--no-audio] [--ffmpeg path]\n"
                   "  out 扩展名决定格式：.mp4 / .gif / .webp / 目录（PNG 序列）\n";
            return 2;
        }
        const QString outPath = args[3];
        double fpsArg = 0;
        int ew = 0;
        int eh = 0;
        double t0 = 0;
        double t1 = -1;
        QString ffPath;
        QString auPath;
        bool mute = false;
        int jobsArg = 1;
        QString cacheArg;
        for (int i = 4; i < args.size(); ++i) {
            if (args[i] == QLatin1String("--fps") && i + 1 < args.size()) {
                fpsArg = args[++i].toDouble();
            } else if (args[i] == QLatin1String("--size") && i + 1 < args.size()) {
                const QStringList wh = args[++i].split(QLatin1Char('x'));
                if (wh.size() == 2) {
                    ew = wh[0].toInt();
                    eh = wh[1].toInt();
                }
            } else if (args[i] == QLatin1String("--range") && i + 2 < args.size()) {
                t0 = args[++i].toDouble();
                t1 = args[++i].toDouble();
            } else if (args[i] == QLatin1String("--ffmpeg") && i + 1 < args.size()) {
                ffPath = args[++i];
            } else if (args[i] == QLatin1String("--audio") && i + 1 < args.size()) {
                auPath = args[++i];
            } else if (args[i] == QLatin1String("--no-audio")) {
                mute = true;
            } else if (args[i] == QLatin1String("--jobs") && i + 1 < args.size()) {
                jobsArg = std::clamp(args[++i].toInt(), 1, 16);
            } else if (args[i] == QLatin1String("--cache") && i + 1 < args.size()) {
                cacheArg = args[++i];
            }
        }
        return runExport(out, project, outPath, fpsArg, ew, eh, t0, t1, ffPath, auPath, mute,
                         jobsArg, cacheArg);
    }

    const double t = args[2].toDouble();
    const QString outPath = args.size() > 3 ? args[3] : QStringLiteral("frame.png");
    int w = 0, h = 0;
    if (args.size() >= 6) {
        w = args[4].toInt();
        h = args[5].toInt();
    }

    sf::FrameRenderer r;
    r.setProject(&project);

    QElapsedTimer timer;
    timer.start();
    // 预热一次（布局/图集首次命中必然 miss，单独计时更公平）
    const QImage warm = r.render(t, w, h);
    const qint64 warmMs = timer.elapsed();

    timer.restart();
    const QImage img = r.render(t, w, h);
    const qint64 ms = timer.elapsed();

    if (img.isNull() || !img.save(outPath)) {
        out << "渲染/保存失败\n";
        return 1;
    }
    const auto st = r.cacheStats();
    const auto bg = r.bgStats();
    out << "已写入 " << outPath << "  " << img.width() << "x" << img.height() << "\n";
    out << "首帧 " << warmMs << "ms / 次帧 " << ms << "ms"
        << "  （字形命中 " << st.glyphHits << " / 未命中 " << st.glyphMisses << "）\n";
    out << "背景缓存 " << bg.entries << " 条 / 烘焙 " << bg.renders << " 次\n";
    Q_UNUSED(warm)
    return 0;
}
