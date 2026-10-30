#pragma once
#include <QMainWindow>
#include <QPoint>
#include <QRect>

#include <functional>

#include "core/sf_project.h"
#include "core/sf_render.h"

namespace sf {
struct EditCmd;
}

class PreviewPane;
class TimelinePane;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTimer;
class QTreeWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // 冒烟截图用：启动即自动播放，无需模拟鼠标点击
    void autoplay();

    // 启动阶段接口（配合 Splash 分步汇报真实进度）：
    bool bootLoadProject();   // 载入默认工程并刷新 UI
    void warmupFirstFrame();  // 预热首帧（字体加载 + 字形烘焙的重活在这里发生）

    // 无边框自绘标题栏：只有空白区拖动窗口，按钮与菜单照常接收点击
    void beginWindowDrag(const QPoint &globalPos);
    void dragWindow(const QPoint &globalPos);
    void endWindowDrag();
    void toggleMaximizedWindow();

protected:
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif
    void changeEvent(QEvent *event) override;
    // 把图片/视频文件拖进窗口 = 插进当前镜头（按扩展名分流 image / portrait / video）
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void tick();
    void togglePlay(bool on);
    void onSeek(double t);

private:
    QWidget *buildMenuBar();
    QWidget *buildToolbar();
    QWidget *buildWorkspace();
    QWidget *buildLibrary();
    QWidget *buildCenter();
    QWidget *buildPreviewWrap();
    QWidget *buildInspector();
    QWidget *buildStatusBar();
    QTreeWidget *buildPartTree();
    void refreshTimeCode();
    void updateShotInfo();
    void syncWindowButtons();
    void setMaximizedWindow(bool on);
    void updateEngineStats();

    /** 检查器：按当前镜头重建/刷新控件，并把控件改动真的写回工程。 */
    void refreshInspector(bool force = false);
    /** 统一的编辑入口：先压快照，再改工程，最后重渲 + 刷检查器。
        用快照而不是逆向命令：参数编辑是高频小改动，工程本身只有几百 KB，
        快照式撤销在这里更简单也更不容易漏字段。上限 kUndoLimit 步。 */
    void patchProject(const QString &label, const std::function<void(sf::Project &)> &fn);

    // ── 拖动合并：一次拖动 = 一条撤销项（滑块每走一步都压栈会把历史冲爆）──
    void beginParamDrag(const QString &label);
    void endParamDrag();
    void patchProjectDrag(const QString &label, const std::function<void(sf::Project &)> &fn);

    /** 改当前立绘的参数。x/y/h/alpha/glow/flip 只动本镜头；at/outAt 是组级的 ——
        同一个人跨 N 个镜头共用一组进场/退场时间，改一处要同步整组。 */
    void applyPortraitParam(const QString &key, double value);
    /** 删掉当前镜头里某类背景部件（portrait / image / video），位置随快照一起可撤销。 */
    void removeBackgroundPart(const QString &part);

    /** 拖动合并的「单步也自成一条」写法：滑块按下到松开 = 一条撤销项。 */
    void applyDragEdit(const QString &label, const std::function<void(sf::Project &)> &fn);
    /** 视频专属参数：in（源内起点）/ rate（倍速）/ loop（循环）。 */
    void applyVideoParam(const QString &key, double value);
    /** 给「旋转目标」选中的那个部件写 rotate（度）。 */
    void applyLayerRotate(double deg);
    /** 给「旋转目标」选中的那个部件写 tint + tintAmount（图片/立绘染色）。 */
    void applyLayerTint(const QString &hex, double amount);
    /** 按 sf::partPresets() 往对应层插一个部件（部件库双击）。 */
    void insertPartPreset(const QString &label);

    /** 歌词 .lrc 双向：导入替换 lyrics（走撤销命令），导出当前工程歌词。 */
    void importLyricsLrc();
    void exportLyricsLrc();

    /** 分析音轨并写回 bpm/offset（无对话框，selftest 也走这条）；失败写 error。 */
    bool applyDetectedBpm(QString *error = nullptr);
    /** 菜单/工具栏入口：等光标、失败提示、状态栏显示置信度。 */
    void detectBpm();

    /** 运镜关键帧（camera.params.keys）：改当前选中那个点的字段。 */
    void applyCameraKeyParam(const QString &field, double value);
    /** 在播放头处加一个关键帧（默认 zoom 1.0 / 中心 0.5,0.5）。 */
    void addCameraKeyAtPlayhead();
    /** 删掉当前选中的关键帧。 */
    void removeCurrentCameraKey();
    void undoEdit();
    void redoEdit();
    void updateUndoActions();
    /** 执行时间轴发来的编辑命令：ProjectEditor 负责合法性（clamp/不重叠），
        MainWindow 负责统一的快照撤销栈 —— 撤销入口只此一个。 */
    void applyEditCmd(const sf::EditCmd &cmd);
    /** 批量编辑：整批成功才进撤销栈（框选整组部件删除/换层走这里）。 */
    void applyEditBatch(const QVector<sf::EditCmd> &cmds, const QString &label);
    /** 一次编辑提交后的统一收尾：把「编辑前」快照入栈 + 渲染/时间轴/检查器全部刷新。 */
    void afterProjectEdit(const sf::Project &before);
    /** --selftest：无头验证「控件 → 工程 → 渲染」链路。退出码 0=通过。 */
    void runSelfTest();

    // ── 菜单动作 ──
    void newProject();
    void saveProject();
    void saveProjectAs();
    void deleteSelectedShot();
    void rerollShot();
    void exportCurrentFrame();
    /** 导出视频：把当前工程存临时 .sf，再调 sf_dump 的内核导出管线。 */
    void exportVideo();
    void openProjectDir();
    void showShortcuts();
    /** 把一张图片作为底图插到当前镜头（放在 background 数组末尾，压在文字之下）。 */
    void insertImagePlate(const QString &absPath);
    /** 当前编辑目标：时间轴选中优先，否则播放头所在镜头；无则 -1。 */
    int currentShotIndex() const;

    bool loadProject(const QString &path);
    bool loadDefaultProject();

    sf::Project m_project;
    sf::FrameRenderer m_renderer;
    QVector<double> m_shotStarts;

    PreviewPane *m_preview = nullptr;
    TimelinePane *m_timeline = nullptr;
    QTimer *m_timer = nullptr;
    QPushButton *m_playBtn = nullptr;
    QLabel *m_tcLabel = nullptr;
    QLabel *m_frameLabel = nullptr;
    QLabel *m_shotInfo = nullptr;
    QLabel *m_kernelLabel = nullptr;
    QLabel *m_resLabel = nullptr;
    QLabel *m_fileChip = nullptr;
    QLabel *m_fmtChip = nullptr;
    QLabel *m_engineLabel = nullptr;
    QLabel *m_bpmLabel = nullptr;   // 状态栏：BPM 与检测置信度
    QPushButton *m_minBtn = nullptr;
    QPushButton *m_maxBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    bool m_playing = false;
    double m_t = 0.0;

    // 自绘标题栏拖动状态
    bool m_dragging = false;
    bool m_maximized = false;
    QPoint m_dragGlobal;
    QPoint m_dragWindowPos;
    QRect m_normalGeometry;

    // ── 检查器（参数真接线）──
    QString m_insShotId;             // 检查器当前绑定的镜头
    QString m_selShotId;             // 时间轴选中的镜头（优先于播放头所在镜头）
    bool m_insBusy = false;          // 刷新控件时抑制信号回写
    QLabel *m_insHead = nullptr;
    QLabel *m_insRange = nullptr;
    QLabel *m_insMeta = nullptr;
    QComboBox *m_insBgKind = nullptr;
    QComboBox *m_insTransIn = nullptr;
    QSlider *m_insZoomA = nullptr;
    QSlider *m_insZoomB = nullptr;
    QSlider *m_insPunch = nullptr;
    QLineEdit *m_insLyric = nullptr;
    QComboBox *m_insFromInt = nullptr; // 入场强度？占位：入场样式
    QLineEdit *m_insSeed = nullptr;

    // ── 立绘 / 场景图（background 混合数组里的成员，顺序即绘制顺序）──
    QLabel *m_insPtName = nullptr;
    QLabel *m_insPlateName = nullptr;
    QSlider *m_insPtX = nullptr;
    QSlider *m_insPtY = nullptr;
    QSlider *m_insPtH = nullptr;
    QSlider *m_insPtAlpha = nullptr;
    QSlider *m_insPtGlow = nullptr;
    QComboBox *m_insPtFlip = nullptr;
    QSlider *m_insPtAt = nullptr;
    QSlider *m_insPtOut = nullptr;
    QPushButton *m_insPtDel = nullptr;
    QPushButton *m_insPlateDel = nullptr;
    QString m_insPortraitPath;   // 检查器当前绑定的立绘（按 path 认人）
    QString m_insPlatePath;      // 当前镜头的场景图 path

    // ── 视频 / 旋转 ──
    QLabel *m_insVideoName = nullptr;
    QSlider *m_insVidIn = nullptr;    // 源内起点（厘秒）
    QSlider *m_insVidRate = nullptr;  // 倍速（×100）
    QComboBox *m_insVidLoop = nullptr;
    QComboBox *m_insRotTarget = nullptr; // 旋转/染色目标：文字/图片/视频/立绘
    QSlider *m_insRotate = nullptr;      // -45..45 度
    QComboBox *m_insTintSel = nullptr;   // 染色色板
    QSlider *m_insTintAmt = nullptr;     // 染色量 0..1
    QString m_insVideoPath;

    // ── 运镜关键帧（camera.params.keys：{t, zoom, x, y}，绝对秒）──
    QLabel *m_insKeyHint = nullptr;
    QComboBox *m_insKeySel = nullptr;
    QSlider *m_insKeyT = nullptr;
    QSlider *m_insKeyZoom = nullptr;
    QSlider *m_insKeyX = nullptr;
    QSlider *m_insKeyY = nullptr;
    QPushButton *m_insKeyAdd = nullptr;
    QPushButton *m_insKeyDel = nullptr;
    int m_insKeyIndex = 0;

    bool m_dragActive = false;   // 正在拖动：改动只进工程，不进撤销栈
    bool m_dragDirty = false;
    sf::Project m_dragBefore;    // 拖动前的快照 = 松开时压的那一条

    // ── 撤销栈（快照式）──
    static constexpr int kUndoLimit = 40;
    QVector<sf::Project> m_undo;
    QVector<sf::Project> m_redo;
    QAction *m_undoAct = nullptr;
    QAction *m_redoAct = nullptr;
    QString m_projectPath;   // 当前工程文件路径（新建时为空）
    bool m_dirty = false;    // 有未保存改动
    QString m_ffmpegPath;    // 懒解析：BPM 检测要用（空 = 交给内核按 env/PATH 找）
    void markDirty();
};
