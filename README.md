# PartyMic · 开黑背景音乐

Windows 11 桌面音频工具：采集指定音乐软件的音频，与实体麦克风混合，输出到 VB-CABLE，让语音客户端把两者作为一个麦克风接收。可选择网易云音乐或其他有 Windows 音频会话的软件。

**当前版本：0.5.1 预览版。** 修复切页时目标页面先出现、再被旧快照盖住的绘制顺序，减少切页准备和动画重绘开销。最终 EXE 已通过 Fedora / Windows 核心测试、实际屏幕序列、滑块与布局检查，以及 Brio 90 录音和 HDMI 回放流程验证。

**本版测试限制：** 独立音频 Smoke 在高负载环境中检出约 59 ms 静音缺口，尚未通过本轮完整硬件验收，因此作为预览版发布。测试后观察到远端 CPU 负载约 97%，原因仍需隔离；没有关闭其他程序。完整结果、EXE 哈希及待办见 [验证记录](docs/verification.md)和[开发状态](docs/development-status.md)。音频算法延续 0.5.0，历史测试不等同于本版重测通过。

网易云当前实播、真人听感 A/B 与 LOL / ACE 兼容性尚未验收。本程序只使用标准 Windows 音频接口和官方 VB-CABLE，不承诺反作弊认证或零封号风险。昨日问题与今日修改见 [10 月 18 日交接](docs/handoff-2026-10-18.md)和[10 月 19 日交接](docs/handoff-2026-10-19.md)。

构建产物：`dist/PartyMic.exe`；包含使用说明和运行库声明的完整压缩包：`dist/PartyMic-win11-x64.zip`。将压缩包拷贝到 Windows 11 x64 后解压，阅读其中的 `使用说明.txt`，安装官方 VB-CABLE 后运行 EXE。校验值见 `dist/SHA256SUMS.txt`。这个交叉编译版本已经静态链接 C++ 运行库，仅导入 Windows 系统 DLL，无需携带 libc++、libunwind、libwinpthread 或 MSVC 运行库 DLL。

![PartyMic 混音工作台](docs/assets/partymic-mixer.png)

## 下载与运行要求

- [发行下载](https://github.com/Kique261/PartyMic/releases)：下载 `PartyMic-win11-x64.zip` 和 `SHA256SUMS.txt`；解压后运行 `PartyMic.exe`。发行包不包含虚拟声卡驱动。
- Windows 11 x64、一个实体麦克风，以及从 [VB-Audio 官网](https://vb-audio.com/Cable/) 安装的 VB-CABLE。首次安装驱动需要管理员权限和重启；PartyMic 日常运行使用普通用户权限。
- 推荐佩戴耳机，避免音响中的音乐再次进入麦克风。摄像头自带麦克风也可选择。
- [构建流程模板](docs/workflows/build.yml) 可放到 `.github/workflows/build.yml` 启用 Windows / Linux CI；当前 GitHub 凭据缺少 `workflow` 权限，未启用 Actions。硬件、听感及游戏兼容性需在真实 Windows 设备上验收。

```powershell
Get-FileHash .\PartyMic.exe -Algorithm SHA256
```

把输出与发行页的 `SHA256SUMS.txt` 对照。仓库里的 `src/` 是源码，EXE 和 ZIP 通过发行页分发，不提交录音或构建缓存。

## 音频路线

```text
网易云音乐等软件 ── Windows 应用进程树回环采集 ── 音乐音量 ──┐
                                                          ├─ 混音 / 限幅 ── CABLE Input
摄像头或其他实体麦克风 ── Windows WASAPI 采集 ── 人声音量 ──┘                     │
                                                                               ▼
你的耳机 ← 音乐软件原来的播放路径                       CABLE Output（虚拟麦克风）
                                                                               │
                                                                               ▼
                                                               LOL / 其他语音客户端
```

VB-CABLE 的名称容易混淆：**CABLE Input 是播放设备，PartyMic 往里面写；CABLE Output 是录音设备，语音客户端从这里读。** 设备名以驱动实际显示为准。

## 准备与使用

1. 在 Windows 11 x64 安装官方 VB-CABLE，按安装程序要求重启。官网：<https://vb-audio.com/Cable/>。驱动安装需要管理员权限；PartyMic 运行时不需要。使用及分发遵循 VB-Audio 自己的授权条款，本项目不打包或自动安装它。
2. 将 Windows 默认播放设备、默认通信播放设备以及语音客户端扬声器都设为耳机。网易云音乐也正常播放到耳机。**不要把 CABLE Input 设为系统默认播放设备**，否则游戏、队友声音等可能一起进入虚拟线路；PartyMic 会检查并拒绝这种默认设置。
3. 在 Windows「设置 → 隐私和安全性 → 麦克风」允许桌面应用访问麦克风。
4. 打开网易云音乐播放一首歌，再打开 `PartyMic.exe`，点击「刷新设备 / 软件」。选择 `cloudmusic.exe`，实体麦克风选择罗技摄像头对应设备（按 Windows 实际识别名称选择），虚拟声卡选择 `CABLE Input (VB-Audio Virtual Cable)`；安装过程中也可能暂时显示为 `扬声器 (VB-Audio Virtual Cable)`，不要选择 16 通道端点。
5. 点击「开始混音」。音乐默认 25%，麦克风 100%；可以调到 0–200%，或分别静音。自动压低默认关闭，以保留音乐原音量；需要时开启，可将讲话时保留音乐调至 25–100%，默认 65%。
6. 在实际使用的 LOL / 腾讯语音客户端中，把麦克风输入改为 `CABLE Output (VB-Audio Virtual Cable)`。语音软件如果只使用 Windows 默认通信录音设备，可将 **CABLE Output** 设为默认通信录音设备。输出仍然使用耳机。
7. 点击侧栏「录音与试听」录制、回放，确认音乐和人声；之后再用语音客户端的麦克风测试确认。停止按钮或关闭窗口都会停止采集和输出。最小化后继续工作。

不要打开 CABLE Output 或实体麦克风的「侦听此设备」，避免回声或啸叫。PartyMic 不额外监听自己的人声，也不改变任何系统默认音频设备。

## 人声录音与试听

调整「人声音量」（0–200%，默认 100%），它同时影响混音和「仅人声」录音。「仅人声」可在未播放音乐、未开始混音时使用；点击「录制 15 秒」正常讲话，自动结束或提前停止后保存到桌面 **PartyMic录音** 文件夹。选择「音乐 + 人声」时，先开始混音，录到的就是 CABLE Output 实际输出。

「试听输出」选择实际耳机、HDMI 或扬声器；点击「试听刚才录音」后先停止混音，再通过所选设备播放。试听音量可单独调整，也可点击「文件夹」用系统播放器打开 WAV。试听结束不自动恢复混音。录音为无损 48 kHz / 16 bit / 双声道 WAV，仅保存在本地，不会自动上传。

若讲话时音乐发闷，先保持自动压低关闭，降低过高的人声音量，并用耳机对比音响外放。外放被麦克风拾回与数字音乐叠加可能改变听感；当前未实现软件回声消除。具体诊断见 [音质与试听排查](docs/troubleshooting.md)。

## 维护文档

- [当前开发状态](docs/development-status.md)：未完成事项、测量证据、失败测试与下一步验收。
- [更新记录](CHANGELOG.md)：每版行为和已知限制。
- [架构](docs/architecture.md)：数据路径、音频参数、录音线程和安全边界。
- [开发与远程验证](docs/development.md)：Fedora 构建、Windows 测试、版本与证据管理。
- [故障排查](docs/troubleshooting.md)：人声压低、外放回声和试听设备选择。
- [验收清单](docs/windows-validation.md)与[实际验证记录](docs/verification.md)。

## 编译

### 在 Linux 上交叉编译 Windows x64

使用 LLVM-MinGW 官方发布版 `20260616` 的 UCRT Ubuntu 22.04 x86_64 工具链，本次在 Fedora 44 执行，编译器为 Clang 22.1.8。工具链可解压到用户缓存目录，无需系统安装或管理员权限。

- 官方工具链：<https://github.com/mstorsjo/llvm-mingw/releases/tag/20260616>
- 文件：`llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64.tar.xz`
- 官方资产 SHA-256：`534b92e067b22a6b4441f48ae9240a3341b17825d04d577eab0cf85c44b4deda`（本次下载已核对）

在本机重建及打包：

```bash
bash scripts/build-windows.sh $HOME/.cache/partymic/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64
```

先下载、校验并解压官方文件，再把实际解压路径传给脚本。脚本编译 Windows EXE，检查 PE 格式、普通用户权限清单及 DLL 依赖，并生成二进制包、更新后的源码包、运行库声明和 SHA-256 校验文件。它不运行 Windows 程序。

### 在 Windows 上原生编译

安装 Visual Studio 2022 / Build Tools 的「使用 C++ 的桌面开发」、CMake 3.24+，以及 **Windows 11 SDK 10.0.22621.0 或更新版本**。在 VS 开发者终端执行：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\PartyMic.exe
```

如果机器装有多个 SDK，可在配置时添加 `-DCMAKE_SYSTEM_VERSION=10.0.22621.0`，或者你已安装的更新 SDK 版本。使用 MSVC 原生编译时，默认 MSVC 运行库为动态链接，目标电脑可能需要 Microsoft Visual C++ 2015–2022 x64 Redistributable；这与已经提供的静态 C++ 运行库交叉编译产物不同。仓库包含 [GitHub Actions 模板](docs/workflows/build.yml)，包括 Windows 构建与 EXE artifact；将其放到 `.github/workflows/build.yml` 并使用含 `workflow` 权限的凭据提交后生效。目前没有已执行的 Windows 原生 CI 记录。

Windows 真机可手动运行 `build\Release\windows_audio_smoke.exe --run`（交叉编译产物位于 `build-windows/windows_audio_smoke.exe`）。它使用 Brio 90、立体声 VB-CABLE 和两个自建低音量测试音进程，验证音源隔离、增益、静音、实体麦克风输入、启停和源进程退出。会短暂向默认播放设备播放测试音并采集麦克风，只记录数值，不保存录音；需要登录用户的交互桌面。该硬件测试不会随 CTest 自动运行。另可使用 `--soak 90` 做短时连续混音检查，或在网易云正在播放时使用 `--player cloudmusic.exe 60` 验证真实音源。`scripts/run-windows-audio-test.ps1` 可保存 UTF-8 数值日志和测试 EXE 哈希；不保存录音。

Linux 上运行跨平台音频核心测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 腾讯 ACE 与使用边界

设计目的只是通过标准录音设备向语音客户端提供混音，没有与游戏或 ACE 集成。应用使用普通用户权限，使用微软 `ActivateAudioInterfaceAsync` 的应用回环采集及 WASAPI 音频接口；进程名称通过 Toolhelp 快照和音频会话枚举获取，不打开目标进程句柄、不读写内存、不注入 DLL、不挂钩游戏、不模拟输入、不使用全局快捷键钩子、不操作反作弊服务、不安装自制驱动，也不修改游戏文件。

界面会排除部分已知的游戏/启动器/反作弊进程名称，防止误选；这只是使用提示，**不是 ACE 白名单、认证或封号风险保证**。Windows 接口正规、虚拟声卡驱动已签名，并不等于腾讯已认可所有使用场景。这里没有查到腾讯对 PartyMic 或这种完整组合的公开兼容性承诺。不要尝试隐藏软件、绕过检测、关闭 ACE、启用测试签名或安装来源不明的驱动。如官方提示冲突，停止使用并向游戏官方确认，不做规避。

## 已实现与限制

- 原生中文 Win32 界面；按有音频会话的软件选择音源，采集其进程树。不会录整台电脑的系统混音。
- 同名子进程会向上寻找同名父进程，覆盖常见播放器的多进程情况。若声音由独立的异名服务进程发出，必须选择实际产生音频的会话；无法保证覆盖所有播放器或浏览器。浏览器通常无法按标签页单独采集。
- 固定内部格式为 48 kHz、32 位浮点、双声道；实体设备与虚拟声卡经 Windows 音频引擎转换。初始化不支持或设备被独占时显示 HRESULT 并停止，不回退为整机录音。
- 音频线程申请 Windows MMCSS Audio 调度；启动前预填充约 30 ms 输入，停止时先关闭采集，再补写约 250 ms 静音并重置缓冲。有界音频缓冲、输入欠载补静音、过量丢最旧数据、独立增益、平滑调音、即时静音、基于音量阈值的自动压低、输出软限幅、三路电平表。自动压低不是语音识别，环境噪声也可能触发。
- 缓冲容量每路 60 ms，共享事件模式 WASAPI 的设备缓冲由 Windows 音频引擎决定；这些是内部参数，**不是端到端延迟承诺**。总延迟受实体设备、VB-CABLE、系统调度和语音客户端编码影响。两路输入各自使用有界队列和 64 抽头窗函数 sinc 重采样，在 ±0.5% 范围内平滑调整读取速率，以校正独立设备的时钟偏差；目标缓冲约 20 ms；滤波内部增加约 0.65 ms 延时。已用合成信号检查 1–20 kHz 幅度响应，长期运行、极端调度延迟和主观音质仍需验证。
- 无热插拔自动恢复；设备断开、格式变化或音源退出时，可能停止并要求刷新、重新开始。检测源进程退出的周期约 2 秒，不自动跟随新的进程号。
- 混音时实体麦克风是必选；可以静音实现只播音乐。当前不提供独立降噪、回声消除、设置持久化、托盘、自动更新或安装包。仅人声录音可独立使用。
- 语音客户端的降噪、语音门限、按键说话和自动增益可能截断音乐。需要在客户端允许的设置中调试，PartyMic 不会替你绕过这些机制；按键说话模式下仍需由用户自己按键。
- 受保护内容、独占音频、ASIO 或特殊音频路径可能无法回环采集。播放器如果退出后重开，刷新后重新选择。

## 验证与故障排查

混音核心测试覆盖：环形队列顺序、环绕、溢出丢帧、快慢时钟偏差校正、超大包、欠载补静音、静音包、异常数值、独立增益、即时静音、输出限幅、自动压低/恢复，以及不同分包大小下的输出一致性。

本次实际验证记录见 [docs/verification.md](docs/verification.md)，Windows 手工验收步骤见 [docs/windows-validation.md](docs/windows-validation.md)。

常见错误：

| 现象 | 检查 |
| --- | --- |
| 没有虚拟声卡 | 安装官方 VB-CABLE 后重启；确认 Windows 播放列表中存在 CABLE Input，再刷新 |
| 音乐软件不显示 | 先播放音乐，再刷新；选择实际产生声音的进程；特殊 ASIO/独占路径不保证支持 |
| 音乐有电平、队友听不到 | 语音客户端输入须选 CABLE Output；检查按键说话、降噪及语音门限 |
| 麦克风无电平 | 确认选择摄像头麦克风、隐私权限、硬件静音、设备未被独占 |
| 提示 default/communications playback | 将 Windows 默认及默认通信播放设备改回耳机 |
| `0x88890004` 等设备错误 | 设备可能被拔出或重置；停止、刷新并重新选择 |
| 初始化失败 / 不支持格式 | 取消设备独占占用；检查共享模式，尝试在设备高级属性中设为 48 kHz，再重试 |
| 能录音但语音里音乐断续 | 客户端可能过滤背景音乐；在官方提供的选项中调整降噪、自动增益和输入门限 |

## 参考资料

- 微软应用回环示例（包括仅采集选定进程及子进程、系统版本要求）：<https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/>
- 微软激活参数：<https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_activation_params>
- WASAPI 初始化和共享模式转换：<https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize>
- VB-CABLE 官网及安装说明：<https://vb-audio.com/Cable/>
- 腾讯 ACE 官网（一般产品说明，不是本软件兼容证明）：<https://www.anticheatexpert.com/>

界面配色来源、布局和控件维护见 [UI 设计说明](docs/ui-design.md)。
