"""抓启动屏截图（开发期冒烟用，PrintWindow 不干扰桌面）。

SF_SPLASH_HOLD 让启动屏按住不放，脚本在两个时间点各抓一张：
  A ≈ 启动后 2.2s   —— 期望停在「正在烘焙字形缓存…」中段进度
  B ≈ 再等 6s       —— 期望「启动完成」满进度
"""
import os
import subprocess
import sys
import time
import ctypes
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shot  # 复用 capture_window / 环境准备

OUT_A = r"D:\Project\ShizukuFlow\docs\splash-mid.png"
OUT_B = r"D:\Project\ShizukuFlow\docs\splash-final.png"


def find_splash(pid, timeout=25.0):
    """启动屏：无边框无标题、约 640×400 的可见顶层窗口。"""
    end = time.time() + timeout
    while time.time() < end:
        hits = []

        def cb(hwnd, _lp):
            p = wintypes.DWORD()
            shot.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value != pid or not shot.user32.IsWindowVisible(hwnd):
                return True
            r = wintypes.RECT()
            shot.user32.GetWindowRect(hwnd, ctypes.byref(r))
            w, h = r.right - r.left, r.bottom - r.top
            if 560 <= w <= 900 and 360 <= h <= 580:  # 125% DPI 下为 800×500
                hits.append((hwnd, r))
                return False
            return True

        shot.user32.EnumWindows(shot.EnumWindowsProc(cb), 0)
        if hits:
            return hits[0]
        time.sleep(0.25)
    return None


def main():
    for stale in ("shizuku_shell.exe",):
        subprocess.run(["taskkill", "/F", "/IM", stale], capture_output=True)
        time.sleep(0.5)

    env = dict(os.environ)
    env["PATH"] = f"{shot.QT_ROOT}\\bin;{shot.MINGW};" + env.get("PATH", "")
    env["QT_PLUGIN_PATH"] = f"{shot.QT_ROOT}\\plugins"
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = f"{shot.QT_ROOT}\\plugins\\platforms"
    env["SF_SPLASH_HOLD"] = "15000"  # 按住启动屏，留足抓帧时间
    for k in ("HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"):
        env.pop(k, None)

    proc = subprocess.Popen([shot.EXE], cwd=os.path.dirname(shot.EXE), env=env)
    print("PID", proc.pid)
    sys.stdout.flush()

    hit = find_splash(proc.pid)
    if not hit:
        print("SPLASH_NOT_FOUND")
        return 1
    hwnd, rect = hit
    print("SPLASH_RECT", rect.left, rect.top, rect.right, rect.bottom)

    time.sleep(1.45)
    ok, _, _ = shot.capture_window(hwnd, OUT_A)
    print("A", os.path.exists(OUT_A), ok)

    time.sleep(6.0)
    if not shot.user32.IsWindowVisible(hwnd):
        print("SPLASH_CLOSED_EARLY")
        return 1
    ok, _, _ = shot.capture_window(hwnd, OUT_B)
    print("B", os.path.exists(OUT_B), ok)

    subprocess.run(["taskkill", "/F", "/IM", "shizuku_shell.exe"], capture_output=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
