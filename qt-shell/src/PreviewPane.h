#pragma once
#include <QWidget>

#include <array>
#include <functional>

#include "core/sf_project.h"
#include "core/sf_render.h"

/**
 * 预览舞台：内核渲染出口。
 *
 * 职责只有两件事：
 *  1. 按需调用 sf::FrameRenderer 得到工程画幅的一帧 QImage（同帧号去重）；
 *  2. letterbox 缩放贴到舞台 + 叠加取景 HUD。
 * 画面内容完全来自后端内核，UI 不做任何业务渲染。
 *
 * 顺带做引擎的计量出口：每次真正出帧都记一次耗时，状态栏据此显示
 * 「上次 / 平均 / p95」。没有这些数字，谈优化只能靠猜。
 */

/** 引擎运行时指标。快照语义：由 PreviewPane 在每次出帧后刷新。 */
struct EngineStats
{
    double lastMs = 0;
    double avgMs = 0;
    double p95Ms = 0;
    qint64 frames = 0;
    /** 0..1；-1 表示还没有足够样本。 */
    double glyphHitRate = -1;
    int glyphEntries = 0;
    qint64 bgBakes = 0;
    int bgEntries = 0;
};

class PreviewPane : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewPane(QWidget *parent = nullptr);

    void setEngine(const sf::Project *project, sf::FrameRenderer *renderer);
    void setTime(double t);
    /** 作废当前帧缓存。编辑改了工程内容但时间没变时，必须调它才看得到变化。 */
    void invalidateFrame();
    void setGuideMode(int mode);
    void setHoldFrames(int frames);

    // 启动预热：立即渲染当前帧一次（首次调用承担字体加载 + 字形烘焙耗时）
    void warmup();

    const EngineStats &engineStats() const { return m_stats; }
    /** 每真正出帧一次回调一次（HUD 重绘不算）。 */
    std::function<void()> onFrameRendered;
    /** 清空计量样本（换工程 / 换画幅时调用）。 */
    void resetStats();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRectF stageRect() const;
    void drawHud(QPainter &p, const QRectF &r) const;
    /** 记录一次出帧：写环形缓冲并重算 avg/p95 + 拉缓存命中率。 */
    void recordFrame(double ms);

    const sf::Project *m_project = nullptr;
    sf::FrameRenderer *m_renderer = nullptr;
    QImage m_frame;
    int m_lastFrame = -1;
    double m_t = 0;
    int m_guideMode = 0;    // 0 演出，1 安全框，2 三分构图，3 网格
    int m_holdFrames = 1;

    static constexpr int kSampleWindow = 180; // ≈30fps 下 6 秒，够看出抖动
    std::array<double, kSampleWindow> m_samples{};
    int m_sampleCount = 0;
    int m_sampleHead = 0;
    EngineStats m_stats;
};
