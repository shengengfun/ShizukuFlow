/**
 * ShizukuFlow 帧编译器（后端）
 *
 * 对应 app/src/lib/renderFrame.ts：把「工程 + 时间 t」编译成 LUME Draw 指令流，
 * 再交给 lume::Renderer 画到 QImage。
 *
 * 业务语义（歌词、卡拉OK、镜头、转场、letterbox）全部留在这层 ——
 * 内核永远不懂「歌词」，只懂指令。
 *
 * 布局/字号/安全区/运镜的每一项数值都与 TS 版逐行对齐，便于像素级对拍。
 */
#pragma once

#include "core/lume.h"
#include "core/sf_project.h"

#include <QImage>

#include <memory>
#include <vector>

namespace sf {

class FrameRenderer
{
public:
    FrameRenderer();
    ~FrameRenderer();

    FrameRenderer(const FrameRenderer &) = delete;
    FrameRenderer &operator=(const FrameRenderer &) = delete;

    void setProject(const Project *p);

    /** 渲染 t 时刻一帧。outW/outH 为 0 时用工程画幅。
        compileMs 非空时回填本次编译（含背景烘焙）耗时，供基准/状态栏拆账。 */
    QImage render(double t, int outW = 0, int outH = 0, double *compileMs = nullptr);

    /** 渲染到调用方持有的缓冲：尺寸/格式一致时原地复用，省掉每帧几 MB 的分配与清零。
        预览这类「永远同一块画布」的场景用它；一次性出图仍用 render()。 */
    void renderInto(QImage &dst, double t, int outW = 0, int outH = 0,
                    double *compileMs = nullptr);

    /** 只编译指令（不绘制）—— 用于调试/对拍。 */
    void compile(double t, std::vector<lume::Draw> &out, double w, double h);

    lume::Renderer::Stats cacheStats() const;

    struct BgStats
    {
        int entries = 0;
        qint64 renders = 0;
    };
    BgStats bgStats() const;

    void resetCaches();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace sf
