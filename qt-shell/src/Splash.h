#pragma once
#include <QWidget>

class QPropertyAnimation;

/**
 * Adobe 风启动屏。
 *
 * 美术稿由 splashtemple/splashlanzhu.svg 烘焙为 2x 背景图（res/splash_bg.png），
 * 动态部分——状态文本与进度条——不进图，由本类在 paintEvent 里实时绘制：
 *   · 状态文本  x=40 baseline=238  11px  #7A7A82
 *   · 进度轨道  x=40 y=312 w=200 h=3  rx=1.5  #2E2E35
 *   · 进度填充  同轨几何 · #5B8DEF（带一点微辉光）
 * 这样加载阶段可以汇报真实进度（解析工程 / 烘焙字形 / 渲染首帧）。
 */
class Splash : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(double progress READ progress WRITE setProgressValue FINAL)
public:
    explicit Splash(QWidget *parent = nullptr);

    double progress() const { return m_progress; }

    // 汇报一个阶段：进度平滑动画走到 v，状态文本换成 msg
    void stage(double v, const QString &msg);

    // 平滑走满最后一段，短暂停留后淡出关闭
    void finish();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void setProgressValue(double v);

    QPixmap m_bg;
    QString m_msg;
    double m_progress = 0.0;
    QPropertyAnimation *m_anim = nullptr;
};
