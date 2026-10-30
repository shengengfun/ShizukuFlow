/**
 * 音频分析：真实波形峰值 + BPM/拍点检测
 *
 * 时间轴上原来画的是「伪波形」（按 BPM 编出来的正弦），只能看个气氛，对不上真实的
 * 人声与鼓点。这里用 ffmpeg 把音轨解成单声道 PCM，压成固定桶率的 min/max 峰值，
 * 供时间轴直接绘制；同一份包络顺带做一次自相关，把 BPM 和首拍偏移一起算出来。
 *
 * 之所以放内核而不是 UI：`sf_dump` 也能跑，导出前可以在命令行确认「这首歌到底几 BPM」，
 * 不用开界面。
 */
#pragma once

#include <QString>
#include <QVector>

namespace sf {

/** 固定桶率的波形峰值。100 桶/秒：213 秒的歌约 2.1 万个桶，绘制时再按像素聚合。 */
struct AudioPeaks
{
    static constexpr int bucketsPerSecond = 100;

    double duration = 0.0;    // 秒
    QVector<float> low;       // 每桶最小值（-1..0 居多）
    QVector<float> high;      // 每桶最大值（0..1 居多）

    bool isEmpty() const { return low.isEmpty() || low.size() != high.size(); }
};

struct BeatInfo
{
    double bpm = 0.0;         // 0 = 没测出来
    double offset = 0.0;      // 首拍相对音频起点的秒数（0 <= offset < 拍长）
    double confidence = 0.0;  // 自相关主峰 / 均值，>2 算可信
};

/**
 * 解码音轨并压成峰值。
 * `ffmpegPath` 为空时按 `SHIZUKUFLOW_FFMPEG` 与 PATH 找。
 * 解码失败（文件缺失、ffmpeg 不在、格式不支持）返回空 AudioPeaks 并写 error。
 */
AudioPeaks computePeaks(const QString &audioPath, const QString &ffmpegPath = QString(),
                        QString *error = nullptr);

/** 从峰值包络估 BPM 与首拍。峰值太短（< 4 秒）会返回 bpm=0。 */
BeatInfo detectBeats(const AudioPeaks &peaks);

} // namespace sf
