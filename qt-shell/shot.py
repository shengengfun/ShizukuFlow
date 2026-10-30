"""启动 Qt 骨架原型并截图（开发期冒烟用）。

坑位备忘：
- 按 PID 枚举窗口，不能只按标题 FindWindow（会命中同名资源管理器窗口）
- 必须在 python 进程存活时调用 ffmpeg，否则子进程被回收后窗口消失
- WebView2 窗口 PrintWindow 会崩，改用 ffmpeg gdigrab 抓桌面再按矩形裁剪
"""
import os
import subprocess
import sys
import time
import ctypes
from ctypes import wintypes

EXE = r"D:\Project\ShizukuFlow\qt-shell\build\shizuku_shell.exe"
QT_ROOT = r"D:\Project\Qt\6.10.3\mingw_64"
MINGW = r"D:\Project\Qt\Tools\mingw1310_64\bin"
FFMPEG = r"D:\Project\LanzhuToolNX\tools\video\ffmpeg\ffmpeg.exe"
OUT = r"D:\Project\ShizukuFlow\docs\qt-shell.png"

user32 = ctypes.WinDLL("user32", use_last_error=True)
try:
    user32.SetProcessDPIAware()
except Exception:
    pass

gdi32 = ctypes.WinDLL("gdi32")
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def capture_window(hwnd, path):
    """PrintWindow 直接抓窗口内容，不依赖前台状态、不干扰用户桌面。"""
    from PIL import Image

    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top

    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    ok = user32.PrintWindow(hwnd, mem, 2)  # 2 = PW_RENDERFULLCONTENT

    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                    ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                    ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                    ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                    ("biClrImportant", wintypes.DWORD)]

    bmi = BMIH()
    bmi.biSize = ctypes.sizeof(BMIH)
    bmi.biWidth = w
    bmi.biHeight = -h  # top-down
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0  # BI_RGB
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
    img.convert("RGB").save(path)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    return ok, w, h


def find_window(pid, timeout=40.0):
    boxes = []

    def cb(hwnd, _lparam):
        pid_out = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid_out))
        if pid_out.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length:
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, length + 1)
            title = buf.value
        else:
            title = ""
        rect = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        if w > 600 and h > 400 and "ShizukuFlow" in title:
            boxes.append((hwnd, rect, title))
            return False
        return True

    end = time.time() + timeout
    while time.time() < end:
        boxes.clear()
        user32.EnumWindows(EnumWindowsProc(cb), 0)
        if boxes:
            return boxes[0]
        time.sleep(0.4)
    return None


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    for stale in ("shizuku_shell.exe",):
        subprocess.run(["taskkill", "/F", "/IM", stale], capture_output=True)
        time.sleep(0.5)

    env = dict(os.environ)
    env["PATH"] = f"{QT_ROOT}\\bin;{MINGW};" + env.get("PATH", "")
    env["QT_PLUGIN_PATH"] = f"{QT_ROOT}\\plugins"
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = f"{QT_ROOT}\\plugins\\platforms"
    for k in ("HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy"):
        env.pop(k, None)

    proc = subprocess.Popen([EXE, "--autoplay"], cwd=os.path.dirname(EXE), env=env)
    print("PID", proc.pid)
    sys.stdout.flush()

    hit = find_window(proc.pid)
    if not hit:
        print("WINDOW_NOT_FOUND")
        return 1
    hwnd, rect, title = hit
    print("TITLE", title)
    print("RECT", rect.left, rect.top, rect.right, rect.bottom)
    sys.stdout.flush()

    user32.ShowWindow(hwnd, 9)
    time.sleep(0.8)

    # 启动参数 --autoplay 已让程序自动播放，等播放头走到卡拉OK高亮段
    time.sleep(5.5)

    w, h = rect.right - rect.left, rect.bottom - rect.top
    ok, w2, h2 = capture_window(hwnd, OUT)
    if ok and os.path.exists(OUT):
        print("SAVED", OUT, os.path.getsize(OUT), f"{w2}x{h2}")
    else:
        print("PRINTWINDOW_FAIL ok=", ok)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
