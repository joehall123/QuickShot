# QuickShot —— Windows 截图小工具（Win32 / C++）

托盘常驻的全屏截图工具：**按一下自定义快捷键，整块屏幕（含多显示器）自动截图并存到图片文件夹**。
纯 Win32 + GDI+ 实现，无第三方库、无 .NET、无运行库安装包（MSVC 静态链接版本）。

- **系统要求**：Windows 7 SP1 及以上（32 / 64 位）
- **体积**：单文件 exe，约 150 KB，免安装、不写注册表

## 下载即用

到 [Releases](https://github.com/joehall123/QuickShot/releases/latest) 下载 `QuickShot-1.1-portable.zip`，
解压后双击 `QuickShot.exe` 即可，无需安装、无需编译。绿色便携，可直接放在 U 盘里跨机器使用
（配置与截图都留在 exe 同目录，换盘符不受影响）。

---

## 一、功能

| 功能 | 说明 |
|---|---|
| 全局快捷键 | 默认 `Ctrl+Alt+A`，可在设置里改成任意组合（如 `F9`、`Ctrl+Shift+S`） |
| 全屏截图 | 一次抓取整个虚拟桌面，多显示器也能一张图全包含；`CAPTUREBLT` 保证分层窗口也能截到 |
| 自动保存 | 默认存到 `我的图片\Screenshots`，文件名 `Screenshot_20261103_212233.png` |
| 文件夹管理 | 托盘右键 →「保存位置」可**新建文件夹并命名**，新建后自动切换为当前目录，之后所有截图存进去 |
| 多格式 | PNG（默认，无损）/ JPEG（质量 92）/ BMP |
| 前缀自定义 | 文件名前缀可改，例如改成 `温度报警` |
| 桌面悬浮球 | 蓝色小球常驻置顶，**压在全屏应用之上**；左键单击 = 截图，右键 = 菜单，可拖动、可贴边 |
| 托盘常驻 | 左键单击图标 = 立刻截图；右键 = 菜单 |
| 开机自启 | 设置里勾选，写入 `HKCU\...\Run` |
| 保存提示 | 截图后在托盘弹出气泡，显示文件名；点击气泡直接打开所在文件夹并选中该文件 |
| 单实例 | 重复启动只会提示"已在运行"，不会出现两个热键打架 |

---

## 二、U 盘便携版（拷进去双击就能用）

`U盘版/` 这个文件夹就是完整的便携包，**整个文件夹拷到 U 盘即可**，不需要安装、不写注册表：

```
U盘版\                      ← 整个拷到 U 盘
├─ QuickShot.exe            64 位主程序
├─ QuickShot_x86.exe        32 位主程序（老机器用）
├─ 使用说明.txt             给现场同事看的纸质版说明
├─ quickshot.ini            首次运行自动生成（配置）
└─ Screenshots\             首次运行自动创建（截图都在这）
```

### 便携模式下程序会自动做这些事

程序启动时会**探测自身所在目录是否可写**：可写就进入便携模式，所有数据都留在 U 盘里。

| 行为 | 便携模式下的表现 |
|---|---|
| 配置文件 | 放在 exe 同目录 `quickshot.ini`，**不写 `%APPDATA%`** |
| 默认保存目录 | 自动变成 `exe 同目录\Screenshots`，不会跑到「我的图片」 |
| 路径记录方式 | 存**相对路径**（`.\Screenshots`）而不是绝对路径 —— 所以盘符从 `E:` 变成 `F:`、
  甚至整个文件夹搬走，配置依然有效 |
| 开机自启 | **自动禁用**（注册表项的盘符会失效），设置里该项显示为「开机自启（U盘不适用）」并置灰 |
| 注册表 | 不写（除非装成固定位置且手动勾选开机自启） |

想改回「安装模式」：把 exe 放到一个不可写的目录（如 `C:\Program Files\QuickShot`），
配置就会自动落到 `%APPDATA%\QuickShot\`，截图回到「我的图片」。

### 32 位还是 64 位

| 文件 | 适用 |
|---|---|
| `QuickShot.exe` | 64 位系统，绝大多数机器 |
| `QuickShot_x86.exe` | 32 位系统 / 老工控机 |

拿不准就先双击 64 位那个，报「不是有效的 Win32 应用程序」就换 `QuickShot_x86.exe`。
两个 exe 放在同一目录时共用同一份 `quickshot.ini` 和 `Screenshots`，不会各存一份。

### Win7 的额外说明

便携包里的 exe 是用 LLVM-MinGW 编译的，走 **UCRT 运行时**：
- Win10 / Win11：双击直接运行，无需任何东西；
- Win7 SP1：需要系统已装 **KB2999226**（大多数打过补丁的机器都有）。若提示缺少
  `api-ms-win-crt-*.dll`，就到 `build_msvc.bat` 那条路去编译 —— VS2017 工具集 + `/MT`
  静态链接，产物单个 exe，任何 Win7 都能直接跑。

**在 U 盘上跨机器使用，建议就用 MSVC 版**：拷一个 exe 过去，不依赖任何运行库，最省心。

---

## 三、直接使用

1. 双击 `QuickShot.exe`（若已自行编译，路径随你）。
2. 首次运行：托盘出现蓝色相机图标，并提示"按 Ctrl+Alt+A 截图"。
3. 按 `Ctrl+Alt+A` → 屏幕被截取 → 气泡显示 `Screenshot_xxx.png`。
4. 想换保存目录：右键托盘图标 →「保存位置」→「新建文件夹…」→ 输入名字（如 `现场A`）→ 之后的截图自动进这个文件夹。

### 悬浮球

启动后在屏幕右侧中部出现一个蓝色小球（内含相机图标），它始终浮在所有窗口——包括全屏的浏览器、PPT 放映、
视频播放器——的最上层。

| 操作 | 效果 |
|---|---|
| 左键单击 | 立即截全屏（截图瞬间球会自动隐藏，不会拍进图里，截完自动回来） |
| 左键按住拖动 | 移动位置，松手时靠近边缘会自动贴边，位置记进配置 |
| 右键单击 | 弹出与托盘一致的菜单：保存位置 / 新建文件夹 / 打开文件夹 / 设置 / 隐藏悬浮球 |
| 悬停 | 球体变亮，给出可点击的反馈 |

不想要它：右键菜单或「设置…」里取消勾选「显示悬浮球」，想恢复同理。

**托盘菜单结构**

```
截图                    Ctrl+Alt+A
────────────────────────────
保存位置 ▸              根目录            ✓
                        现场A
                        现场B
                        ────────────
                        新建文件夹…
                        更改根目录…
打开保存文件夹
────────────────────────────
设置…
关于
退出
```

---

## 四、编译

源码零依赖，两条路任选。

### 方式 A：Visual Studio（推荐，产物体积小、Win7 兼容最好）

1. 安装 VS2017 / 2019 / 2022，勾选「使用 C++ 的桌面开发」。
2. 在「x64 Native Tools Command Prompt」里进入本目录，执行：

```bat
build_msvc.bat
```

脚本等价于：

```bat
cl /nologo /W3 /O2 /MT /utf-8 /EHsc /DUNICODE /D_UNICODE QuickShot.cpp app.rc ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib gdiplus.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib
```

> `/MT` 静态链接 CRT，生成的 exe 拷到别的机器直接能跑，不用装 VC 运行库。
> **目标机器是 Win7 时，请用 VS2017 的 v141 工具集编译**（VS2019 16.11 及更早同样支持 Win7）；
> VS2022 的 v143 运行库已不再支持 Win7。
> 需要 32 位版（老机器 / 部分工业现场 PC）：把 `vcvarsall.bat x64` 改成 `x86`，`cl` 默认即 32 位。

### 方式 B：MinGW-w64

```bat
build_mingw.bat
```

等价于：

```bat
windres app.rc -o app_res.o
g++ -O2 -municode -mwindows -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ ^
    QuickShot.cpp app_res.o -o QuickShot.exe ^
    -lgdiplus -lshell32 -lshlwapi -lole32 -ladvapi32 -luser32 -lgdi32 -lcomdlg32
```

> 注意：MinGW-w64 有 **ucrt** 和 **msvcrt** 两种运行时。要兼容原版 Win7，请用 `msvcrt` 版工具链；
> `ucrt` 版生成的 exe 在 Win7 上需要已安装 KB2999226（Universal C Runtime）。

### 本目录里现成的 `QuickShot.exe`

这是我在 macOS 上用 LLVM-MinGW（ucrt 运行时）交叉编译出来的**验证版**，已经通过零警告编译，
但我**没有 Windows 环境实测运行**。用法：
- Windows 10 / 11：可以直接双击运行；
- Windows 7：若提示缺少 `api-ms-win-crt-*.dll`，请改用方式 A 自己编译（`/MT`），或先装 KB2999226。

要正式发布给别人用，建议按方式 A 在 VS 里重编一次。

---

## 五、文件说明

| 文件 | 作用 |
|---|---|
| `QuickShot.cpp` | 全部源码（约 1400 行，单文件，含托盘、热键、截图、GDI+ 编码、设置界面） |
| `app.rc` / `app.manifest` | 资源：应用清单（Win7+ 声明、DPI 感知、主题控件）+ 版本信息 |
| `build_msvc.bat` | MSVC 构建脚本 |
| `build_mingw.bat` | MinGW-w64 构建脚本 |
| `QuickShot_x86.exe` | 32 位版本（同一份源码编译） |
| `U盘版/` | 便携包：两个 exe + 使用说明，整个文件夹拷到 U 盘即可 |
| `quickshot.ini` | 运行时自动生成的配置（首次运行后出现） |

配置文件位置：优先放在 exe 同目录（便携）；若该目录不可写（例如放在 `C:\Program Files`），
自动改存 `%APPDATA%\QuickShot\quickshot.ini`。

```ini
[hotkey]
mod=3            ; 1=Alt 2=Ctrl 4=Shift 8=Win，可相加
vk=65            ; 主键虚拟键码，65='A'
[save]
baseDir=C:\Users\xxx\Pictures\Screenshots
subDir=现场A
format=0         ; 0=PNG 1=JPEG 2=BMP
prefix=Screenshot
[ui]
balloon=1
autostart=0
```

---

## 六、实现要点（便于二次开发）

1. **热键**：`RegisterHotKey` + 隐藏消息窗口接收 `WM_HOTKEY`；带 `MOD_NOREPEAT` 防止长按连拍。
2. **截图**：`GetSystemMetrics(SM_XVIRTUALSCREEN …)` 取虚拟桌面矩形 → `CreateDIBSection`(32bpp) →
   `BitBlt(SRCCOPY | CAPTUREBLT)`。
3. **编码**：GDI+ `Bitmap` 直接架在 DIB 内存上，`PixelFormat32bppRGB` 规避 alpha 全 0 导致的透明 PNG 问题。
4. **DPI**：`SetProcessDPIAware()`，否则高分屏下只能截到被缩放的低分辨率画面。
5. **设置界面**：不用 .rc 对话框资源，纯代码建窗 + 自建模态循环（`IsDialogMessage` + 旁路键盘消息），
   好处是只有一个 cpp 就能编译，不依赖资源编辑器。
6. **快捷键录制控件**：自绘小窗口，`WM_KEYDOWN/WM_SYSKEYDOWN` 里用 `GetKeyState` 拼修饰键，
   拒绝裸键（无修饰键且非 F1–F24），避免把日常输入吞掉。
7. **悬浮球**：`WS_EX_LAYERED`（逐像素 alpha，圆球边缘才不糊）+ `WS_EX_TOPMOST` +
   `WS_EX_TOOLWINDOW`（不进任务栏/Alt+Tab）+ `WS_EX_NOACTIVATE`（点它不会抢走全屏应用的焦点）；
   球体用 GDI+ 径向渐变画好后 `UpdateLayeredWindow` 推上去。
8. **压住全屏应用**：单纯置顶会被新激活的全屏窗口顶掉，所以用
   `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 在**每次前台窗口变化时立刻 `SetWindowPos(HWND_TOPMOST)` 抢回**，
   另有 2 秒定时器兜底（菜单/对话框打开期间自动暂停，否则球会盖住自己的菜单）。
9. **截图不拍到球**：截图前 `SW_HIDE` + 等 70ms 让窗口真正消失，截完 `SW_SHOWNA` 恢复（不激活）。
10. **便携模式判定**：启动时往 exe 同目录写一个临时文件探测可写性（写完即删），
   可写 = 便携模式。路径存盘时若位于 exe 目录之下则改写成 `".\xxx"` 相对形式，
   读盘时再拼回当前盘符，从而免疫 U 盘盘符变化。

---

## 七、已知限制

- **真正独占全屏的 DirectX 游戏**（独占模式，非"无边框窗口"）盖不住——这类窗口由显示驱动直接接管输出，
  任何普通置顶窗口都上不去，必须走 DirectX 挂钩注入才能实现（如 Steam/Discord 的游戏内覆盖层）。
  浏览器 F11、PPT 放映、视频播放器、绝大多数工业软件的全屏，都能正常压住。
- 只截**整屏**，没有区域选择 / 窗口选择（按你的需求做的；需要的话可以加）。
- 截图不含鼠标指针。
- 热键被其他软件占用时（如 QQ 的 Ctrl+Alt+A），启动会弹提示，需在设置里换一个组合。
- 不支持 Win7 之前系统（用了 `SetProcessDPIAware`、`SHCreateDirectoryEx` 等 Vista+ API）。
