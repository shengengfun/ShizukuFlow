"""把启动屏美术稿（SVG）烘焙成 Qt 用的 2x 背景 PNG。

为什么不是运行时直接渲染 SVG：
  Qt 的 QSvgRenderer 只覆盖 SVG Tiny 1.2 子集，本稿用了 feDropShadow /
  feGaussianBlur / 多层渐变混合，直读会掉效果；而 Chrome 渲染后按 2x 位图
  贴图，视觉效果与设计稿一致。

分工（关键）：
  · 位图只负责「美术」——背景、立绘、图标、渐变；
  · 所有文字（标题/副标题/状态/署名/版本号）与进度条都从 SVG 里剥掉，
    交给 Qt 矢量绘制（Splash.cpp）。这样任意 DPI 下文字都是锐利的，
    也顺带解决了 2x 位图缩到 125% DPI 时小字发虚的问题。

用法：
  python tools/bake_splash.py                       # 默认烘焙 shizuku 版
  python tools/bake_splash.py --svg <path.svg>
"""
import argparse
import base64
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QT_SHELL = os.path.dirname(HERE)
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"


def bake(svg_path, icon_path, out_png, scale=2.0, art=(640, 400)):
    src = open(svg_path, encoding="utf-8").read()

    # 1) 剥掉蓝色进度填充（灰色轨道保留作轨道底）
    #    注意属性顺序不定，逐 rect 检查两个特征而不是假设顺序
    src = re.sub(
        r"<rect\b[^>]*>",
        lambda m: "" if ('y="312"' in m.group(0) and '#5B8DEF' in m.group(0)) else m.group(0),
        src,
    )

    # 2) 剥掉全部文字：改由 Qt 绘制
    src = re.sub(r"\s*<text[^>]*>[^<]*</text>", "", src)

    # 3) logo 槽位从「L 字标」换成应用图标（裁进圆角矩形，边框保留）
    if icon_path and os.path.exists(icon_path):
        b64 = base64.b64encode(open(icon_path, "rb").read()).decode()
        lblock = re.compile(
            r'\s*<path d="M 72 74 L 72 110 L 98 110".*?<circle cx="104" cy="72"[^>]*/>',
            re.S,
        )
        repl = (
            '\n      <clipPath id="iconArtClip">'
            '<rect x="40" y="48" width="88" height="88" rx="18"/></clipPath>'
            f'\n      <image clip-path="url(#iconArtClip)" x="40" y="48" width="88" height="88"'
            f'\n             preserveAspectRatio="xMidYMid slice"'
            f'\n             xlink:href="data:image/png;base64,{b64}"/>'
            '\n      <rect x="40" y="48" width="88" height="88" rx="18" fill="none"'
            '\n            stroke="#4A4A56" stroke-width="2"/>'
        )
        src, n = lblock.subn(repl, src)
        if n == 0:
            print("warn: logo 字标块未匹配，图标未替换", file=sys.stderr)

    # 4) 根尺寸放大到 scale 倍（viewBox 不变）
    w, h = art
    src = src.replace(
        f'width="{w}" height="{h}" viewBox="0 0 {w} {h}"',
        f'width="{int(w * scale)}" height="{int(h * scale)}" viewBox="0 0 {w} {h}"',
        1,
    )

    tmp_svg = os.path.join(tempfile.gettempdir(), "_sf_splash_bg.svg")
    open(tmp_svg, "w", encoding="utf-8").write(src)

    profile = os.path.join(tempfile.gettempdir(), "chrome-splash-bake")
    cmd = [
        CHROME, "--headless=new", "--disable-gpu", "--no-first-run",
        f"--user-data-dir={profile}",
        "--default-background-color=00000000",  # 圆角外透明
        f"--window-size={int(w * scale)},{int(h * scale)}",
        f"--screenshot={out_png}",
        "file:///" + tmp_svg.replace("\\", "/"),
    ]

    # 坑：Windows 上 chrome.exe 启动器会立刻返回，真正的渲染进程在后台写文件。
    # 所以必须 Popen + 轮询产出文件（且必须比启动时间新），不能 subprocess.run 等退出码。
    started = time.time()
    if os.path.exists(out_png):
        os.remove(out_png)
    subprocess.Popen(cmd)
    for _ in range(90):
        time.sleep(1)
        if os.path.exists(out_png) and os.path.getsize(out_png) > 0 \
                and os.path.getmtime(out_png) >= started:
            time.sleep(0.5)  # 等写入收尾
            break
    else:
        raise SystemExit("烘焙失败：Chrome 90s 内未产出截图")

    from PIL import Image
    with Image.open(out_png) as im:
        if im.size != (int(w * scale), int(h * scale)):
            raise SystemExit(f"烘焙失败：尺寸异常 {im.size}")
    print("baked:", out_png, os.path.getsize(out_png), "bytes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--svg", default=r"D:\Project\splashtemple\splashshizuku.svg")
    ap.add_argument("--icon", default=os.path.join(os.path.dirname(QT_SHELL), "icon.png"))
    ap.add_argument("--out", default=os.path.join(QT_SHELL, "res", "splash_bg.png"))
    ap.add_argument("--scale", type=float, default=2.0)
    a = ap.parse_args()
    bake(a.svg, a.icon, a.out, a.scale)


if __name__ == "__main__":
    main()
