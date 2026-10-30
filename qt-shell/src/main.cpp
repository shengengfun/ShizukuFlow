#include "MainWindow.h"
#include "Splash.h"
#include "theme.h"

#include <QApplication>
#include <QEventLoop>
#include <QIcon>
#include <QPalette>
#include <QTimer>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

// Windows 11：深色标题 + 圆角 + 主题色描边 + 任务栏身份。
// 无边框窗口不继承这些，得逐项告诉 DWM，否则深色界面会被系统灰边和直角框住。
void applyImmersiveFrame(QWidget *w)
{
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(w->winId());
    HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        using FnDwmSet = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        auto fn = reinterpret_cast<FnDwmSet>(::GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (fn) {
            const BOOL dark = TRUE;
            // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE（Win10 1809 起为 19，20 兼容 20H1+）
            fn(hwnd, 20, &dark, sizeof(dark));
            const DWORD round = 2; // DWMWCP_ROUND
            fn(hwnd, 33, &round, sizeof(round));
            const COLORREF border = RGB(0x35, 0x35, 0x3f); // --line，与自绘 1px 描边同色
            fn(hwnd, 34, &border, sizeof(border));
        }
        ::FreeLibrary(dwm);
    }
    // 无边框窗口默认没有任务栏身份，显式补上，否则最小化后就找不回来了
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_APPWINDOW);
#else
    Q_UNUSED(w)
#endif
}

void applyTheme(QApplication &app)
{
    app.setStyle(QStringLiteral("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, Theme::c(Theme::Col::Bg));
    p.setColor(QPalette::WindowText, Theme::c(Theme::Col::Tx));
    p.setColor(QPalette::Base, Theme::c(Theme::Col::Bg));
    p.setColor(QPalette::AlternateBase, Theme::c(Theme::Col::Panel2));
    p.setColor(QPalette::ToolTipBase, Theme::c(Theme::Col::Panel3));
    p.setColor(QPalette::ToolTipText, Theme::c(Theme::Col::Tx));
    p.setColor(QPalette::Text, Theme::c(Theme::Col::Tx));
    p.setColor(QPalette::Button, Theme::c(Theme::Col::Panel3));
    p.setColor(QPalette::ButtonText, Theme::c(Theme::Col::Tx));
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, Theme::c(Theme::Col::Accent));
    p.setColor(QPalette::HighlightedText, Theme::c(Theme::Col::AccentInk));
    p.setColor(QPalette::Link, Theme::c(Theme::Col::Accent));
    p.setColor(QPalette::Disabled, QPalette::Text, Theme::c(Theme::Col::Tx3));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, Theme::c(Theme::Col::Tx3));
    p.setColor(QPalette::Disabled, QPalette::WindowText, Theme::c(Theme::Col::Tx3));
    app.setPalette(p);

    app.setFont(Theme::ui(Theme::F::Body));
    app.setStyleSheet(Theme::styleSheet());
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/splash/app_icon.png")));
    applyTheme(app);

    // ── Adobe 式启动屏：分阶段汇报真实加载进度 ──────────────────────────────
    Splash splash;
    splash.show();

    // 让进度动画播完、界面完成重绘（截图冒烟时可用 SF_SPLASH_HOLD 拉长停留）
    const auto settle = [&app](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };

    splash.stage(0.10, QStringLiteral("正在初始化界面…"));
    settle(320);

    MainWindow w;

    splash.stage(0.45, QStringLiteral("正在载入工程数据…"));
    settle(280);
    w.bootLoadProject();

    splash.stage(0.80, QStringLiteral("正在烘焙字形缓存…"));
    settle(120);
    w.warmupFirstFrame(); // 重活：字体加载 + 字形烘焙

    splash.stage(1.0, QStringLiteral("启动完成"));

    // 冒烟截图用：按住启动屏不放（毫秒），便于抓帧
    const int holdMs = qEnvironmentVariableIntValue("SF_SPLASH_HOLD");
    if (holdMs > 0)
        settle(holdMs);

    w.show();
    applyImmersiveFrame(&w); // 需要 HWND 已建出：圆角 / 描边 / 任务栏身份都作用在原生窗口上
    splash.finish();

    for (const QString &a : app.arguments()) {
        if (a == QStringLiteral("--autoplay"))
            w.autoplay();
    }

    return app.exec();
}
