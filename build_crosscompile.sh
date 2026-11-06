#!/bin/sh
# ============================================================================
#  QuickShot - 在 macOS / Linux 上交叉编译 Windows 版
#
#  用途：本机没有 Windows 环境时，用 Homebrew 的 mingw-w64 直接产出可运行的
#        exe，和 build_msvc.bat / build_mingw.bat 编的是同一份源码。
#
#  前置：brew install mingw-w64        （Linux: apt-get install mingw-w64）
#
#  产物：build/QuickShot.exe      64 位
#        build/QuickShot_x86.exe  32 位
#
#  注意：编出来的是 Windows 程序，在本机只能编译、不能运行，
#        要实测请把 exe 拿到 Windows 机器上跑。
# ============================================================================
set -e

cd "$(dirname "$0")"
OUT=build
mkdir -p "$OUT"

# 允许通过环境变量覆盖（例如指向 Apple Silicon 上的具体前缀）
CXX64=${CXX64:-x86_64-w64-mingw32-g++}
CXX32=${CXX32:-i686-w64-mingw32-g++}
WRES64=${WRES64:-x86_64-w64-mingw32-windres}
WRES32=${WRES32:-i686-w64-mingw32-windres}

# 与 build_mingw.bat 保持一致的编译参数；额外开 -Wall -Wextra，
# 让 README 里「零警告」这条说法在 CI 上可验证
FLAGS="-O2 -municode -mwindows -DUNICODE -D_UNICODE -Wall -Wextra"
LIBS="-lgdiplus -lshell32 -lshlwapi -lole32 -ladvapi32 -luser32 -lgdi32 -lcomdlg32"
STATIC="-static -static-libgcc -static-libstdc++"

echo "==> 64 位：资源脚本"
$WRES64 --codepage=65001 app.rc -o "$OUT/app_res_x64.o"

echo "==> 64 位：编译"
$CXX64 $FLAGS $STATIC QuickShot.cpp "$OUT/app_res_x64.o" -o "$OUT/QuickShot.exe" $LIBS

echo "==> 32 位：资源脚本"
$WRES32 --codepage=65001 app.rc -o "$OUT/app_res_x86.o"

echo "==> 32 位：编译"
$CXX32 $FLAGS $STATIC QuickShot.cpp "$OUT/app_res_x86.o" -o "$OUT/QuickShot_x86.exe" $LIBS

echo
echo "==> 完成"
ls -lh "$OUT"/*.exe
file "$OUT/QuickShot.exe"
