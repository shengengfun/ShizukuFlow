#include "PreviewPane.h"

#include "theme.h"

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMargin = 14.0; // 舞台四周留白
}

PreviewPane::PreviewPane(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PreviewPane::setEngine(const sf::Project *project, sf::FrameRenderer *renderer)
{
    m_project = project;
    m_renderer = renderer;
    m_lastFrame = -1;
    resetStats();
    update();
}

void PreviewPane::resetStats()
{
    m_samples.fill(0.0);
    m_sampleCount = 0;
    m_sampleHead = 0;
    m_stats = EngineStats{};
}

void PreviewPane::recordFrame(double ms)
{
    m_samples[m_sampleHead] = ms;
    m_sampleHead = (m_sampleHead + 1) % kSampleWindow;
    if (m_sampleCount < kSampleWindow)
        ++m_sampleCount;

    ++m_stats.frames;
    m_stats.lastMs = ms;

    double sum = 0;
    std::array<double, kSampleWindow> sorted{};
    for (int i = 0; i < m_sampleCount; ++i) {
        sorted[size_t(i)] = m_samples[size_t(i)];
        sum += m_samples[size_t(i)];
    }
    m_stats.avgMs = m_sampleCount > 0 ? sum / m_sampleCount : 0.0;
    std::sort(sorted.begin(), sorted.begin() + m_sampleCount);
    const int idx = std::min(m_sampleCount - 1, int(m_sampleCount * 0.95));
    m_stats.p95Ms = m_sampleCount > 0 ? sorted[size_t(idx)] : 0.0;

    if (m_renderer) {
        const auto s = m_renderer->cacheStats();
        const qint64 total = s.glyphHits + s.glyphMisses;
        m_stats.glyphHitRate = total > 0 ? double(s.glyphHits) / double(total) : -1.0;
        m_stats.glyphEntries = s.glyphEntries;
        const auto bg = m_renderer->bgStats();
        m_stats.bgBakes = bg.renders;
        m_stats.bgEntries = bg.entries;
    }
}

void PreviewPane::setTime(double t)
{
    m_t = t;
    update();
}

void PreviewPane::invalidateFrame()
{
    // paintEvent 按 m_lastFrame 去重；编辑改了工程内容但 t 没变时，
    // 不清这个缓存就永远看不到改动（"改了参数画面没反应"的典型来源）。
    m_lastFrame = -1;
    m_frame = QImage();
    update();
}

void PreviewPane::setGuideMode(int mode)
{
    m_guideMode = std::clamp(mode, 0, 3);
    update();
}

void PreviewPane::setHoldFrames(int frames)
{
    m_holdFrames = std::max(1, frames);
    m_lastFrame = -1;
    update();
}

void PreviewPane::warmup()
{
    if (m_project && m_renderer) {
        const int fps = std::max(1, m_project->canvas.fps);
        const double renderTime =
            m_holdFrames == 1
                ? m_t
                : std::floor(m_t * fps / m_holdFrames) * m_holdFrames / double(fps);
        m_renderer->renderInto(m_frame, renderTime);
        m_lastFrame = int(renderTime * fps);
        update();
    }
}

QRectF PreviewPane::stageRect() const
{
    const double aspect = m_project ? double(m_project->canvas.width) / m_project->canvas.height
                                    : 16.0 / 9.0;
    const double availW = std::max(16.0, width() - 2 * kMargin);
    const double availH = std::max(9.0, height() - 2 * kMargin);
    double w = availW;
    double h = w / aspect;
    if (h > availH) {
        h = availH;
        w = h * aspect;
    }
    return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
}

void PreviewPane::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF r = stageRect();

    // ── 内核出帧（同帧号去重：seek 与播放共享）
    bool haveFrame = false;
    if (m_project && m_renderer) {
        const int fps = std::max(1, m_project->canvas.fps);
        const double renderTime =
            m_holdFrames == 1
                ? m_t
                : std::floor(m_t * fps / m_holdFrames) * m_holdFrames / double(fps);
        const int fi = int(renderTime * fps);
        if (fi != m_lastFrame) {
            QElapsedTimer clock;
            clock.start();
            m_renderer->renderInto(m_frame, renderTime);
            recordFrame(clock.nsecsElapsed() / 1e6);
            m_lastFrame = fi;
            if (onFrameRendered)
                onFrameRendered();
        }
        haveFrame = !m_frame.isNull();
    }

    if (haveFrame) {
        p.drawImage(r, m_frame);
    } else {
        // 无工程 / 渲染失败：给一个安静的占位
        p.fillRect(r, QColor("#0a0a0f"));
        p.setFont(Theme::ui(Theme::F::Small));
        p.setPen(Theme::c(Theme::Col::Tx3, 120));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("未载入工程"));
    }

    if (m_guideMode != 0) {
        p.save();
        p.setClipRect(r);
        p.setPen(QPen(Theme::c(Theme::Col::Accent, m_guideMode == 1 ? 125 : 72), 1,
                      m_guideMode == 1 ? Qt::DashLine : Qt::SolidLine));
        if (m_guideMode == 1) {
            p.drawRect(r.adjusted(r.width() * 0.05, r.height() * 0.05,
                                  -r.width() * 0.05, -r.height() * 0.05));
            p.setPen(QPen(Theme::c(Theme::Col::Accent, 72), 1));
            p.drawRect(r.adjusted(r.width() * 0.10, r.height() * 0.10,
                                  -r.width() * 0.10, -r.height() * 0.10));
        } else if (m_guideMode == 2) {
            p.drawLine(QPointF(r.left() + r.width() / 3.0, r.top()),
                       QPointF(r.left() + r.width() / 3.0, r.bottom()));
            p.drawLine(QPointF(r.left() + r.width() * 2.0 / 3.0, r.top()),
                       QPointF(r.left() + r.width() * 2.0 / 3.0, r.bottom()));
            p.drawLine(QPointF(r.left(), r.top() + r.height() / 3.0),
                       QPointF(r.right(), r.top() + r.height() / 3.0));
            p.drawLine(QPointF(r.left(), r.top() + r.height() * 2.0 / 3.0),
                       QPointF(r.right(), r.top() + r.height() * 2.0 / 3.0));
        } else {
            p.setPen(QPen(Theme::c(Theme::Col::Accent, 48), 1));
            for (int i = 1; i < 12; ++i)
                p.drawLine(QPointF(r.left() + r.width() * i / 12.0, r.top()),
                           QPointF(r.left() + r.width() * i / 12.0, r.bottom()));
            for (int i = 1; i < 9; ++i)
                p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 9.0),
                           QPointF(r.right(), r.top() + r.height() * i / 9.0));
        }
        p.restore();
    }

    drawHud(p, r);
}

void PreviewPane::drawHud(QPainter &p, const QRectF &r) const
{
    // 四角 L 形定位标记（专业取景器的味道，不抢内容）
    p.setPen(QPen(Theme::c(Theme::Col::Tx3, 90), 1));
    const double len = std::min(12.0, r.width() * 0.02);
    const double pad = std::min(16.0, r.width() * 0.025);
    const QRectF inner = r.adjusted(pad, pad, -pad, -pad);
    p.drawLine(inner.topLeft(), inner.topLeft() + QPointF(len, 0));
    p.drawLine(inner.topLeft(), inner.topLeft() + QPointF(0, len));
    p.drawLine(inner.topRight(), inner.topRight() - QPointF(len, 0));
    p.drawLine(inner.topRight(), inner.topRight() + QPointF(0, len));
    p.drawLine(inner.bottomLeft(), inner.bottomLeft() + QPointF(len, 0));
    p.drawLine(inner.bottomLeft(), inner.bottomLeft() - QPointF(0, len));
    p.drawLine(inner.bottomRight(), inner.bottomRight() - QPointF(len, 0));
    p.drawLine(inner.bottomRight(), inner.bottomRight() - QPointF(0, len));

    QFont mf = Theme::mono(9);
    if (r.height() > 240)
        mf.setPixelSize(10);
    p.setFont(mf);

    const int shot = m_project ? m_project->shotIndexAt(m_t) : -1;
    if (shot >= 0) {
        p.setPen(Theme::c(Theme::Col::Accent, 200));
        p.drawText(QRectF(inner.left(), inner.top() + 4, 90, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::asprintf("S%03d", shot + 1));
    }

    if (m_project) {
        p.setPen(Theme::c(Theme::Col::Tx3, 180));
        const QString fmt = QString::asprintf("%dx%d · %dfps", m_project->canvas.width,
                                              m_project->canvas.height, m_project->canvas.fps);
        p.drawText(QRectF(inner.right() - 160, inner.top() + 4, 160, 14),
                   Qt::AlignRight | Qt::AlignVCenter, fmt);

        p.setPen(Theme::c(Theme::Col::Tx3, 160));
        const QString meta = m_project->artist.isEmpty()
                                 ? m_project->title
                                 : m_project->title + QStringLiteral(" — ") + m_project->artist;
        p.drawText(QRectF(inner.left(), inner.bottom() - 20, 320, 16),
                   Qt::AlignLeft | Qt::AlignVCenter, meta);
    }
}
