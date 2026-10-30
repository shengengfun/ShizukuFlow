#pragma once
#include <QWidget>

#include <memory>

#include "core/sf_edit.h"
#include "core/sf_project.h"

class TimeCanvas;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

// 时间轴：头部工具条 + 自绘轨道 + 底部状态条（总高 214，与原型 .timeline 一致）
class TimelinePane : public QWidget
{
    Q_OBJECT
public:
    explicit TimelinePane(QWidget *parent = nullptr);
    ~TimelinePane() override; // PeakJob 是不完整类型，析构必须在 .cpp 里生成

    void setProject(const sf::Project *project);
    void setTime(double t);
    double time() const;

    QPushButton *playButton() const { return m_playBtn; }
    QLabel *timeLabel() const { return m_tcLabel; }
    void setPlaying(bool on);

    /** 缩放到「全片可见」：整条时间轴刚好铺满画布宽度。 */
    void fitAll();
    /**
     * 删除当前时间轴选中：有部件选择先删部件（整组一条撤销），
     * 否则删选中的镜头。返回 false = 时间轴上没有可删的东西（交给外层兜底）。
     */
    bool deleteSelection();
    /** 换工程后重置视图：清掉旧选择，再适配全片（新建工程时路径没变，setProject 认不出来）。 */
    void reloadProjectView();
    /** 以播放头为锚缩放：factor > 1 放大（看得更细），< 1 缩小。 */
    void zoomBy(double factor);
    /** 当前每秒像素（给菜单显示/记忆用）。 */
    double pixelsPerSecond() const;

signals:
    void seeked(double t);
    void playToggled(bool on);
    void stepShot(int dir); // -1 上一镜 / +1 下一镜

    // 编辑请求：拖动块/裁剪/删除都只在这里「申请」，由外层 ProjectEditor 执行并回滚
    void editRequested(const sf::EditCmd &cmd);
    // 批量编辑请求：框选多个部件整组删除/换层，整批只压一条撤销
    void batchEditRequested(const QVector<sf::EditCmd> &cmds, const QString &label);
    // 选中变化：id 为空表示取消选中
    void selectionChanged(const QString &id, bool isShot);

private:
    /** 后台任务（定义在 .cpp）：线程只写这里，UI 线程定时来取。 */
    struct PeakJob;
    struct FilmJob;

    void kickPeaks(const QString &absAudioPath);
    void kickThumbs(const QString &projectPath);
    void pollJobs();   // 统一回收波形/胶片条两个任务
    void refreshBpmLabel();

    TimeCanvas *m_canvas = nullptr;
    QPushButton *m_playBtn = nullptr;
    QPushButton *m_waveBtn = nullptr;
    QLabel *m_tcLabel = nullptr;
    QLabel *m_bpmLabel = nullptr;
    QLabel *m_karaokeLabel = nullptr;
    QLabel *m_selLabel = nullptr; // 选区/手势提示（没选中时展示部件轨操作提示）
    QSlider *m_zoomSlider = nullptr;
    QLabel *m_zoomLabel = nullptr;

    QString m_projPath;                 // 当前挂载的工程路径（换工程才清选择/适配全片）
    QString m_audioAbs;                 // 已请求过分析的音轨绝对路径（换轨才重算）
    QString m_thumbsFor;                // 已出过缩略图的工程路径（换工程才重算）
    std::shared_ptr<PeakJob> m_peakJob; // 进行中的波形任务；nullptr = 没有
    std::shared_ptr<FilmJob> m_filmJob; // 进行中的缩略图任务
    QTimer *m_peakPoll = nullptr;       // 轮询任务完成（避免跨线程碰 QObject）
};
