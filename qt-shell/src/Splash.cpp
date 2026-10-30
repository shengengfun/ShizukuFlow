#include "Splash.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>

#include <algorithm>

namespace {
// ── 美术稿坐标系：640×400（与 SVG viewBox 一致）
constexpr double kArtW = 640.0;
constexpr double kArtH = 400.0;

// 进度条（美术稿实测几何）
constexpr double kBarX = 40.0;
constexpr double kBarY = 312.0;
constexpr double kBarW = 200.0;
constexpr double kBarH = 3.0;

// 配色（取自 SVG）
const QColor kTrackGray(0x2E, 0x2E, 0x35);
const QColor kFillBlue(0x5B, 0x8D, 0xEF);
const QColor kTitle(0xF5, 0xF5, 0xF7);
const QColor kSubtitle(0x8E, 0x8E, 0x93);
const QColor kMsg(0x7A, 0x7A, 0x82);
const QColor kCredit(0xB0, 0xB0, 0xB8);
const QColor kVersion(0x5A, 0x5A, 0x5F);

// 文案（artwork 署名与版本号按设计稿，可随时改这里）
const QString kProduct = QStringLiteral("LUME Motion");
const QString kTagline = QStringLiteral("文字动画 PV 制作");
const QString kArtwork = QStringLiteral("Artwork: Your Name");
const QString kVersionLine = QStringLiteral("v0.1.0 · AGPL-3.0 · Build 20261030");

// 基线绘制：美术稿给的是 baseline y，直接 drawText(QPointF) 才能逐像素对位
void text(QPainter &p, double x, double baseline, int px, QFont::Weight w, const QColor &c,
          const QString &s, double letterSpacing = 0.0)
{
    QFont f(QStringLiteral("Segoe UI"));
    f.setPixelSize(px);
    f.setWeight(w);
    if (letterSpacing != 0.0)
        f.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    p.setFont(f);
    p.setPen(c);
    p.drawText(QPointF(x, baseline), s);
}
} // namespace

Splash::Splash(QWidget *parent)
    : QWidget(parent, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(int(kArtW), int(kArtH));

    m_bg.load(QStringLiteral(":/splash/splash_bg.png"));
    if (!m_bg.isNull())
        m_bg.setDevicePixelRatio(m_bg.width() / kArtW); // 2x 图 → 逻辑 640×400

    m_anim = new QPropertyAnimation(this, "progress", this);
    m_anim->setDuration(320);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);

    // 居中于主屏
    if (const QScreen *scr = QGuiApplication::primaryScreen()) {
        const QRect avail = scr->availableGeometry();
        move(avail.center() - QRect(0, 0, width(), height()).center());
    }
}

void Splash::stage(double v, const QString &msg)
{
    m_msg = msg;
    m_anim->stop();
    m_anim->setStartValue(m_progress);
    m_anim->setEndValue(std::clamp(v, 0.0, 1.0));
    m_anim->start();
    update();
}

void Splash::setProgressValue(double v)
{
    m_progress = std::clamp(v, 0.0, 1.0);
    update();
}

void Splash::finish()
{
    stage(1.0, QStringLiteral("启动完成"));
    auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(360);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    fade->setEasingCurve(QEasingCurve::InQuad);
    connect(fade, &QPropertyAnimation::finished, this, &QWidget::close);
    QTimer::singleShot(420, this, [fade] { fade->start(QAbstractAnimation::DeleteWhenStopped); });
}

void Splash::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // ── 美术层（背景 / 立绘 / 图标），2x 位图按逻辑尺寸贴
    if (!m_bg.isNull())
        p.drawPixmap(0, 0, m_bg);

    // ── 文字层：矢量绘制，逐条对齐美术稿的 baseline
    text(p, 40, 178, 26, QFont::DemiBold, kTitle, kProduct, 0.2);
    text(p, 40, 204, 13, QFont::Normal, kSubtitle, kTagline);
    text(p, 40, 238, 11, QFont::Normal, kMsg, m_msg);
    text(p, 40, 356, 11, QFont::Medium, kCredit, kArtwork);
    text(p, 40, 378, 9, QFont::Normal, kVersion, kVersionLine);

    // ── 进度条
    p.setPen(Qt::NoPen);
    p.setBrush(kTrackGray);
    p.drawRoundedRect(QRectF(kBarX, kBarY, kBarW, kBarH), kBarH / 2, kBarH / 2);

    const double w = kBarW * m_progress;
    if (w >= 0.5) {
        p.setBrush(QColor(kFillBlue.red(), kFillBlue.green(), kFillBlue.blue(), 52));
        p.drawRoundedRect(QRectF(kBarX, kBarY - 2.5, w, kBarH + 5.0), 3, 3);
        p.setBrush(kFillBlue);
        p.drawRoundedRect(QRectF(kBarX, kBarY, w, kBarH), kBarH / 2, kBarH / 2);
    }
}
