// ============================================================================
//  QuickShot - 轻量级截图工具（Win32 / GDI+，Windows 7 及以上）
//
//  功能：
//    1. 托盘常驻，自定义全局快捷键（默认 Ctrl+Alt+A）
//    2. 一键截取全屏（支持多显示器，含分层窗口）
//    3. 自动保存到 exe 同目录下的「验证截图」，可自建并命名子文件夹
//    4. 支持 PNG / JPEG / BMP，文件名前缀可自定义
//    5. 可选开机自启、截图后气泡提示
//    6. 区域 / 窗口截图：拖框选区域、单击选窗口、W 窗口吸附、F 整屏
//    7. 每次截图后托盘图标与悬浮球短暂变色，作为「已存盘」的视觉回执
//
//  构建：见 build_msvc.bat / build_mingw.bat
// ============================================================================

#define _WIN32_WINNT 0x0601
#define WINVER       0x0601
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>      // Shell_NotifyIcon / NOTIFYICONDATA（LEAN_AND_MEAN 下需显式包含）
#include <shlobj.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 常量与全局状态
// ---------------------------------------------------------------------------
static const wchar_t* APP_NAME    = L"QuickShot";
static const wchar_t* APP_VERSION = L"1.4";

// 默认截图子目录名：程序启动时建在 exe 同目录下，截图默认都落在这里
static const wchar_t* SHOT_FOLDER = L"验证截图";

#define WM_TRAY_MSG        (WM_APP + 10)
#define WM_KB_CHANGED      (WM_APP + 11)

#define ID_TRAY_ICON       1
#define ID_HOTKEY_CAPTURE  101
#define ID_HOTKEY_SELECT   102
#define IDT_FLASH          5      // 截图后图标变色的恢复定时器（挂在主隐藏窗口）
#define FLASH_ON_MS        500    // 变色持续多久
#define FLASH_HOLD_MAX_MS  4000   // 气泡不肯关闭时，最多再拖这么久就必须复位（兜底）

// 菜单命令
enum {
    IDM_CAPTURE = 1000,
    IDM_SETTINGS,
    IDM_OPEN_FOLDER,
    IDM_NEW_FOLDER,
    IDM_CHOOSE_BASE,
    IDM_TOGGLE_BALL,
    IDM_ABOUT,
    IDM_EXIT,
    IDM_SELECT,                // 区域 / 窗口截图（菜单入口）
    IDM_FOLDER_BASE = 1100,
    IDM_FOLDER_ITEM = 2000      // 动态子文件夹项起点
};

// 窗口类名
#define CLS_MAIN     L"QS_MainWnd"
#define CLS_KEYBOX   L"QS_KeyBox"
#define CLS_NEWFOLDER L"QS_NewFolder"
#define CLS_SETTINGS L"QS_Settings"
#define CLS_SELECT   L"QS_SelectWnd"
#define CLS_PREVIEW  L"QS_PreviewWnd"

struct Config {
    UINT         mod   = MOD_CONTROL | MOD_ALT;   // 全屏截图热键的修饰键
    UINT         vk    = 'A';                     // 全屏截图热键的主键
    UINT         mod2  = MOD_CONTROL | MOD_ALT;   // 区域截图热键的修饰键
    UINT         vk2   = 'S';                     // 区域截图热键的主键
    std::wstring baseDir;                         // 根目录
    std::wstring subDir;                          // 当前子文件夹（空=根目录）
    int          format = 0;                      // 0=PNG 1=JPEG 2=BMP
    std::wstring prefix = L"Screenshot";          // 文件名前缀
    bool         balloon = true;                  // 截图后气泡提示
    bool         autostart = false;               // 开机自启
    bool         ball = true;                     // 显示桌面悬浮球
    int          ballX = -1;                      // 悬浮球位置（-1 = 未设置）
    int          ballY = -1;
};

static HINSTANCE g_hInst      = nullptr;
static HWND      g_hwndMain   = nullptr;
static HICON     g_hiconBig   = nullptr;
static HICON     g_hiconSmall = nullptr;
static HICON     g_hiconBigFlash   = nullptr;   // 截图反馈用的变色图标
static HICON     g_hiconSmallFlash = nullptr;
static bool      g_flashing   = false;          // true = 正处于变色回执期间
static bool      g_balloonUp  = false;          // 气泡仍在显示（此时改图标会被 Shell 关掉气泡）
static ULONGLONG g_flashStart = 0;              // 本次变色的起始时刻，用于兜底复位
static UINT      g_msgTaskbarCreated = 0;
static HFONT     g_font       = nullptr;
static HFONT     g_fontBold   = nullptr;
static int       g_dpi        = 96;
static Config    g_cfg;
static Config    g_tmp;                 // 设置窗口的临时副本
static bool      g_modal      = false;
static bool      g_kbRecording = false;
static std::wstring g_lastSaved;
static std::vector<std::wstring> g_menuFolders;
static NOTIFYICONDATAW g_nid  = {};
static bool  g_portable = false;   // true = 同目录可写，按 U 盘便携模式运行

// 前向声明（悬浮球与托盘菜单互相引用）
static void DoCapture();
static void DoSelectCapture();
static void ShowAppMenu(POINT pt);
static void ShowSettings();
static void BallShow(bool show);
static void BallClampToScreen();
static void BallEnsureTopmost();
static void BallUpdate();
static void FlashCaptureFeedback();   // 截图成功后的图标变色（定义在托盘一节）

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static std::wstring ItoS(int v) { return std::to_wstring(v); }

static std::wstring Pad2(int v)
{
    std::wstring s = std::to_wstring(v);
    while (s.size() < 2) s = L"0" + s;
    return s;
}

static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p);
    size_t k = s.find_last_of(L'\\');
    return (k == std::wstring::npos) ? s : s.substr(0, k);
}

static bool DirWritable(const std::wstring& dir)
{
    std::wstring t = dir + L"\\.qs_wtest";
    HANDLE h = CreateFileW(t.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileW(t.c_str());
    return true;
}

static std::wstring ConfigPath()
{
    static std::wstring cached;
    if (!cached.empty()) return cached;

    std::wstring dir = ExeDir();
    // 程序目录可写 = 便携模式（U 盘/绿色版）：配置和数据全部留在本目录
    g_portable = DirWritable(dir);
    if (g_portable) {
        cached = dir + L"\\quickshot.ini";
    } else {
        wchar_t app[MAX_PATH] = {0};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, app))) {
            std::wstring d = std::wstring(app) + L"\\QuickShot";
            SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
            cached = d + L"\\quickshot.ini";
        } else {
            cached = dir + L"\\quickshot.ini";
        }
    }
    return cached;
}

static bool IsPortable()
{
    ConfigPath();          // 确保已完成一次性探测
    return g_portable;
}

// 判断 path 是否位于 dir 之下（不区分大小写，且要求目录边界，避免 QuickShot2 误判）
static bool IsUnderDir(const std::wstring& path, const std::wstring& dir)
{
    if (dir.empty() || path.size() <= dir.size()) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    wchar_t next = path[dir.size()];
    return next == L'\\' || next == L'/';
}

// 保存配置时：U 盘内的路径改写为相对路径（".\Screenshots"），
// 这样盘符从 E: 变成 F: 也不会失效
static std::wstring PathToStored(const std::wstring& abs)
{
    std::wstring dir = ExeDir();
    if (IsUnderDir(abs, dir)) return L"." + abs.substr(dir.size());
    if (_wcsnicmp(abs.c_str(), dir.c_str(), dir.size()) == 0 && abs.size() == dir.size())
        return L".";
    return abs;
}

// 读取配置时：把相对路径还原到当前盘符下
static std::wstring StoredToPath(const std::wstring& stored)
{
    if (stored.empty()) return stored;
    if (stored == L".") return ExeDir();
    if (stored.size() >= 2 && stored[0] == L'.' &&
        (stored[1] == L'\\' || stored[1] == L'/')) {
        std::wstring dir = ExeDir();
        if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
            dir.pop_back();
        return dir + stored.substr(1);
    }
    return stored;
}

static std::wstring DefaultPicturesDir()
{
    // 默认直接存到 exe 同目录下的「验证截图」：现场核对时图就在软件旁边，
    // 不用去翻系统「图片」文件夹。便携模式下这条路径会被 PathToStored 改写成
    // 相对形式（".\验证截图"），所以换盘符、整个文件夹搬走都依然有效。
    if (IsPortable()) return ExeDir() + L"\\" + SHOT_FOLDER;

    // 程序目录不可写（例如装进 Program Files）时退回「我的图片」，
    // 否则截图会因为落不了盘而失败
    wchar_t p[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, 0, p)))
        return std::wstring(p) + L"\\" + SHOT_FOLDER;
    return ExeDir() + L"\\" + SHOT_FOLDER;
}

static bool EnsureDir(const std::wstring& dir)
{
    if (dir.empty()) return false;
    if (PathIsDirectoryW(dir.c_str())) return true;
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return (r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS ||
            r == ERROR_FILE_EXISTS || PathIsDirectoryW(dir.c_str()));
}

static std::wstring SaveDir()
{
    std::wstring d = g_cfg.baseDir;
    if (!g_cfg.subDir.empty()) d += L"\\" + g_cfg.subDir;
    return d;
}

// ---------------------------------------------------------------------------
// 配置文件
// ---------------------------------------------------------------------------
static void LoadConfig()
{
    std::wstring f = ConfigPath();
    wchar_t buf[1024] = {0};

    g_cfg.mod  = (UINT)GetPrivateProfileIntW(L"hotkey", L"mod", MOD_CONTROL | MOD_ALT, f.c_str());
    g_cfg.vk   = (UINT)GetPrivateProfileIntW(L"hotkey", L"vk", 'A', f.c_str());
    g_cfg.mod2 = (UINT)GetPrivateProfileIntW(L"hotkey", L"mod2", MOD_CONTROL | MOD_ALT, f.c_str());
    g_cfg.vk2  = (UINT)GetPrivateProfileIntW(L"hotkey", L"vk2", 'S', f.c_str());

    GetPrivateProfileStringW(L"save", L"baseDir", L"", buf, 1024, f.c_str());
    g_cfg.baseDir = StoredToPath(buf);
    if (g_cfg.baseDir.empty()) g_cfg.baseDir = DefaultPicturesDir();

    GetPrivateProfileStringW(L"save", L"subDir", L"", buf, 1024, f.c_str());
    g_cfg.subDir = buf;

    g_cfg.format = GetPrivateProfileIntW(L"save", L"format", 0, f.c_str());

    GetPrivateProfileStringW(L"save", L"prefix", L"Screenshot", buf, 1024, f.c_str());
    g_cfg.prefix = buf;
    if (g_cfg.prefix.empty()) g_cfg.prefix = L"Screenshot";

    g_cfg.balloon   = GetPrivateProfileIntW(L"ui", L"balloon", 1, f.c_str()) != 0;
    g_cfg.autostart = GetPrivateProfileIntW(L"ui", L"autostart", 0, f.c_str()) != 0;
    if (IsPortable()) g_cfg.autostart = false;   // U 盘盘符会变，不能做开机自启

    g_cfg.ball      = GetPrivateProfileIntW(L"ui", L"ball", 1, f.c_str()) != 0;
    g_cfg.ballX     = GetPrivateProfileIntW(L"ui", L"ballX", -1, f.c_str());
    g_cfg.ballY     = GetPrivateProfileIntW(L"ui", L"ballY", -1, f.c_str());
}

static void SaveConfig()
{
    std::wstring f = ConfigPath();
    WritePrivateProfileStringW(L"hotkey", L"mod", ItoS((int)g_cfg.mod).c_str(), f.c_str());
    WritePrivateProfileStringW(L"hotkey", L"vk",  ItoS((int)g_cfg.vk).c_str(),  f.c_str());
    WritePrivateProfileStringW(L"hotkey", L"mod2", ItoS((int)g_cfg.mod2).c_str(), f.c_str());
    WritePrivateProfileStringW(L"hotkey", L"vk2",  ItoS((int)g_cfg.vk2).c_str(),  f.c_str());
    std::wstring storedBase = PathToStored(g_cfg.baseDir);
    WritePrivateProfileStringW(L"save", L"baseDir", storedBase.c_str(), f.c_str());
    WritePrivateProfileStringW(L"save", L"subDir",  g_cfg.subDir.c_str(),  f.c_str());
    WritePrivateProfileStringW(L"save", L"format",  ItoS(g_cfg.format).c_str(), f.c_str());
    WritePrivateProfileStringW(L"save", L"prefix",  g_cfg.prefix.c_str(),  f.c_str());
    WritePrivateProfileStringW(L"ui", L"balloon",   g_cfg.balloon ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"ui", L"autostart", g_cfg.autostart ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"ui", L"ball", g_cfg.ball ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"ui", L"ballX", ItoS(g_cfg.ballX).c_str(), f.c_str());
    WritePrivateProfileStringW(L"ui", L"ballY", ItoS(g_cfg.ballY).c_str(), f.c_str());
}

static void ApplyAutostart(bool on)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;

    if (on) {
        wchar_t path[MAX_PATH * 2] = {0};
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, APP_NAME, 0, REG_SZ,
                       (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// 托盘图标（运行时绘制，避免依赖 .ico 资源）
// ---------------------------------------------------------------------------
struct Canvas {
    int n;
    std::vector<BYTE> px;   // BGRA

    explicit Canvas(int size) : n(size), px((size_t)size * size * 4, 0) {}

    void set(int x, int y, BYTE r, BYTE g, BYTE b, BYTE a)
    {
        if (x < 0 || y < 0 || x >= n || y >= n) return;
        size_t i = ((size_t)y * n + x) * 4;
        px[i + 0] = b; px[i + 1] = g; px[i + 2] = r; px[i + 3] = a;
    }

    // 圆角矩形（坐标与半径为浮点，按像素中心采样）
    void roundRect(double x0, double y0, double x1, double y1, double rad,
                   BYTE R, BYTE G, BYTE B)
    {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                double cx = x + 0.5, cy = y + 0.5;
                if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
                double dx = 0, dy = 0;
                if (cx < x0 + rad)      dx = (x0 + rad) - cx;
                else if (cx > x1 - rad) dx = cx - (x1 - rad);
                if (cy < y0 + rad)      dy = (y0 + rad) - cy;
                else if (cy > y1 - rad) dy = cy - (y1 - rad);
                if (dx * dx + dy * dy <= rad * rad)
                    set(x, y, R, G, B, 255);
            }
        }
    }

    void circle(double cx, double cy, double rad, BYTE R, BYTE G, BYTE B)
    {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                double dx = (x + 0.5) - cx, dy = (y + 0.5) - cy;
                if (dx * dx + dy * dy <= rad * rad)
                    set(x, y, R, G, B, 255);
            }
        }
    }
};

// 图标配色：平时蓝色，截图成功的那一瞬间换成绿色，作为「已存盘」的即时信号
struct IconPalette {
    BYTE bodyR,  bodyG,  bodyB;
    BYTE lensR,  lensG,  lensB;
    BYTE shineR, shineG, shineB;
};

static IconPalette PaletteFor(bool flash)
{
    if (flash) return IconPalette{ 16, 185, 129,   8,  92,  66, 170, 255, 214 };
    return        IconPalette{ 31, 111, 229,  16,  58, 120, 170, 210, 255 };
}

static HICON MakeAppIcon(int size, bool flash)
{
    const int SS = 4;                    // 超采样倍数，用于抗锯齿
    const int big = size * SS;
    const IconPalette P = PaletteFor(flash);

    Canvas c(big);

    // 机身：圆角矩形（蓝 / 变色时绿）
    c.roundRect(big * 0.06, big * 0.24, big * 0.94, big * 0.94, big * 0.14,
                P.bodyR, P.bodyG, P.bodyB);
    // 顶部取景凸起
    c.roundRect(big * 0.34, big * 0.13, big * 0.66, big * 0.30, big * 0.05,
                P.bodyR, P.bodyG, P.bodyB);
    // 镜头白圈
    c.circle(big * 0.50, big * 0.60, big * 0.245, 255, 255, 255);
    // 镜头深色
    c.circle(big * 0.50, big * 0.60, big * 0.165, P.lensR, P.lensG, P.lensB);
    // 高光
    c.circle(big * 0.44, big * 0.54, big * 0.052, P.shineR, P.shineG, P.shineB);
    // 右上闪光灯
    c.roundRect(big * 0.72, big * 0.32, big * 0.84, big * 0.42, big * 0.03,
                235, 245, 255);

    // 盒式滤波降采样
    std::vector<BYTE> out((size_t)size * size * 4, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            unsigned sumA = 0, sumR = 0, sumG = 0, sumB = 0;
            for (int sy = 0; sy < SS; ++sy) {
                for (int sx = 0; sx < SS; ++sx) {
                    int bx = x * SS + sx, by = y * SS + sy;
                    size_t i = ((size_t)by * big + bx) * 4;
                    unsigned a = c.px[i + 3];
                    sumA += a;
                    sumR += c.px[i + 2] * a;
                    sumG += c.px[i + 1] * a;
                    sumB += c.px[i + 0] * a;
                }
            }
            size_t o = ((size_t)y * size + x) * 4;
            BYTE a = (BYTE)(sumA / (SS * SS));
            if (sumA > 0) {
                out[o + 0] = (BYTE)(sumB / sumA);
                out[o + 1] = (BYTE)(sumG / sumA);
                out[o + 2] = (BYTE)(sumR / sumA);
            }
            out[o + 3] = a;
        }
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) return nullptr;

    memcpy(bits, out.data(), out.size());

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;

    HICON ic = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

// ---------------------------------------------------------------------------
// 快捷键文本
// ---------------------------------------------------------------------------
static bool IsModifierVk(UINT vk)
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
           vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
           vk == VK_LWIN    || vk == VK_RWIN;
}

static bool IsExtendedVk(UINT vk)
{
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
    case VK_UP:     case VK_DOWN:   case VK_DIVIDE: case VK_NUMLOCK:
        return true;
    default:
        return false;
    }
}

static std::wstring KeyName(UINT vk)
{
    switch (vk) {
    case VK_SNAPSHOT: return L"PrintScreen";
    case VK_SPACE:    return L"Space";
    case VK_TAB:      return L"Tab";
    case VK_RETURN:   return L"Enter";
    case VK_ESCAPE:   return L"Esc";
    case VK_BACK:     return L"Backspace";
    case VK_PAUSE:    return L"Pause";
    case VK_CAPITAL:  return L"CapsLock";
    case VK_ADD:      return L"Num+";
    case VK_SUBTRACT: return L"Num-";
    case VK_MULTIPLY: return L"Num*";
    case VK_DIVIDE:   return L"Num/";
    case VK_DECIMAL:  return L"Num.";
    default: break;
    }

    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if (IsExtendedVk(vk)) lp |= (1 << 24);

    wchar_t buf[64] = {0};
    if (GetKeyNameTextW(lp, buf, 64) > 0) return buf;

    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    return L"VK" + ItoS((int)vk);
}

static std::wstring HotkeyText(UINT mod, UINT vk)
{
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl+";
    if (mod & MOD_ALT)     s += L"Alt+";
    if (mod & MOD_SHIFT)   s += L"Shift+";
    if (mod & MOD_WIN)     s += L"Win+";
    s += KeyName(vk);
    return s;
}

// ---------------------------------------------------------------------------
// 截图与保存
// ---------------------------------------------------------------------------
static int GetEncoderClsid(const wchar_t* mime, CLSID* clsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return -1;

    std::vector<BYTE> buf(size);
    ImageCodecInfo* p = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(num, size, p) != Ok) return -1;

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(p[i].MimeType, mime) == 0) {
            *clsid = p[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

static std::wstring MakeFilePath()
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    const wchar_t* ext = (g_cfg.format == 1) ? L".jpg"
                       : (g_cfg.format == 2) ? L".bmp" : L".png";

    std::wstring stem = g_cfg.prefix + L"_" +
                        ItoS(st.wYear) + Pad2(st.wMonth) + Pad2(st.wDay) + L"_" +
                        Pad2(st.wHour) + Pad2(st.wMinute) + Pad2(st.wSecond);

    std::wstring dir = SaveDir();
    std::wstring path = dir + L"\\" + stem + ext;

    int n = 2;
    while (PathFileExistsW(path.c_str())) {
        path = dir + L"\\" + stem + L"_" + ItoS(n++) + ext;
    }
    return path;
}

// ---------------------------------------------------------------------------
// 抓屏与保存
//   一次 BitBlt 得到整块虚拟桌面的 DIB；整屏截图与区域截图共用同一份像素，
//   区域模式只是从中裁一个矩形，避免二次抓屏时画面已经变化。
// ---------------------------------------------------------------------------
struct ScreenImage {
    HBITMAP hbm  = nullptr;
    BYTE*   bits = nullptr;          // 32bpp，自上而下，行跨距 = w * 4
    int     w = 0, h = 0;            // 虚拟桌面宽高
    int     vx = 0, vy = 0;          // 虚拟桌面左上角（多显示器时可为负）

    void Free()
    {
        if (hbm) { DeleteObject(hbm); hbm = nullptr; }
        bits = nullptr;
        w = h = 0;
    }
};

static bool GrabScreen(ScreenImage* img, std::wstring* err)
{
    img->vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    img->vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    img->w  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    img->h  = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (img->w <= 0 || img->h <= 0) {
        if (err) *err = L"无法获取屏幕尺寸。";
        return false;
    }

    HDC hScreen = GetDC(nullptr);
    if (!hScreen) {
        if (err) *err = L"获取屏幕设备上下文失败。";
        return false;
    }
    HDC hMem = CreateCompatibleDC(hScreen);
    if (!hMem) {
        ReleaseDC(nullptr, hScreen);
        if (err) *err = L"创建内存 DC 失败。";
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = img->w;
    bmi.bmiHeader.biHeight      = -img->h;      // 负数 = 自上而下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits) {
        if (hbm) DeleteObject(hbm);
        DeleteDC(hMem);
        ReleaseDC(nullptr, hScreen);
        if (err) *err = L"创建位图失败（可能内存不足）。";
        return false;
    }

    HGDIOBJ old = SelectObject(hMem, hbm);
    // CAPTUREBLT 让分层窗口（如某些播放器、菜单）也能被截到
    BOOL ok = BitBlt(hMem, 0, 0, img->w, img->h,
                     hScreen, img->vx, img->vy, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SelectObject(hMem, old);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);

    if (!ok) {
        DeleteObject(hbm);
        if (err) *err = L"屏幕抓取失败。";
        return false;
    }

    img->hbm  = hbm;
    img->bits = (BYTE*)bits;
    return true;
}

static int ClampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// area 用 DIB 像素坐标（左上角为 0,0），越界部分自动裁到画面内
static bool SaveDibRegion(const ScreenImage& img, const RECT& area,
                          std::wstring* outPath, std::wstring* err)
{
    RECT r = area;
    r.left   = ClampInt(r.left,   0, img.w);
    r.top    = ClampInt(r.top,    0, img.h);
    r.right  = ClampInt(r.right,  0, img.w);
    r.bottom = ClampInt(r.bottom, 0, img.h);
    int rw = r.right - r.left;
    int rh = r.bottom - r.top;
    if (rw <= 0 || rh <= 0 || !img.bits) {
        if (err) *err = L"选区无效。";
        return false;
    }

    std::wstring dir = SaveDir();
    if (!EnsureDir(dir)) {
        if (err) *err = L"无法创建保存目录：\n" + dir;
        return false;
    }

    std::wstring path = MakeFilePath();
    const wchar_t* mime = (g_cfg.format == 1) ? L"image/jpeg"
                        : (g_cfg.format == 2) ? L"image/bmp" : L"image/png";
    CLSID clsid;
    bool saved = false;
    Status st = GenericError;

    if (GetEncoderClsid(mime, &clsid) >= 0) {
        // PixelFormat32bppRGB：忽略 alpha 通道，保证输出不透明
        Bitmap full(img.w, img.h, img.w * 4, PixelFormat32bppRGB, img.bits);

        // 整屏沿用原来的「直接编码 DIB」路径，不额外裁剪，避免回归；
        // 区域则画到一张独立位图再编码，编码器不必理解外层 DIB 的行跨距。
        bool whole = (r.left == 0 && r.top == 0 && rw == img.w && rh == img.h);
        Bitmap* owned  = nullptr;
        Bitmap* target = &full;
        if (!whole) {
            owned = new Bitmap(rw, rh, PixelFormat32bppRGB);
            Graphics* cg = Graphics::FromImage(owned);
            if (cg) {
                cg->DrawImage(&full,
                              RectF(0.0f, 0.0f, (REAL)rw, (REAL)rh),
                              (REAL)r.left, (REAL)r.top, (REAL)rw, (REAL)rh,
                              UnitPixel);
                delete cg;
            }
            target = owned;
        }

        if (g_cfg.format == 1) {
            ULONG quality = 92;
            EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid           = EncoderQuality;
            ep.Parameter[0].Type           = EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ep.Parameter[0].Value          = &quality;
            st = target->Save(path.c_str(), &clsid, &ep);
        } else {
            st = target->Save(path.c_str(), &clsid, nullptr);
        }
        delete owned;
        saved = (st == Ok);
    }

    if (!saved) {
        if (err) *err = L"图片编码失败（GDI+ 状态码 " + ItoS((int)st) + L"）。";
        return false;
    }

    if (outPath) *outPath = path;
    return true;
}

static bool CaptureAndSave(std::wstring* outPath, std::wstring* err)
{
    ScreenImage img;
    if (!GrabScreen(&img, err)) return false;
    RECT all = { 0, 0, img.w, img.h };
    bool ok = SaveDibRegion(img, all, outPath, err);
    img.Free();
    return ok;
}

// ---------------------------------------------------------------------------
// 桌面悬浮球
//   分层窗口 + 置顶，压在各种全屏/最大化应用之上；左键截图，右键菜单，可拖动
// ---------------------------------------------------------------------------
#define CLS_BALL       L"QS_BallWnd"
#define IDT_BALL_TOP   1

static HWND  g_hwndBall     = nullptr;
static int   g_ballSize     = 56;
static bool  g_ballHover    = false;
static bool  g_ballPressed  = false;
static bool  g_ballDrag     = false;
static bool  g_dragMoved    = false;
static bool  g_ballTracking = false;
static bool  g_menuOpen     = false;   // 菜单弹出期间禁止抢顶，否则球会盖住菜单
static POINT g_dragCur      = {0, 0};   // 按下时的光标位置
static POINT g_dragWnd      = {0, 0};   // 按下时的窗口左上角

static void AddRoundRectF(GraphicsPath& path, const RectF& r, float rad)
{
    float d2 = rad * 2.0f;
    path.AddArc(r.X, r.Y, d2, d2, 180, 90);
    path.AddArc(r.GetRight() - d2, r.Y, d2, d2, 270, 90);
    path.AddArc(r.GetRight() - d2, r.GetBottom() - d2, d2, d2, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d2, d2, d2, 90, 90);
    path.CloseFigure();
}

// 生成带透明通道的球形位图（GDI+ 抗锯齿绘制）
// flash = true 时整球改绿色，用于截图成功后的即时回执
static HBITMAP MakeBallBitmap(int size, bool hover, bool pressed, bool flash)
{
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(0, 0, 0, 0));

    float pad = size * 0.07f;
    float d   = (float)size - pad * 2.0f;
    RectF rc(pad, pad, d, d);
    float cx = pad + d * 0.5f;
    float cy = pad + d * 0.5f;

    // 外发光（模拟投影/光晕）
    for (int i = 3; i >= 1; --i) {
        float grow = (float)i * 0.9f;
        BYTE a = (BYTE)(hover ? 26 : 16);
        Pen glow(flash ? Color(a, 40, 200, 140) : Color(a, 70, 140, 235), grow * 1.6f);
        g.DrawEllipse(&glow, rc.X - grow, rc.Y - grow, d + grow * 2, d + grow * 2);
    }

    // 球体：径向渐变
    GraphicsPath circle;
    circle.AddEllipse(rc);
    PathGradientBrush pg(&circle);
    pg.SetCenterPoint(PointF(cx - d * 0.20f, cy - d * 0.22f));
    if (flash)
        pg.SetCenterColor(Color(255, 52, 211, 153));
    else if (pressed)
        pg.SetCenterColor(Color(255, 70, 150, 235));
    else if (hover)
        pg.SetCenterColor(Color(255, 125, 205, 255));
    else
        pg.SetCenterColor(Color(255, 96, 176, 246));

    Color surround[1];
    surround[0] = flash    ? Color(255, 6, 95, 70)
                : pressed  ? Color(255, 8, 62, 140)
                : hover    ? Color(255, 14, 88, 196)
                           : Color(255, 10, 72, 165);
    int nSur = 1;
    pg.SetSurroundColors(surround, &nSur);
    g.FillPath(&pg, &circle);

    // 顶部高光
    {
        GraphicsPath gloss;
        gloss.AddEllipse(rc.X + d * 0.18f, rc.Y + d * 0.10f, d * 0.64f, d * 0.46f);
        PathGradientBrush gp(&gloss);
        gp.SetCenterColor(Color(90, 255, 255, 255));
        Color gs[1] = { Color(0, 255, 255, 255) };
        int ns = 1;
        gp.SetSurroundColors(gs, &ns);
        g.FillPath(&gp, &gloss);
    }

    // 白色描边
    {
        Pen rim(Color(hover ? 150 : 90, 255, 255, 255), 1.2f);
        g.DrawEllipse(&rim, rc);
    }

    // 相机图形
    {
        SolidBrush white(Color(255, 255, 255, 255));
        float bw = d * 0.54f, bh = d * 0.36f;
        float bx = cx - bw * 0.5f;
        float by = cy - bh * 0.5f + d * 0.05f;

        // 顶部取景凸起
        RectF bump(cx - d * 0.11f, by - d * 0.10f, d * 0.22f, d * 0.14f);
        GraphicsPath bp;
        AddRoundRectF(bp, bump, d * 0.035f);
        g.FillPath(&white, &bp);

        // 机身
        RectF body(bx, by, bw, bh);
        GraphicsPath bodyPath;
        AddRoundRectF(bodyPath, body, d * 0.075f);
        g.FillPath(&white, &bodyPath);

        // 镜头
        SolidBrush lens(flash    ? Color(255, 6, 78, 56)
                       : pressed ? Color(255, 20, 84, 168)
                                 : Color(255, 26, 108, 200));
        float lr = d * 0.105f;
        g.FillEllipse(&lens, cx - lr, cy - lr + d * 0.05f, lr * 2, lr * 2);

        // 镜片反光
        SolidBrush shine(Color(170, 255, 255, 255));
        float sr = d * 0.038f;
        g.FillEllipse(&shine, cx - lr * 0.75f, cy - lr * 0.55f + d * 0.05f, sr * 2, sr * 2);
    }

    HBITMAP hbm = nullptr;
    if (bmp.GetHBITMAP(Color(0, 0, 0, 0), &hbm) != Ok) return nullptr;
    return hbm;
}

// 重绘并推送到分层窗口
static void BallUpdate()
{
    if (!g_hwndBall) return;

    HBITMAP bmp = MakeBallBitmap(g_ballSize, g_ballHover, g_ballPressed, g_flashing);
    if (!bmp) return;

    HDC hScreen = GetDC(nullptr);
    HDC hMem    = CreateCompatibleDC(hScreen);
    HGDIOBJ old = SelectObject(hMem, bmp);

    POINT ptSrc = { 0, 0 };
    SIZE  sz    = { g_ballSize, g_ballSize };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(g_hwndBall, hScreen, nullptr, &sz, hMem, &ptSrc,
                        0, &bf, ULW_ALPHA);

    SelectObject(hMem, old);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
    DeleteObject(bmp);
}

// 强制压回最顶层：全屏应用激活后，普通置顶会被抢走
static void BallEnsureTopmost()
{
    if (!g_hwndBall || !IsWindowVisible(g_hwndBall)) return;
    if (g_menuOpen || g_modal) return;      // 菜单/对话框期间别乱插队
    SetWindowPos(g_hwndBall, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void BallClampPos(int* x, int* y)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (*x < vx) *x = vx;
    if (*y < vy) *y = vy;
    if (*x + g_ballSize > vx + vw) *x = vx + vw - g_ballSize;
    if (*y + g_ballSize > vy + vh) *y = vy + vh - g_ballSize;
}

static void BallClampToScreen()
{
    if (!g_hwndBall) return;
    RECT r;
    GetWindowRect(g_hwndBall, &r);
    int x = r.left, y = r.top;
    BallClampPos(&x, &y);
    if (x != r.left || y != r.top) {
        SetWindowPos(g_hwndBall, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_cfg.ballX = x; g_cfg.ballY = y;
    }
}

// 拖动结束后贴边
static void BallSnapToEdge()
{
    if (!g_hwndBall) return;

    RECT r;
    GetWindowRect(g_hwndBall, &r);
    HMONITOR mon = MonitorFromWindow(g_hwndBall, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    int margin = S(12);
    int x = r.left, y = r.top;

    if (x - mi.rcMonitor.left < margin)               x = mi.rcMonitor.left;
    if (mi.rcMonitor.right - (x + g_ballSize) < margin) x = mi.rcMonitor.right - g_ballSize;
    if (y - mi.rcMonitor.top < margin)                y = mi.rcMonitor.top;
    if (mi.rcMonitor.bottom - (y + g_ballSize) < margin) y = mi.rcMonitor.bottom - g_ballSize;

    BallClampPos(&x, &y);

    SetWindowPos(g_hwndBall, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_cfg.ballX = x;
    g_cfg.ballY = y;
    SaveConfig();
}

static void BallCreate()
{
    if (g_hwndBall) return;

    g_ballSize = S(56);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int x = g_cfg.ballX, y = g_cfg.ballY;
    bool valid = (x >= vx && y >= vy &&
                  x + g_ballSize <= vx + vw && y + g_ballSize <= vy + vh);
    if (!valid) {
        // 默认贴在主屏右侧、垂直居中
        x = vx + vw - g_ballSize - S(24);
        y = vy + (vh - g_ballSize) / 2;
    }

    g_hwndBall = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLS_BALL, L"QuickShot 悬浮球", WS_POPUP,
        x, y, g_ballSize, g_ballSize,
        nullptr, nullptr, g_hInst, nullptr);

    if (!g_hwndBall) return;

    BallUpdate();
    ShowWindow(g_hwndBall, SW_SHOWNA);
    SetTimer(g_hwndBall, IDT_BALL_TOP, 2000, nullptr);   // 定时抢回置顶
}

static void BallShow(bool show)
{
    g_cfg.ball = show;

    if (show) {
        if (!g_hwndBall) {
            BallCreate();
        } else {
            ShowWindow(g_hwndBall, SW_SHOWNA);
            BallEnsureTopmost();
        }
    } else if (g_hwndBall) {
        ShowWindow(g_hwndBall, SW_HIDE);
    }

    SaveConfig();
}

static LRESULT CALLBACK BallProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_LBUTTONDOWN:
        SetCapture(h);
        g_ballDrag    = true;
        g_dragMoved   = false;
        g_ballPressed = true;
        GetCursorPos(&g_dragCur);
        {
            RECT r;
            GetWindowRect(h, &r);
            g_dragWnd.x = r.left;
            g_dragWnd.y = r.top;
        }
        BallUpdate();
        return 0;

    case WM_MOUSEMOVE:
        if (g_ballDrag) {
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - g_dragCur.x;
            int dy = pt.y - g_dragCur.y;
            if (!g_dragMoved && (abs(dx) > S(4) || abs(dy) > S(4)))
                g_dragMoved = true;

            if (g_dragMoved) {
                int nx = g_dragWnd.x + dx;
                int ny = g_dragWnd.y + dy;
                BallClampPos(&nx, &ny);
                SetWindowPos(h, nullptr, nx, ny, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } else if (!g_ballHover) {
            g_ballHover = true;
            BallUpdate();
            if (!g_ballTracking) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                g_ballTracking = true;
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        g_ballTracking = false;
        g_ballHover    = false;
        BallUpdate();
        return 0;

    case WM_LBUTTONUP: {
        bool moved = g_dragMoved;
        ReleaseCapture();
        g_ballDrag    = false;
        g_dragMoved   = false;
        g_ballPressed = false;
        BallUpdate();

        if (moved) {
            BallSnapToEdge();      // 拖到位后贴边并记住位置
        } else if (GetKeyState(VK_SHIFT) & 0x8000) {
            DoSelectCapture();     // Shift + 单击 = 区域 / 窗口截图
        } else {
            DoCapture();           // 单击 = 全屏截图
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt;
        GetCursorPos(&pt);
        ShowAppMenu(pt);           // 右键 = 菜单
        return 0;
    }

    case WM_TIMER:
        if (w == IDT_BALL_TOP) BallEnsureTopmost();
        return 0;

    case WM_DESTROY:
        KillTimer(h, IDT_BALL_TOP);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// 前台窗口切换时立即抢回置顶（覆盖全屏应用的关键）
static void CALLBACK ForegroundEventProc(HWINEVENTHOOK, DWORD ev, HWND,
                                         LONG, LONG, DWORD, DWORD)
{
    if (ev == EVENT_SYSTEM_FOREGROUND) BallEnsureTopmost();
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------
static void Balloon(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO)
{
    if (!g_cfg.balloon) return;

    g_nid.uFlags = NIF_INFO;
    lstrcpynW(g_nid.szInfoTitle, title.c_str(), 64);
    lstrcpynW(g_nid.szInfo, text.c_str(), 256);
    g_nid.dwInfoFlags = flags;
    g_nid.uTimeout    = 3000;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_balloonUp = true;      // 记下气泡状态，变色恢复要等它关闭后再动图标

    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

static void AddTrayIcon()
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = g_hwndMain;
    g_nid.uID              = ID_TRAY_ICON;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_MSG;
    // 首次添加时也可能正处在变色期间（例如 explorer 重启），按当前状态选图标
    HICON small = (g_flashing && g_hiconSmallFlash) ? g_hiconSmallFlash : g_hiconSmall;
    g_nid.hIcon            = small ? small : LoadIcon(nullptr, IDI_APPLICATION);

    std::wstring tip = std::wstring(L"QuickShot  ") +
                       HotkeyText(g_cfg.mod, g_cfg.vk) + L" 全屏 / " +
                       HotkeyText(g_cfg.mod2, g_cfg.vk2) + L" 区域";
    lstrcpynW(g_nid.szTip, tip.c_str(), 128);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void UpdateTrayTip()
{
    std::wstring tip = std::wstring(L"QuickShot  ") +
                       HotkeyText(g_cfg.mod, g_cfg.vk) + L" 全屏 / " +
                       HotkeyText(g_cfg.mod2, g_cfg.vk2) + L" 区域";
    lstrcpynW(g_nid.szTip, tip.c_str(), 128);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// 换用正常色 / 变色图标。图标在启动时就两套都备好，这里只做指针切换，
// 避免在截图路径上现画一张图把回执拖慢。
static void ApplyTrayIcon(bool flash)
{
    if (!g_nid.hWnd) return;
    HICON ic = (flash && g_hiconSmallFlash) ? g_hiconSmallFlash : g_hiconSmall;
    if (!ic) return;

    g_nid.hIcon  = ic;
    g_nid.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;   // 与 Balloon() 的收尾保持一致
}

// 截图成功后：托盘图标和悬浮球一起变绿约半秒再恢复。
// 气泡提示容易被忽略，连拍时更需要一个「这张确实存下来了」的即时信号。
static void FlashCaptureFeedback()
{
    g_flashing   = true;
    g_flashStart = GetTickCount64();
    ApplyTrayIcon(true);
    BallUpdate();

    if (g_hwndMain) {
        KillTimer(g_hwndMain, IDT_FLASH);          // 连拍时重新计时，不要提前复位
        SetTimer(g_hwndMain, IDT_FLASH, FLASH_ON_MS, nullptr);
    }
}

static void EndCaptureFlash()
{
    if (!g_flashing) return;

    // Shell 的行为：气泡显示期间对图标做一次 NIM_MODIFY，会把气泡直接关掉，
    // 于是「点气泡打开所在文件夹」就没了。所以这里宁可让托盘多绿一会儿，
    // 也等气泡自己关闭再复位；悬浮球不受影响，已经按时变回去了。
    //
    // 但气泡关闭事件并非所有 Shell 版本都会发（explorer 重启、第三方任务栏等），
    // 所以必须有个兜底时限，否则图标会永久绿下去、定时器还会空转。
    if (g_balloonUp && g_hwndMain &&
        GetTickCount64() - g_flashStart < FLASH_HOLD_MAX_MS) {
        SetTimer(g_hwndMain, IDT_FLASH, 200, nullptr);
        return;
    }
    if (g_balloonUp) g_balloonUp = false;   // 兜底路径：不再等这个气泡

    g_flashing = false;
    ApplyTrayIcon(false);
    BallUpdate();
}

// ---------------------------------------------------------------------------
// 文件夹工具
// ---------------------------------------------------------------------------
static void OpenFolder(const std::wstring& dir)
{
    EnsureDir(dir);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void RevealFile(const std::wstring& file)
{
    if (file.empty() || !PathFileExistsW(file.c_str())) {
        OpenFolder(SaveDir());
        return;
    }
    std::wstring args = L"/select,\"" + file + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static std::vector<std::wstring> ListSubFolders()
{
    std::vector<std::wstring> v;
    std::wstring pattern = g_cfg.baseDir + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
            v.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return v;
}

// 过滤非法文件名字符
static std::wstring SanitizeFolderName(const std::wstring& in)
{
    std::wstring out;
    for (wchar_t c : in) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"'  || c == L'<' || c == L'>' || c == L'|')
            out += L'_';
        else
            out += c;
    }

    // 去掉首尾空格与结尾的点
    while (!out.empty() && (out.front() == L' ' || out.front() == L'\t')) out.erase(out.begin());
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.' || out.back() == L'\t')) out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// 通用模态循环（非对话框资源实现）
// ---------------------------------------------------------------------------
static void RunModal(HWND hwnd, HWND keybox, HWND keybox2 = nullptr)
{
    g_modal = true;
    MSG msg;

    for (;;) {
        if (!IsWindow(hwnd)) break;
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            if (msg.message == WM_QUIT) PostQuitMessage((int)msg.wParam);
            break;
        }
        // 录制快捷键时绕过 IsDialogMessage，否则 Alt/Tab 会被当作助记符吞掉
        HWND focus = GetFocus();
        bool inKeyBox = (keybox && focus == keybox) ||
                        (keybox2 && focus == keybox2);
        if (inKeyBox &&
            (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP ||
             msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP ||
             msg.message == WM_CHAR || msg.message == WM_SYSCHAR)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_modal = false;
}

// ---------------------------------------------------------------------------
// 快捷键录制控件
// ---------------------------------------------------------------------------
static LRESULT CALLBACK KeyBoxProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
        SetFocus(h);
        return 0;

    case WM_SETFOCUS:
        g_kbRecording = true;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_KILLFOCUS:
        g_kbRecording = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)w;
        if (IsModifierVk(vk)) return 0;

        UINT mod = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
        if (GetKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
        if (GetKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
            mod |= MOD_WIN;

        // 无修饰键的 Tab / Alt+F4 放行，避免无法离开输入框或关闭窗口
        if (vk == VK_TAB && mod == 0) return 0;
        // 裸键（无修饰键且非功能键）不接受，否则会把日常输入吞掉
        if (mod == 0 && !(vk >= VK_F1 && vk <= VK_F24)) return 0;
        if (vk == VK_F4 && (mod & MOD_ALT) && !(mod & MOD_CONTROL)) {
            PostMessageW(GetParent(h), WM_CLOSE, 0, 0);
            return 0;
        }

        // 控件 207 是「区域截图」热键框，其余是全屏截图热键框
        if (GetDlgCtrlID(h) == 207) {
            g_tmp.mod2 = mod;
            g_tmp.vk2  = vk;
        } else {
            g_tmp.mod  = mod;
            g_tmp.vk   = vk;
        }
        g_kbRecording = false;

        InvalidateRect(h, nullptr, FALSE);
        SendMessageW(GetParent(h), WM_KB_CHANGED, 0, 0);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        bool hot = (GetFocus() == h);
        HPEN pen = CreatePen(PS_SOLID, hot ? 2 : 1,
                             hot ? RGB(0, 120, 215) : RGB(150, 150, 150));
        HGDIOBJ oldPen   = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        SetBkMode(dc, TRANSPARENT);
        bool second = (GetDlgCtrlID(h) == 207);
        std::wstring text = g_kbRecording ? std::wstring(L"请按下组合键…")
                            : HotkeyText(second ? g_tmp.mod2 : g_tmp.mod,
                                         second ? g_tmp.vk2  : g_tmp.vk);
        SetTextColor(dc, g_kbRecording ? RGB(140, 140, 140) : RGB(20, 20, 20));
        HGDIOBJ oldFont = SelectObject(dc, g_font);
        RECT tr = rc;
        tr.left  += S(10);
        tr.right -= S(6);
        DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);

        EndPaint(h, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// 新建文件夹对话框
//   在本程序内直接给文件夹命名：实时显示完整路径、重名检测、Enter 确认
// ---------------------------------------------------------------------------
#define IDC_NF_EDIT  300

struct NFState {
    std::wstring root;            // 建在哪个目录下
    std::wstring name;            // 用户输入的名称
    bool ok      = false;
    bool existed = false;         // 该文件夹本来就存在，用户选择直接切换
    HWND hEdit   = nullptr;
    HWND hPreview = nullptr;
};

static bool IsDirThere(const std::wstring& dir)
{
    DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring TrimSpace(const std::wstring& in)
{
    std::wstring s = in;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == L' ' || s.back()  == L'\t')) s.pop_back();
    return s;
}

static std::wstring NFJoin(const std::wstring& root, const std::wstring& name)
{
    std::wstring r = root;
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    if (name.empty()) return r;
    return r + L"\\" + name;
}

// 输入时实时刷新「将创建在」路径，并提示重名
static void NFUpdatePreview(NFState* st)
{
    if (!st->hPreview || !st->hEdit) return;

    wchar_t buf[512] = {0};
    GetWindowTextW(st->hEdit, buf, 512);
    std::wstring n = TrimSpace(buf);

    if (n.empty()) {
        SetWindowTextW(st->hPreview, (st->root + L"\\（请在上面输入名称）").c_str());
        return;
    }

    std::wstring full = NFJoin(st->root, n);
    std::wstring text = full + (IsDirThere(full) ? L"   ← 已存在，确定后直接切换过去" : L"");
    SetWindowTextW(st->hPreview, text.c_str());
}

// 自建窗口没有 DefDlgProc，IsDialogMessage 拿不到默认按钮，
// 所以回车/Esc 必须自己在输入框里拦下来
static WNDPROC g_nfOldEdit = nullptr;

static LRESULT CALLBACK NFEditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN) {
            PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            return 0;
        }
        if (w == VK_ESCAPE) {
            PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == L'\r' || w == L'\n')) return 0;   // 别让编辑框发出提示音
    return CallWindowProcW(g_nfOldEdit, h, m, w, l);
}

static LRESULT CALLBACK NewFolderProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    NFState* st = (NFState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    case WM_CREATE: {
        NFState* s = (NFState*)GetWindowLongPtrW(h, GWLP_USERDATA);

        CreateWindowExW(0, L"STATIC", L"文件夹名称：", WS_CHILD | WS_VISIBLE,
                        S(20), S(18), S(200), S(20), h, nullptr, g_hInst, nullptr);

        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", s->name.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 S(20), S(42), S(420), S(28), h,
                                 (HMENU)IDC_NF_EDIT, g_hInst, nullptr);
        s->hEdit = e;

        CreateWindowExW(0, L"STATIC", L"将创建在：", WS_CHILD | WS_VISIBLE,
                        S(20), S(86), S(200), S(20), h, nullptr, g_hInst, nullptr);

        s->hPreview = CreateWindowExW(0, L"STATIC", L"",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                      S(20), S(108), S(420), S(40), h, nullptr, g_hInst, nullptr);

        CreateWindowExW(0, L"STATIC", L"创建后立即切换为当前保存目录，之后的截图都存进这个文件夹。",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        S(20), S(152), S(420), S(20), h, nullptr, g_hInst, nullptr);

        HWND b1 = CreateWindowExW(0, L"BUTTON", L"确定",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  S(280), S(190), S(76), S(30), h, (HMENU)IDOK, g_hInst, nullptr);
        HWND b2 = CreateWindowExW(0, L"BUTTON", L"取消",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  S(364), S(190), S(76), S(30), h, (HMENU)IDCANCEL, g_hInst, nullptr);

        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        (void)b1; (void)b2;

        g_nfOldEdit = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)NFEditProc);

        SendMessageW(e, EM_SETSEL, 0, -1);
        NFUpdatePreview(s);
        return 0;
    }
    case WM_COMMAND: {
        UINT id   = LOWORD(w);
        UINT code = HIWORD(w);

        if (id == IDC_NF_EDIT && code == EN_CHANGE) {
            NFUpdatePreview(st);
            return 0;
        }

        if (id == IDOK) {
            wchar_t buf[512] = {0};
            GetWindowTextW(st->hEdit, buf, 512);
            std::wstring name = TrimSpace(buf);

            if (name.empty()) {
                MessageBoxW(h, L"请先输入文件夹名称。", APP_NAME, MB_ICONWARNING | MB_OK);
                SetFocus(st->hEdit);
                return 0;
            }
            if (name == L"." || name == L"..") {
                MessageBoxW(h, L"这个名字不能用作文件夹名。", APP_NAME, MB_ICONWARNING | MB_OK);
                return 0;
            }
            if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
                MessageBoxW(h, L"名称不能包含下列字符：\n\\ / : * ? \" < > |",
                            APP_NAME, MB_ICONWARNING | MB_OK);
                return 0;
            }
            if (name.back() == L'.' || name.back() == L' ') {
                MessageBoxW(h, L"名称不能以点或空格结尾。", APP_NAME, MB_ICONWARNING | MB_OK);
                return 0;
            }
            if (name.size() > 120) {
                MessageBoxW(h, L"名称太长了，请控制在 120 个字符以内。",
                            APP_NAME, MB_ICONWARNING | MB_OK);
                return 0;
            }

            std::wstring full = NFJoin(st->root, name);
            if (IsDirThere(full)) {
                std::wstring q = L"文件夹「" + name + L"」已经存在。\n\n是否直接切换到它？";
                if (MessageBoxW(h, q.c_str(), APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES)
                    return 0;
                st->existed = true;
            }

            st->name = name;
            st->ok   = true;
            DestroyWindow(h);
            return 0;
        }

        if (id == IDCANCEL) {
            DestroyWindow(h);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// 弹出命名窗口；返回 true 时 out 里是用户确认的文件夹名
static bool NewFolderDialog(HWND owner, const std::wstring& root, NFState* out)
{
    NFState st;
    st.root = root;
    st.name = L"新建文件夹";

    // 客户区 460x228，外框尺寸由 AdjustWindowRectEx 算，避免按钮被标题栏挤掉
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc = { 0, 0, S(460), S(228) };
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

    int x = (GetSystemMetrics(SM_CXSCREEN) - cw) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;

    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, CLS_NEWFOLDER, L"新建文件夹",
                             style, x, y, cw, ch, owner, nullptr, g_hInst, &st);
    if (!h) return false;

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    SetForegroundWindow(h);

    // 预填「新建文件夹」并全选，直接打字即可覆盖
    SetFocus(st.hEdit);
    SendMessageW(st.hEdit, EM_SETSEL, 0, -1);

    RunModal(h, nullptr);
    if (!IsWindow(h)) {
        if (st.ok && out) *out = st;
        return st.ok;
    }
    DestroyWindow(h);
    return false;
}

// ---------------------------------------------------------------------------
// 设置窗口
// ---------------------------------------------------------------------------
struct SetState {
    bool ok = false;
    HWND hKey = nullptr;
    HWND hKey2 = nullptr;
    HWND hFmt = nullptr;
    HWND hPrefix = nullptr;
    HWND hPath = nullptr;
    HWND hAuto = nullptr;
    HWND hBalloon = nullptr;
    HWND hBall = nullptr;
};

static HWND MkChild(HWND parent, const wchar_t* cls, const wchar_t* text,
                    DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void RefreshPathLabel(SetState* st)
{
    SetWindowTextW(st->hPath, g_tmp.baseDir.c_str());
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    SetState* st = (SetState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    case WM_CREATE: {
        SetState* s = (SetState*)GetWindowLongPtrW(h, GWLP_USERDATA);
        g_tmp = g_cfg;   // 编辑副本

        MkChild(h, L"BUTTON", L" 快捷键 ", BS_GROUPBOX, 14, 10, 412, 100, 0);

        MkChild(h, L"STATIC", L"全屏截图：", SS_LEFT, 26, 37, 68, 20, 0);
        s->hKey = CreateWindowExW(0, CLS_KEYBOX, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  S(94), S(33), S(158), S(28), h,
                                  (HMENU)200, g_hInst, nullptr);
        MkChild(h, L"STATIC", L"点击后直接按下组合键", SS_LEFT, 262, 38, 156, 20, 0);

        MkChild(h, L"STATIC", L"区域截图：", SS_LEFT, 26, 71, 68, 20, 0);
        s->hKey2 = CreateWindowExW(0, CLS_KEYBOX, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   S(94), S(67), S(158), S(28), h,
                                   (HMENU)207, g_hInst, nullptr);
        MkChild(h, L"STATIC", L"拖框选区域 / 单击选窗口", SS_LEFT, 262, 72, 156, 20, 0);

        MkChild(h, L"BUTTON", L" 保存 ", BS_GROUPBOX, 14, 118, 412, 140, 0);

        MkChild(h, L"STATIC", L"图片格式：", SS_LEFT, 26, 146, 72, 20, 0);
        s->hFmt = CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  CBS_DROPDOWNLIST | WS_VSCROLL,
                                  S(102), S(143), S(90), S(200), h,
                                  (HMENU)201, g_hInst, nullptr);
        SendMessageW(s->hFmt, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(s->hFmt, CB_ADDSTRING, 0, (LPARAM)L"PNG");
        SendMessageW(s->hFmt, CB_ADDSTRING, 0, (LPARAM)L"JPEG");
        SendMessageW(s->hFmt, CB_ADDSTRING, 0, (LPARAM)L"BMP");
        SendMessageW(s->hFmt, CB_SETCURSEL, (WPARAM)g_tmp.format, 0);

        MkChild(h, L"STATIC", L"文件名前缀：", SS_LEFT, 208, 146, 80, 20, 0);
        s->hPrefix = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_tmp.prefix.c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     S(288), S(143), S(126), S(24), h,
                                     (HMENU)202, g_hInst, nullptr);
        SendMessageW(s->hPrefix, WM_SETFONT, (WPARAM)g_font, TRUE);

        MkChild(h, L"STATIC", L"当前目录：", SS_LEFT, 26, 179, 72, 20, 0);
        s->hPath = MkChild(h, L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS |
                          SS_NOPREFIX, 100, 179, 230, 20, 0);
        MkChild(h, L"BUTTON", L"更改…", BS_PUSHBUTTON | WS_TABSTOP,
                340, 175, 74, 26, 203);

        s->hAuto = MkChild(h, L"BUTTON", L"开机自启",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 26, 214, 152, 24, 204);
        s->hBalloon = MkChild(h, L"BUTTON", L"截图后气泡提示",
                              BS_AUTOCHECKBOX | WS_TABSTOP, 182, 214, 120, 24, 205);
        s->hBall = MkChild(h, L"BUTTON", L"显示悬浮球",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 306, 214, 114, 24, 206);

        // 便携版（U 盘）不做开机自启：注册表项指向的盘符随时会变
        if (IsPortable()) {
            SetWindowTextW(s->hAuto, L"开机自启（U盘不适用）");
            EnableWindow(s->hAuto, FALSE);
            SendMessageW(s->hAuto, BM_SETCHECK, BST_UNCHECKED, 0);
        }

        SendMessageW(s->hAuto,    BM_SETCHECK, g_tmp.autostart ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(s->hBalloon, BM_SETCHECK, g_tmp.balloon   ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(s->hBall,    BM_SETCHECK, g_tmp.ball      ? BST_CHECKED : BST_UNCHECKED, 0);

        MkChild(h, L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP,
                238, 280, 88, 30, IDOK);
        MkChild(h, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP,
                334, 280, 88, 30, IDCANCEL);

        RefreshPathLabel(s);
        return 0;
    }
    case WM_KB_CHANGED:
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDOK) {
            // 读取界面数据并应用
            g_cfg.mod  = g_tmp.mod;
            g_cfg.vk   = g_tmp.vk;
            g_cfg.mod2 = g_tmp.mod2;
            g_cfg.vk2  = g_tmp.vk2;

            int sel = (int)SendMessageW(st->hFmt, CB_GETCURSEL, 0, 0);
            if (sel >= 0) g_cfg.format = sel;

            wchar_t buf[MAX_PATH] = {0};
            GetWindowTextW(st->hPrefix, buf, MAX_PATH);
            std::wstring pf = SanitizeFolderName(buf);   // 前缀同样不允许非法字符
            if (pf.empty()) pf = L"Screenshot";
            g_cfg.prefix = pf;

            // 根目录变了：清空子目录选择，避免指向不存在的路径
            std::wstring oldBase = g_cfg.baseDir;
            g_cfg.baseDir = g_tmp.baseDir;
            if (g_cfg.baseDir != oldBase) {
                g_cfg.subDir.clear();
                EnsureDir(g_cfg.baseDir);
            }

            g_cfg.balloon   = SendMessageW(st->hBalloon, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool autoOn     = SendMessageW(st->hAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool ballOn     = SendMessageW(st->hBall, BM_GETCHECK, 0, 0) == BST_CHECKED;

            if (autoOn != g_cfg.autostart) {
                g_cfg.autostart = autoOn;
                ApplyAutostart(autoOn);
            }

            if (ballOn != g_cfg.ball) BallShow(ballOn);   // 悬浮球显隐即时生效

            SaveConfig();
            st->ok = true;
            DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL) {
            DestroyWindow(h);
            return 0;
        }
        if (id == 203) {   // 更改根目录
            BROWSEINFOW bi = {};
            bi.hwndOwner = h;
            bi.lpszTitle = L"选择截图的根目录";
            bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH] = {0};
                if (SHGetPathFromIDListW(pidl, path)) {
                    g_tmp.baseDir = path;
                    g_tmp.subDir.clear();
                    RefreshPathLabel(st);
                }
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowSettings()
{
    if (g_modal) return;

    int cw = S(440), ch = S(358);   // 高度含标题栏，两行热键录制
    int x = (GetSystemMetrics(SM_CXSCREEN) - cw) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;

    static SetState st;
    st = SetState();

    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, CLS_SETTINGS, L"QuickShot 设置",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             x, y, cw, ch, g_hwndMain, nullptr, g_hInst, &st);
    if (!h) return;

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    SetForegroundWindow(h);

    RunModal(h, st.hKey, st.hKey2);

    if (st.ok) {
        // 两个热键都重新注册；冲突时提示用户换一个组合键
        UnregisterHotKey(g_hwndMain, ID_HOTKEY_CAPTURE);
        UnregisterHotKey(g_hwndMain, ID_HOTKEY_SELECT);
        UINT m2 = g_cfg.mod;
        UINT m3 = g_cfg.mod2;
#ifdef MOD_NOREPEAT
        m2 |= MOD_NOREPEAT;
        m3 |= MOD_NOREPEAT;
#endif
        std::wstring failed;
        if (!RegisterHotKey(g_hwndMain, ID_HOTKEY_CAPTURE, m2, g_cfg.vk))
            failed += HotkeyText(g_cfg.mod, g_cfg.vk) + L"（全屏）\n";
        if (!RegisterHotKey(g_hwndMain, ID_HOTKEY_SELECT, m3, g_cfg.vk2))
            failed += HotkeyText(g_cfg.mod2, g_cfg.vk2) + L"（区域）\n";
        if (!failed.empty()) {
            MessageBoxW(g_hwndMain,
                        (L"下列快捷键注册失败，可能已被其他程序占用，请重新设置：\n" + failed).c_str(),
                        APP_NAME, MB_ICONWARNING | MB_OK);
        }
        UpdateTrayTip();
    }
}

// ---------------------------------------------------------------------------
// 截图动作
// ---------------------------------------------------------------------------
static void DoCapture()
{
    // 先把悬浮球藏起来，否则它会出现在截图里
    bool ballShown = (g_hwndBall && IsWindowVisible(g_hwndBall));
    if (ballShown) {
        ShowWindow(g_hwndBall, SW_HIDE);
        Sleep(70);              // 等窗口真正从屏幕上消失
    }

    std::wstring path, err;
    bool ok = CaptureAndSave(&path, &err);

    if (ballShown && g_hwndBall) {
        ShowWindow(g_hwndBall, SW_SHOWNA);
        BallEnsureTopmost();
    }

    if (!ok) {
        Balloon(L"截图失败", err, NIIF_ERROR);
        return;
    }

    g_lastSaved = path;

    std::wstring name = path;
    size_t k = name.find_last_of(L'\\');
    if (k != std::wstring::npos) name = name.substr(k + 1);

    // 先让图标变色，再弹气泡：气泡本身也会 NIM_MODIFY，顺序反过来会让绿色那一下丢失
    FlashCaptureFeedback();
    Balloon(L"截图已保存", name + L"\n（点击打开所在文件夹）");
}

// ---------------------------------------------------------------------------
// 区域 / 窗口截图
//   进入时先冻结一帧全屏画面，再用一个覆盖整个虚拟桌面的置顶窗口做取景框：
//   拖框 = 区域，单击 = 鼠标下的顶层窗口，W = 窗口吸附提示，F = 整屏，
//   Enter = 保存当前选区，Esc / 右键 = 取消。
//   选区坐标一律用「客户区坐标」= 屏幕坐标 - 虚拟桌面原点，正好对应 DIB 像素。
// ---------------------------------------------------------------------------
#define IDT_PREVIEW_CLOSE  2

struct SelState {
    ScreenImage img;
    HWND  hwnd     = nullptr;
    RECT  sel      = {};        // 当前选区（客户区坐标）
    bool  hasSel   = false;
    bool  dragging = false;
    POINT anchor   = {};
    bool  snap     = false;     // W：鼠标移动时自动吸附到窗口
    bool  done     = false;
    bool  ok       = false;
    bool  full     = false;     // F：直接整屏
    std::wstring note;          // 窗口标题等附加提示
};

static SelState* g_sel = nullptr;

static POINT SelFromScreen(const SelState* s, POINT p)
{
    p.x = ClampInt(p.x - s->img.vx, 0, s->img.w);
    p.y = ClampInt(p.y - s->img.vy, 0, s->img.h);
    return p;
}

static void SelNormalize(RECT* r)
{
    if (r->left > r->right) { int t = r->left; r->left = r->right; r->right = t; }
    if (r->top > r->bottom) { int t = r->top; r->top = r->bottom; r->bottom = t; }
}

// ---------------------------------------------------------------------------
// 窗口命中测试：按 Z 序枚举顶层窗口，找第一个包含鼠标点的。
//   取景框自己覆盖全屏，不能靠 WindowFromPoint（会命中自己），必须枚举排除。
// ---------------------------------------------------------------------------
struct WinHit {
    HWND  self  = nullptr;
    POINT pt    = {};
    HWND  found = nullptr;
    RECT  rc    = {};
    wchar_t title[128] = {};
};

static BOOL CALLBACK EnumTopProc(HWND hw, LPARAM lp)
{
    WinHit* hit = (WinHit*)lp;
    if (hw == hit->self || !IsWindowVisible(hw) || IsIconic(hw)) return TRUE;
    if (GetWindowLongW(hw, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t cls[64] = {0};
    GetClassNameW(hw, cls, 64);
    if (wcscmp(cls, CLS_SELECT) == 0 || wcscmp(cls, CLS_MAIN) == 0 ||
        wcscmp(cls, CLS_BALL) == 0 || wcscmp(cls, CLS_PREVIEW) == 0 ||
        wcscmp(cls, CLS_SETTINGS) == 0 || wcscmp(cls, CLS_NEWFOLDER) == 0)
        return TRUE;

    // DWMWA_EXTENDED_FRAME_BOUNDS = 9：去掉 Aero 的透明阴影边框。
    // 动态取函数，不为 dwmapi 增加链接依赖；取不到就退回 GetWindowRect。
    typedef HRESULT (WINAPI *DwmAttrFn)(HWND, DWORD, PVOID, DWORD);
    static DwmAttrFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE mod = LoadLibraryW(L"dwmapi.dll");
        if (mod) fn = (DwmAttrFn)(void*)GetProcAddress(mod, "DwmGetWindowAttribute");
    }
    RECT r = {};
    bool got = false;
    if (fn) {
        RECT ext = {};
        if (SUCCEEDED(fn(hw, 9, &ext, sizeof(ext)))) { r = ext; got = true; }
    }
    if (!got && !GetWindowRect(hw, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;

    if (PtInRect(&r, hit->pt)) {
        hit->found = hw;
        hit->rc    = r;
        GetWindowTextW(hw, hit->title, 128);
        return FALSE;
    }
    return TRUE;
}

static bool HitTestWindow(POINT screenPt, RECT* clientRect, std::wstring* title)
{
    if (!g_sel) return false;
    WinHit hit;
    hit.self = g_sel->hwnd;
    hit.pt   = screenPt;
    EnumWindows(EnumTopProc, (LPARAM)&hit);
    if (!hit.found) return false;

    RECT r = hit.rc;
    r.left   -= g_sel->img.vx;
    r.right  -= g_sel->img.vx;
    r.top    -= g_sel->img.vy;
    r.bottom -= g_sel->img.vy;
    r.left   = ClampInt(r.left,   0, g_sel->img.w);
    r.right  = ClampInt(r.right,  0, g_sel->img.w);
    r.top    = ClampInt(r.top,    0, g_sel->img.h);
    r.bottom = ClampInt(r.bottom, 0, g_sel->img.h);
    if (r.right <= r.left || r.bottom <= r.top) return false;

    *clientRect = r;
    if (title) {
        *title = hit.title;
        if (title->size() > 24) { title->resize(24); *title += L"…"; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 取景框绘制：双缓冲，冻结画面 + 半透明遮罩；选区内部恢复清晰画面
// ---------------------------------------------------------------------------
static int TextWidth(HDC ref, const std::wstring& t)
{
    HDC m = CreateCompatibleDC(ref);
    if (!m) return (int)t.size() * 10;
    HGDIOBJ old = SelectObject(m, g_font);
    RECT r = {};
    DrawTextW(m, t.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(m, old);
    DeleteDC(m);
    return r.right - r.left;
}

static void SelPaint(HWND h)
{
    SelState* s = g_sel;
    if (!s || !s->img.bits) return;

    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    HDC mem = CreateCompatibleDC(dc);
    if (!mem) { EndPaint(h, &ps); return; }
    HBITMAP mb = CreateCompatibleBitmap(dc, s->img.w, s->img.h);
    if (!mb) { DeleteDC(mem); EndPaint(h, &ps); return; }
    HGDIOBJ oldBmp = SelectObject(mem, mb);

    std::wstring hint = L"拖框选区域 · 单击选窗口 · W 窗口吸附 · F 整屏 · Enter 保存 · Esc 取消";
    if (s->snap) hint += L"   [窗口吸附 开]";
    int hintW = TextWidth(dc, hint) + S(28);
    if (hintW > s->img.w - S(16)) hintW = s->img.w - S(16);
    RECT hintBox = { (s->img.w - hintW) / 2, S(16),
                     (s->img.w - hintW) / 2 + hintW, S(16) + S(30) };

    std::wstring label;
    RECT labelBox = {};
    bool showLabel = false;
    if (s->hasSel) {
        RECT r = s->sel;
        int rw = r.right - r.left, rh = r.bottom - r.top;
        if (rw > 0 && rh > 0) {
            showLabel = true;
            label = ItoS(rw) + L" × " + ItoS(rh);
            if (!s->note.empty()) label += L"   " + s->note;
            int tw = TextWidth(dc, label) + S(18);
            if (tw > s->img.w - S(8)) tw = s->img.w - S(8);
            int maxLeft = s->img.w - tw - S(4);
            if (maxLeft < 0) maxLeft = 0;
            int th = S(24);
            labelBox.left   = ClampInt(r.left, S(4), maxLeft);
            labelBox.top    = (r.top >= th + S(10)) ? (r.top - th - S(6)) : (r.top + S(6));
            labelBox.right  = labelBox.left + tw;
            labelBox.bottom = labelBox.top + th;
        }
    }

    {
        Graphics g(mem);
        Bitmap snap(s->img.w, s->img.h, s->img.w * 4,
                    PixelFormat32bppRGB, s->img.bits);
        g.DrawImage(&snap, 0, 0, s->img.w, s->img.h);

        SolidBrush dim(Color(115, 0, 0, 0));
        g.FillRectangle(&dim, 0.0f, 0.0f, (REAL)s->img.w, (REAL)s->img.h);

        if (s->hasSel) {
            RECT r = s->sel;
            int rw = r.right - r.left, rh = r.bottom - r.top;
            if (rw > 0 && rh > 0) {
                g.DrawImage(&snap,
                            RectF((REAL)r.left, (REAL)r.top, (REAL)rw, (REAL)rh),
                            (REAL)r.left, (REAL)r.top, (REAL)rw, (REAL)rh,
                            UnitPixel);
                Pen border(Color(255, 0, 170, 255), 2.0f);
                g.DrawRectangle(&border, (REAL)r.left, (REAL)r.top,
                                (REAL)rw, (REAL)rh);
                SolidBrush handle(Color(255, 255, 255, 255));
                REAL hs = (REAL)S(6);
                g.FillRectangle(&handle, r.left - hs / 2, r.top - hs / 2, hs, hs);
                g.FillRectangle(&handle, r.right - hs / 2, r.top - hs / 2, hs, hs);
                g.FillRectangle(&handle, r.left - hs / 2, r.bottom - hs / 2, hs, hs);
                g.FillRectangle(&handle, r.right - hs / 2, r.bottom - hs / 2, hs, hs);
            }
        }

        SolidBrush panel(Color(210, 20, 20, 20));
        Pen edge(Color(255, 235, 235, 235), 1.0f);
        if (showLabel)
            g.FillRectangle(&panel, (REAL)labelBox.left, (REAL)labelBox.top,
                            (REAL)(labelBox.right - labelBox.left),
                            (REAL)(labelBox.bottom - labelBox.top));
        g.FillRectangle(&panel, (REAL)hintBox.left, (REAL)hintBox.top,
                        (REAL)(hintBox.right - hintBox.left),
                        (REAL)(hintBox.bottom - hintBox.top));
        g.DrawRectangle(&edge, (REAL)hintBox.left, (REAL)hintBox.top,
                        (REAL)(hintBox.right - hintBox.left),
                        (REAL)(hintBox.bottom - hintBox.top));
    }

    // GDI 文字要等 Graphics 析构之后再画，避免两套引擎同时操作同一个 HDC
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(mem, g_font);
    if (showLabel)
        DrawTextW(mem, label.c_str(), -1, &labelBox,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    DrawTextW(mem, hint.c_str(), -1, &hintBox,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, oldFont);

    int px = ps.rcPaint.left, py = ps.rcPaint.top;
    int pw = ps.rcPaint.right - px, ph = ps.rcPaint.bottom - py;
    if (pw > 0 && ph > 0) BitBlt(dc, px, py, pw, ph, mem, px, py, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(mb);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static void SelFinish(SelState* s, bool ok)
{
    if (s->done) return;
    s->ok   = ok;
    s->done = true;
    // 唤醒 DoSelectCapture 里阻塞的 GetMessage
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);
}

static LRESULT CALLBACK SelectProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    SelState* s = g_sel;
    if (!s) return DefWindowProcW(h, m, w, l);

    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        SelPaint(h);
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return TRUE;

    case WM_LBUTTONDOWN: {
        POINT p;
        GetCursorPos(&p);
        s->anchor   = SelFromScreen(s, p);
        s->dragging = true;
        s->hasSel   = false;
        s->note.clear();
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT screen;
        GetCursorPos(&screen);
        POINT p = SelFromScreen(s, screen);
        if (s->dragging) {
            RECT r = { s->anchor.x, s->anchor.y, p.x, p.y };
            SelNormalize(&r);
            s->sel    = r;
            s->hasSel = true;
            s->note.clear();
            InvalidateRect(h, nullptr, FALSE);
        } else if (s->snap) {
            RECT wr;
            std::wstring title;
            if (HitTestWindow(screen, &wr, &title)) {
                s->sel = wr; s->hasSel = true; s->note = title;
            } else {
                s->hasSel = false; s->note.clear();
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!s->dragging) return 0;
        ReleaseCapture();
        s->dragging = false;
        int rw = s->sel.right - s->sel.left;
        int rh = s->sel.bottom - s->sel.top;
        if (s->hasSel && rw >= 4 && rh >= 4) {
            SelFinish(s, true);                 // 拖框 = 区域
        } else {
            POINT screen;                       // 单击 = 鼠标下的窗口
            GetCursorPos(&screen);
            RECT wr;
            std::wstring title;
            if (HitTestWindow(screen, &wr, &title)) {
                s->sel = wr; s->hasSel = true; s->note = title;
                SelFinish(s, true);
            } else {
                s->hasSel = false;
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        SelFinish(s, false);
        return 0;

    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE:
            SelFinish(s, false);
            return 0;
        case VK_RETURN:
            if (s->hasSel) SelFinish(s, true);
            else { s->full = true; SelFinish(s, true); }
            return 0;
        case 'F':
            s->full = true;
            SelFinish(s, true);
            return 0;
        case 'W':
            s->snap = !s->snap;
            if (!s->snap) { s->hasSel = false; s->note.clear(); }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        SelFinish(s, false);
        return 0;      // 不交给 DefWindowProc，窗口统一由 DoSelectCapture 销毁

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// 保存后的缩略图回执：右下角浮 3 秒，单击打开所在文件夹并选中文件
// ---------------------------------------------------------------------------
struct PrevState {
    std::wstring path;
    std::wstring label;
};
static PrevState g_prev;
static HWND      g_hwndPreview = nullptr;

static LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP mb = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, mb);
        {
            Graphics g(mem);
            SolidBrush bg(Color(255, 32, 32, 32));
            g.FillRectangle(&bg, 0.0f, 0.0f, (REAL)rc.right, (REAL)rc.bottom);

            Image* img = Image::FromFile(g_prev.path.c_str());
            if (img && img->GetLastStatus() == Ok &&
                img->GetWidth() > 0 && img->GetHeight() > 0) {
                REAL boxW = (REAL)(rc.right - S(20));
                REAL boxH = (REAL)(rc.bottom - S(46));
                REAL sc = boxW / (REAL)img->GetWidth();
                REAL sc2 = boxH / (REAL)img->GetHeight();
                if (sc2 < sc) sc = sc2;
                if (sc > 1.0f) sc = 1.0f;
                REAL dw = (REAL)img->GetWidth() * sc;
                REAL dh = (REAL)img->GetHeight() * sc;
                REAL dx = ((REAL)rc.right - dw) / 2.0f;
                REAL dy = (REAL)S(10) + (boxH - dh) / 2.0f;
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.DrawImage(img, RectF(dx, dy, dw, dh));
            }
            delete img;

            Pen edge(Color(255, 100, 100, 100), 1.0f);
            g.DrawRectangle(&edge, 0.0f, 0.0f,
                            (REAL)rc.right - 1, (REAL)rc.bottom - 1);
        }
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(230, 230, 230));
        HGDIOBJ oldFont = SelectObject(mem, g_font);
        RECT tr = { S(8), rc.bottom - S(32), rc.right - S(8), rc.bottom - S(8) };
        DrawTextW(mem, g_prev.label.c_str(), -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(mem, oldFont);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(mb);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_TIMER:
        DestroyWindow(h);
        return 0;

    case WM_LBUTTONUP:
        RevealFile(g_prev.path);
        DestroyWindow(h);
        return 0;

    case WM_RBUTTONUP:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        KillTimer(h, IDT_PREVIEW_CLOSE);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowPreview(const std::wstring& path, int iw, int ih)
{
    if (g_hwndPreview && IsWindow(g_hwndPreview)) DestroyWindow(g_hwndPreview);

    g_prev.path = path;
    std::wstring name = path;
    size_t k = name.find_last_of(L'\\');
    if (k != std::wstring::npos) name = name.substr(k + 1);
    g_prev.label = name + L"   " + ItoS(iw) + L" × " + ItoS(ih);

    int w = S(268), hgt = S(190);
    int x = GetSystemMetrics(SM_CXSCREEN) - w - S(18);
    int y = GetSystemMetrics(SM_CYSCREEN) - hgt - S(18);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             CLS_PREVIEW, L"", WS_POPUP | WS_VISIBLE,
                             x, y, w, hgt, g_hwndMain, nullptr, g_hInst, nullptr);
    if (!h) { g_hwndPreview = nullptr; return; }
    g_hwndPreview = h;
    SetTimer(h, IDT_PREVIEW_CLOSE, 3200, nullptr);
}

// ---------------------------------------------------------------------------
// 进入区域截图模式：冻结画面 → 取景框 → 按选区裁图保存
// ---------------------------------------------------------------------------
static void DoSelectCapture()
{
    if (g_modal || g_sel) return;

    bool ballShown = (g_hwndBall && IsWindowVisible(g_hwndBall));
    if (ballShown) {
        ShowWindow(g_hwndBall, SW_HIDE);
        Sleep(70);
    }

    SelState st;
    std::wstring err;
    if (!GrabScreen(&st.img, &err)) {
        if (ballShown && g_hwndBall) {
            ShowWindow(g_hwndBall, SW_SHOWNA);
            BallEnsureTopmost();
        }
        Balloon(L"截图失败", err, NIIF_ERROR);
        return;
    }

    g_sel   = &st;
    g_modal = true;

    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CLS_SELECT, L"",
                             WS_POPUP | WS_VISIBLE,
                             st.img.vx, st.img.vy, st.img.w, st.img.h,
                             g_hwndMain, nullptr, g_hInst, nullptr);
    if (h) {
        st.hwnd = h;
        SetForegroundWindow(h);
        SetFocus(h);
        InvalidateRect(h, nullptr, FALSE);

        MSG msg;
        while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                st.done = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (IsWindow(h)) DestroyWindow(h);
    } else {
        err = L"无法创建选区窗口。";
        Balloon(L"截图失败", err, NIIF_ERROR);
    }

    g_sel   = nullptr;
    g_modal = false;

    if (ballShown && g_hwndBall) {
        ShowWindow(g_hwndBall, SW_SHOWNA);
        BallEnsureTopmost();
    }

    if (!st.ok) { st.img.Free(); return; }

    RECT area = st.full ? RECT{0, 0, st.img.w, st.img.h} : st.sel;
    int aw = area.right - area.left;
    int ah = area.bottom - area.top;

    std::wstring path;
    bool ok = SaveDibRegion(st.img, area, &path, &err);
    st.img.Free();
    if (!ok) {
        Balloon(L"截图失败", err, NIIF_ERROR);
        return;
    }

    g_lastSaved = path;
    std::wstring name = path;
    size_t k = name.find_last_of(L'\\');
    if (k != std::wstring::npos) name = name.substr(k + 1);
    FlashCaptureFeedback();
    Balloon(L"截图已保存",
            name + L"\n" + ItoS(aw) + L" × " + ItoS(ah) +
            L"\n（点击打开所在文件夹）");
    ShowPreview(path, aw, ah);
}

static void CreateNewFolder()
{
    if (!EnsureDir(g_cfg.baseDir)) {
        MessageBoxW(g_hwndMain, (L"无法创建根目录：\n" + g_cfg.baseDir).c_str(),
                    APP_NAME, MB_ICONERROR | MB_OK);
        return;
    }

    NFState nf;
    if (!NewFolderDialog(g_hwndMain, g_cfg.baseDir, &nf)) return;   // 用户取消

    std::wstring full = NFJoin(g_cfg.baseDir, nf.name);

    if (!nf.existed && !EnsureDir(full)) {
        MessageBoxW(g_hwndMain, (L"创建文件夹失败：\n" + full).c_str(),
                    APP_NAME, MB_ICONERROR | MB_OK);
        return;
    }

    // 建好（或选中已有）后立即切换为当前保存目录
    g_cfg.subDir = nf.name;
    SaveConfig();

    Balloon(nf.existed ? L"已切换到已有文件夹" : L"已新建并切换",
            nf.name + L"\n" + full);
}

// ---------------------------------------------------------------------------
// 托盘菜单
// ---------------------------------------------------------------------------
static void ShowAppMenu(POINT pt)
{
    if (g_modal) return;

    g_menuFolders = ListSubFolders();

    HMENU m = CreatePopupMenu();

    std::wstring cap = L"全屏截图\t" + HotkeyText(g_cfg.mod, g_cfg.vk);
    AppendMenuW(m, MF_STRING, IDM_CAPTURE, cap.c_str());
    std::wstring cap2 = L"区域 / 窗口截图…\t" + HotkeyText(g_cfg.mod2, g_cfg.vk2);
    AppendMenuW(m, MF_STRING, IDM_SELECT, cap2.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU sub = CreatePopupMenu();
    AppendMenuW(sub, MF_STRING | (g_cfg.subDir.empty() ? MF_CHECKED : 0),
                IDM_FOLDER_BASE, L"根目录");
    if (!g_menuFolders.empty()) AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g_menuFolders.size(); ++i) {
        UINT flags = MF_STRING | (g_cfg.subDir == g_menuFolders[i] ? MF_CHECKED : 0);
        AppendMenuW(sub, flags, IDM_FOLDER_ITEM + (UINT)i, g_menuFolders[i].c_str());
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sub, MF_STRING, IDM_NEW_FOLDER, L"新建文件夹…");
    AppendMenuW(sub, MF_STRING, IDM_CHOOSE_BASE, L"更改根目录…");

    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"保存位置");
    AppendMenuW(m, MF_STRING, IDM_NEW_FOLDER, L"新建文件夹…（自己命名）");
    AppendMenuW(m, MF_STRING, IDM_OPEN_FOLDER, L"打开保存文件夹");
    AppendMenuW(m, MF_STRING | (g_cfg.ball ? MF_CHECKED : 0),
                IDM_TOGGLE_BALL, L"显示悬浮球");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"关于");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    HWND prevFg = GetForegroundWindow();
    SetForegroundWindow(g_hwndMain);

    g_menuOpen = true;
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, g_hwndMain, nullptr);
    g_menuOpen = false;
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);
    DestroyMenu(m);   // 会连带销毁子菜单

    // 把焦点还给原来的前台窗口（例如正在全屏的应用），避免它被最小化。
    // 但如果接下来要弹自己的对话框，就不能先还——否则对话框会被全屏应用盖住。
    bool opensUI = (cmd == IDM_SETTINGS || cmd == IDM_NEW_FOLDER ||
                    cmd == IDM_CHOOSE_BASE || cmd == IDM_ABOUT ||
                    cmd == IDM_SELECT);
    if (!opensUI && prevFg && prevFg != g_hwndMain && prevFg != g_hwndBall &&
        IsWindow(prevFg))
        SetForegroundWindow(prevFg);

    if (cmd == 0) return;

    if (cmd >= IDM_FOLDER_ITEM && cmd < IDM_FOLDER_ITEM + (int)g_menuFolders.size()) {
        g_cfg.subDir = g_menuFolders[cmd - IDM_FOLDER_ITEM];
        SaveConfig();
        Balloon(L"已切换保存目录", g_cfg.subDir);
        return;
    }

    switch (cmd) {
    case IDM_CAPTURE:
        DoCapture();
        break;
    case IDM_SELECT:
        DoSelectCapture();
        break;
    case IDM_FOLDER_BASE:
        g_cfg.subDir.clear();
        SaveConfig();
        Balloon(L"已切换保存目录", L"根目录");
        break;
    case IDM_NEW_FOLDER:
        CreateNewFolder();
        break;
    case IDM_CHOOSE_BASE: {
        BROWSEINFOW bi = {};
        bi.hwndOwner = g_hwndMain;
        bi.lpszTitle = L"选择截图的根目录";
        bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH] = {0};
            if (SHGetPathFromIDListW(pidl, path)) {
                g_cfg.baseDir = path;
                g_cfg.subDir.clear();
                SaveConfig();
                Balloon(L"根目录已更改", g_cfg.baseDir);
            }
            CoTaskMemFree(pidl);
        }
        break;
    }
    case IDM_OPEN_FOLDER:
        OpenFolder(SaveDir());
        break;
    case IDM_TOGGLE_BALL:
        BallShow(!g_cfg.ball);
        break;
    case IDM_SETTINGS:
        ShowSettings();
        break;
    case IDM_ABOUT: {
        std::wstring mode = IsPortable()
            ? std::wstring(L"便携模式（配置与截图都保存在程序所在目录）")
            : std::wstring(L"安装模式");
        std::wstring msg = std::wstring(L"QuickShot ") + APP_VERSION +
                           L"\n" + mode +
                           L"\n\n全屏截图：" + HotkeyText(g_cfg.mod, g_cfg.vk) +
                           L"\n区域 / 窗口截图：" + HotkeyText(g_cfg.mod2, g_cfg.vk2) +
                           L"\n保存位置：" + SaveDir() +
                           L"\n配置文件：" + ConfigPath() +
                           L"\n\n托盘左键 = 全屏截图，右键 = 菜单；\n悬浮球左键 = 全屏，Shift + 左键 = 区域。";
        MessageBoxW(g_hwndMain, msg.c_str(), APP_NAME, MB_ICONINFORMATION | MB_OK);
        break;
    }
    case IDM_EXIT:
        DestroyWindow(g_hwndMain);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 主窗口（隐藏，仅接收消息）
// ---------------------------------------------------------------------------
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        AddTrayIcon();
        return 0;

    case WM_HOTKEY:
        if (g_modal) return 0;     // 取景框 / 对话框期间不叠加截图
        if (w == ID_HOTKEY_CAPTURE) DoCapture();
        else if (w == ID_HOTKEY_SELECT) DoSelectCapture();
        return 0;

    case WM_TRAY_MSG:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:
            if (!g_modal) DoCapture();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT pt;
            GetCursorPos(&pt);
            ShowAppMenu(pt);
            break;
        }
        case NIN_BALLOONUSERCLICK:
            g_balloonUp = false;
            RevealFile(g_lastSaved);
            EndCaptureFlash();
            break;

        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE:
            g_balloonUp = false;
            EndCaptureFlash();      // 气泡关了，补做被推迟的图标复位
            break;

        default:
            break;
        }
        return 0;

    case WM_TIMER:
        if (w == IDT_FLASH) {
            KillTimer(h, IDT_FLASH);
            EndCaptureFlash();
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(w) == IDM_EXIT) DestroyWindow(h);
        return 0;

    case WM_DISPLAYCHANGE:
        BallClampToScreen();     // 分辨率/显示器变化后把球拽回可见区域
        return 0;

    case WM_ENDSESSION:
        if (w) {
            if (g_hwndBall) DestroyWindow(g_hwndBall);
            DestroyWindow(h);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(h, IDT_FLASH);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        if (m == g_msgTaskbarCreated && g_msgTaskbarCreated != 0) {
            // 资源管理器重启后重新加回托盘图标。
            // 旧气泡随着 explorer 一起没了，不会再发关闭通知，这里手动清掉标记，
            // 否则变色复位会一直等一个永远不会来的事件（直到撞上兜底时限）。
            // 图标颜色由 AddTrayIcon 按当前 g_flashing 重新决定，不必额外补刷。
            g_balloonUp = false;
            AddTrayIcon();
            return 0;
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// 注册窗口类 / 字体
// ---------------------------------------------------------------------------
static void CreateAppFonts()
{
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    if (g_dpi <= 0) g_dpi = 96;

    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(9, g_dpi, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei", LF_FACESIZE);
    g_font = CreateFontIndirectW(&lf);

    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei", LF_FACESIZE);
    lf.lfWeight = FW_BOLD;
    g_fontBold = CreateFontIndirectW(&lf);
}

static bool RegisterClasses()
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    wc.lpfnWndProc   = MainProc;
    wc.lpszClassName = CLS_MAIN;
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc   = KeyBoxProc;
    wc.lpszClassName = CLS_KEYBOX;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc   = NewFolderProc;
    wc.lpszClassName = CLS_NEWFOLDER;
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc   = SettingsProc;
    wc.lpszClassName = CLS_SETTINGS;
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc   = BallProc;
    wc.lpszClassName = CLS_BALL;
    wc.hbrBackground = nullptr;
    wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc   = SelectProc;
    wc.lpszClassName = CLS_SELECT;
    wc.hCursor       = LoadCursor(nullptr, IDC_CROSS);
    if (!RegisterClassExW(&wc)) return false;

    wc.lpfnWndProc   = PreviewProc;
    wc.lpszClassName = CLS_PREVIEW;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInst;

    // 单实例
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"QuickShot_SingleInstance_Mutex");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"QuickShot 已在运行（见系统托盘）。", APP_NAME,
                    MB_ICONINFORMATION | MB_OK);
        return 0;
    }

    // 截图工具必须声明 DPI 感知，否则高分屏下会截到被缩放的低分辨率画面
    SetProcessDPIAware();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    OleInitialize(nullptr);

    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    GdiplusStartup(&gdipToken, &gsi, nullptr);

    CreateAppFonts();
    LoadConfig();

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_hiconBig        = MakeAppIcon(32, false);
    g_hiconSmall      = MakeAppIcon(16, false);
    g_hiconBigFlash   = MakeAppIcon(32, true);
    g_hiconSmallFlash = MakeAppIcon(16, true);
    if (!g_hiconBig)   g_hiconBig   = LoadIcon(nullptr, IDI_APPLICATION);
    if (!g_hiconSmall) g_hiconSmall = LoadIcon(nullptr, IDI_APPLICATION);
    // 变色图标只是反馈用的锦上添花；万一画不出来就留空，
    // ApplyTrayIcon 会自动退回普通图标，不能因为它失败而拦住启动

    if (!RegisterClasses()) {
        MessageBoxW(nullptr, L"窗口类注册失败。", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }

    g_hwndMain = CreateWindowExW(0, CLS_MAIN, APP_NAME, WS_POPUP,
                                 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_hwndMain) {
        MessageBoxW(nullptr, L"初始化失败。", APP_NAME, MB_ICONERROR | MB_OK);
        return 1;
    }

    // 首次运行：建立默认目录
    EnsureDir(g_cfg.baseDir);

    // 若配置了开机自启，刷新注册表项（exe 可能被移动/改名过）
    if (g_cfg.autostart) ApplyAutostart(true);

    // 注册两个全局热键；冲突时逐个提示，不阻断程序驻留
    UINT regMod  = g_cfg.mod;
    UINT regMod2 = g_cfg.mod2;
#ifdef MOD_NOREPEAT
    regMod  |= MOD_NOREPEAT;
    regMod2 |= MOD_NOREPEAT;
#endif
    std::wstring failed;
    if (!RegisterHotKey(g_hwndMain, ID_HOTKEY_CAPTURE, regMod, g_cfg.vk))
        failed += HotkeyText(g_cfg.mod, g_cfg.vk) + L"（全屏）\n";
    if (!RegisterHotKey(g_hwndMain, ID_HOTKEY_SELECT, regMod2, g_cfg.vk2))
        failed += HotkeyText(g_cfg.mod2, g_cfg.vk2) + L"（区域）\n";
    if (!failed.empty()) {
        std::wstring msg = L"下列快捷键注册失败，可能被其他程序占用：\n" + failed +
                           L"\n程序仍会驻留在托盘，右键图标选择「设置…」可更换快捷键。";
        MessageBoxW(nullptr, msg.c_str(), APP_NAME, MB_ICONWARNING | MB_OK);
    }

    // 悬浮球（默认显示）：压在全屏应用之上，左键截图、右键菜单
    if (g_cfg.ball) BallCreate();

    // 前台窗口一变就抢回置顶，这是能盖住全屏应用的关键
    HWINEVENTHOOK hookFg = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        ForegroundEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    std::wstring hello = std::wstring(L"按 ") + HotkeyText(g_cfg.mod, g_cfg.vk) +
                         L" 全屏截图\n按 " + HotkeyText(g_cfg.mod2, g_cfg.vk2) +
                         L" 区域 / 窗口截图，或点击悬浮球";
    if (IsPortable())
        hello += std::wstring(L"\n截图自动存到程序目录下的 ") + SHOT_FOLDER;
    Balloon(L"QuickShot 已启动", hello);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hookFg) UnhookWinEvent(hookFg);
    if (g_hiconBig)        DestroyIcon(g_hiconBig);
    if (g_hiconSmall)      DestroyIcon(g_hiconSmall);
    if (g_hiconBigFlash)   DestroyIcon(g_hiconBigFlash);
    if (g_hiconSmallFlash) DestroyIcon(g_hiconSmallFlash);
    if (g_font)       DeleteObject(g_font);
    if (g_fontBold)   DeleteObject(g_fontBold);

    GdiplusShutdown(gdipToken);
    OleUninitialize();
    CoUninitialize();

    if (mutex) CloseHandle(mutex);
    return 0;
}
