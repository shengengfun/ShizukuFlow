#!/usr/bin/env bash
# 构建 ShizukuFlow 的 Qt 原生骨架原型
# Qt 与 MinGW 装在 D:\Project\Qt，供本工作区所有项目共用
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
QT_ROOT="${QT_ROOT:-D:/Project/Qt/6.10.3/mingw_64}"
MINGW_BIN="${MINGW_BIN:-D:/Project/Qt/Tools/mingw1310_64/bin}"

export PATH="$MINGW_BIN:$PATH"

# Git Bash 下 cmake 无法自动定位 MinGW 工具链，显式给出 Windows 路径
cmake -S "$ROOT" -B "$ROOT/build" -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH="$QT_ROOT" \
  -DCMAKE_CXX_COMPILER="$MINGW_BIN/g++.exe" \
  -DCMAKE_C_COMPILER="$MINGW_BIN/gcc.exe" \
  -DCMAKE_MAKE_PROGRAM="$MINGW_BIN/mingw32-make.exe" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$ROOT/build" -j

echo "产物: $ROOT/build/shizuku_shell.exe"
