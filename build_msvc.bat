@echo off
chcp 936 >nul
rem ===================================================================
rem  QuickShot - MSVC 构建脚本
rem  在 "x64 Native Tools Command Prompt for VS" 中运行，
rem  或直接双击（会尝试调用 vcvarsall.bat）。
rem  输出：QuickShot.exe（依赖系统自带 gdiplus.dll，Win7 及以上可用）
rem ===================================================================
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo 未找到 cl.exe，正在尝试初始化 Visual Studio 环境...
    for %%p in (
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
    ) do (
        if exist %%p call %%p x64 >nul 2>nul
    )
)

where cl >nul 2>nul
if errorlevel 1 (
    echo [错误] 找不到 cl.exe，请安装 Visual Studio 2017+ 附带的"C++ 桌面开发"组件。
    pause
    exit /b 1
)

echo 正在编译 QuickShot.exe ...
rem app.rc 会把 app.manifest 与版本信息打包进 exe（cl 会自动调用 rc.exe）
cl /nologo /W3 /O2 /MT /utf-8 /EHsc /DUNICODE /D_UNICODE QuickShot.cpp app.rc ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib gdiplus.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib

if errorlevel 1 (
    echo [失败] 编译未通过。
    pause
    exit /b 1
)

del /q *.obj >nul 2>nul
echo.
echo [完成] 已生成 QuickShot.exe
pause
