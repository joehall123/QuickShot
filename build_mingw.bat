@echo off
chcp 936 >nul
rem ===================================================================
rem  QuickShot - MinGW-w64 构建脚本
rem  前置：安装 mingw-w64（例如 MSYS2 的 mingw-w64-x86_64-gcc）
rem  输出：QuickShot.exe（静态链接运行库，单文件即可分发）
rem ===================================================================
setlocal

set CXX=g++
where %CXX% >nul 2>nul
if errorlevel 1 (
    echo [错误] 找不到 g++，请把 MinGW-w64 的 bin 目录加入 PATH。
    pause
    exit /b 1
)

rem 把清单与版本信息编译成资源
windres app.rc -o app_res.o
if errorlevel 1 (
    echo [失败] 资源编译未通过。
    pause
    exit /b 1
)

%CXX% -O2 -municode -mwindows -DUNICODE -D_UNICODE ^
      -static -static-libgcc -static-libstdc++ ^
      QuickShot.cpp app_res.o -o QuickShot.exe ^
      -lgdiplus -lshell32 -lshlwapi -lole32 -ladvapi32 -luser32 -lgdi32 -lcomdlg32

if errorlevel 1 (
    echo [失败] 编译未通过。
    pause
    exit /b 1
)

echo.
echo [完成] 已生成 QuickShot.exe
pause
