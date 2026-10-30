#include "sf_audio.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace sf {
namespace {

/** 每桶采样数。40 × 100桶/秒 = 4000Hz —— 人声基频的富集区，够用又不至于解太慢。 */
constexpr int kSamplesPerBucket = 40;
constexpr int kSampleRate = AudioPeaks::bucketsPerSecond * kSamplesPerBucket;

QString resolveFfmpeg(const QString &hint)
{
    auto usable = [](const QString &p) {
        return !p.isEmpty() && QFileInfo::exists(p);
    };
    if (usable(hint))
        return hint;
    const QString env = qEnvironmentVariable("SHIZUKUFLOW_FFMPEG");
    if (usable(env))
        return env;
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (usable(onPath))
        return onPath;
    return QString();
}

} // namespace

AudioPeaks computePeaks(const QString &audioPath, const QString &ffmpegPath, QString *error)
{
    AudioPeaks out;
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return AudioPeaks();
    };

    if (audioPath.isEmpty() || !QFileInfo::exists(audioPath))
        return fail(QStringLiteral("音轨不存在：%1").arg(audioPath));
    const QString ff = resolveFfmpeg(ffmpegPath);
    if (ff.isEmpty())
        return fail(QStringLiteral("找不到 ffmpeg"));

    QProcess p;
    p.setProgram(ff);
    // s16le 单声道：体积是 float 的一半，峰值统计用不上 24bit 的精度。
    p.setArguments({QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"),
                    audioPath, QStringLiteral("-vn"), QStringLiteral("-ac"), QStringLiteral("1"),
                    QStringLiteral("-ar"), QString::number(kSampleRate), QStringLiteral("-f"),
                    QStringLiteral("s16le"), QStringLiteral("-")});
    p.start();
    if (!p.waitForStarted(10000))
        return fail(QStringLiteral("ffmpeg 启动失败：%1").arg(p.errorString()));
    if (!p.waitForFinished(300000)) {
        p.kill();
        p.waitForFinished(3000);
        return fail(QStringLiteral("ffmpeg 解码超时"));
    }
    const QByteArray raw = p.readAllStandardOutput();
    if (raw.size() < 2)
        return fail(QStringLiteral("ffmpeg 没有输出 PCM：%1")
                        .arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed()));

    const int n = raw.size() / 2;
    const auto *samples = reinterpret_cast<const qint16 *>(raw.constData());
    const int buckets = (n + kSamplesPerBucket - 1) / kSamplesPerBucket;
    out.low.resize(buckets);
    out.high.resize(buckets);
    for (int b = 0; b < buckets; ++b) {
        const int from = b * kSamplesPerBucket;
        const int to = std::min(n, from + kSamplesPerBucket);
        float lo = 0.0f;
        float hi = 0.0f;
        for (int i = from; i < to; ++i) {
            const float v = samples[i] / 32768.0f;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        out.low[b] = lo;
        out.high[b] = hi;
    }
    out.duration = double(n) / kSampleRate;
    return out;
}

BeatInfo detectBeats(const AudioPeaks &peaks)
{
    BeatInfo info;
    if (peaks.isEmpty() || peaks.duration < 4.0)
        return info;

    const int n = peaks.high.size();
    // 能量包络：取绝对值大的那一侧。鼓点是双极性的，只看 high 会漏掉反相的部分。
    QVector<float> env(n);
    for (int i = 0; i < n; ++i)
        env[i] = std::max(std::fabs(peaks.high[i]), std::fabs(peaks.low[i]));

    // 3 桶滑动平均，压掉高频噪声，让自相关只看「起音」这种慢包络。
    QVector<float> smooth(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        float s = 0.0f;
        int c = 0;
        for (int k = -1; k <= 1; ++k)
            if (i + k >= 0 && i + k < n) {
                s += env[i + k];
                ++c;
            }
        smooth[i] = s / std::max(1, c);
    }

    // 起音强度 = 半波整流后的一阶差分：只有「变响」才算打击。
    QVector<float> onset(n, 0.0f);
    for (int i = 1; i < n; ++i)
        onset[i] = std::max(0.0f, smooth[i] - smooth[i - 1]);

    const double bps = AudioPeaks::bucketsPerSecond;
    const int lagMin = std::max(2, int(std::lround(bps * 60.0 / 200.0))); // 200 BPM
    const int lagMax = std::min(n / 2, int(std::lround(bps * 60.0 / 60.0))); // 60 BPM
    if (lagMax <= lagMin + 1)
        return info;

    // 前缀平方和：后面算归一化自相关时要 O(1) 拿到任意区间的能量。
    QVector<double> sq(n + 1, 0.0);
    for (int i = 0; i < n; ++i)
        sq[i + 1] = sq[i] + double(onset[i]) * onset[i];

    // 归一化自相关（Pearson 式）。之前用「总和 / 剩余样本数」，长 lag 天然占便宜，
    // 会把 130 BPM 听成 65 —— 半速偏差的来源就在这个分母上。
    auto score = [&](int lag) {
        double num = 0.0;
        for (int i = 0; i + lag < n; ++i)
            num += double(onset[i]) * onset[i + lag];
        const double ea = sq[n - lag] - sq[0];
        const double eb = sq[n] - sq[lag];
        const double den = std::sqrt(std::max(1e-12, ea * eb));
        return num / den;
    };

    QVector<double> sc(lagMax + 2, 0.0);
    double mean = 0.0;
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        // 节奏先验：绝大多数流行歌落在 70~170。只做轻微加权（σ 一个八度），
        // 用来打破自相关本来就分不清的倍频歧义，不硬性限制。
        const double bpm = 60.0 * bps / lag;
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 1.0, 2.0));
        sc[lag] = score(lag) * prior;
        mean += sc[lag];
    }
    mean /= double(lagMax - lagMin + 1);
    if (mean <= 1e-9)
        return info;

    int best = lagMin;
    for (int lag = lagMin; lag <= lagMax; ++lag)
        if (sc[lag] > sc[best])
            best = lag;

    // 倍频歧义：自相关在 lag 与 2*lag 上都会出峰。若半拍位置（lag/2）分数也不低，
    // 说明真实的鼓点是它的两倍速（比如 70 被听成 140 的反面），按「落在 70~170」取。
    auto improved = [&](int lag) { return lag > 0 && sc[lag] > 0.62 * sc[best]; };
    if (best / 2 >= lagMin && improved(best / 2)) {
        int local = best / 2;
        int pick = local;
        for (int l = std::max(lagMin, local - 2); l <= std::min(lagMax, local + 2); ++l)
            if (sc[l] > sc[pick])
                pick = l;
        best = pick;
    } else if (best * 2 <= lagMax && sc[best * 2] > 0.85 * sc[best]) {
        best *= 2;
    }

    // 抛物线插值：整数 lag 的分辨率只有 1%，直接取整会让 128 BPM 显示成 131。
    double frac = 0.0;
    if (best > lagMin && best < lagMax) {
        const double a = sc[best - 1];
        const double b = sc[best];
        const double c = sc[best + 1];
        const double denom = (a - 2 * b + c);
        if (std::fabs(denom) > 1e-12)
            frac = 0.5 * (a - c) / denom;
    }
    const double lag = best + std::clamp(frac, -0.5, 0.5);
    if (lag <= 0.0)
        return info;

    // 相位：拍点落在哪。枚举一个拍长内的偏移，取起音和最大者。
    const int period = std::max(1, int(std::lround(lag)));
    int bestPhase = 0;
    double bestPhaseScore = -1.0;
    for (int ph = 0; ph < period && ph < n; ++ph) {
        double s = 0.0;
        for (int i = ph; i < n; i += period)
            s += onset[i];
        if (s > bestPhaseScore) {
            bestPhaseScore = s;
            bestPhase = ph;
        }
    }

    info.bpm = 60.0 * bps / lag;
    info.offset = bestPhase / bps;
    info.confidence = mean > 0.0 ? sc[best] / mean : 0.0;
    return info;
}

} // namespace sf
