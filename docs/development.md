# 开发、更新与远程验证

当前分发为 0.5.2 预览版：严格合成音源 Smoke、Fidelity、90 秒连续性与 70 次启停通过，当前网易云实播和真人听感仍待确认。恢复开发先读 [最新交接](handoff-2026-10-19-continuity.md)。入口为 [README](../README.md)，待办见 [development-status.md](development-status.md)，数据路径见 [architecture.md](architecture.md)。

## 环境与构建

项目为 C++20、CMake 3.24+、Win32 GUI，没有第三方音频库；频响校正由项目内的窗函数 sinc 实现，界面淡入使用系统 msimg32。工作环境为 Fedora，通过 LLVM-MinGW UCRT 交叉编译 Windows x64；Windows 已登录的交互桌面用于真实设备验证。不要用 SSH 的 session 0 或 RDP 重定向麦克风替代实体 Brio 90 验收。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash scripts/build-windows.sh /absolute/path/to/llvm-mingw
```

`build-windows/PartyMic.exe` 为主程序；`windows_audio_smoke.exe` 是只记录数值的硬件测试；`windows_audio_record.exe` 是明确指定后才保存 WAV 的人工试听辅助工具。不要把后者的录音行为误写成前者的行为。`dist/` 包含主 EXE、二进制 ZIP、按 CMake 版本生成的源码 ZIP 和 SHA256SUMS。

## 修改与回归

1. 音频算法改动先在 `tests/mixer_tests.cpp` 加入有意义的行为回归：默认不压低、可调压低、音乐分量保真、静音、限幅、分包一致性、时钟漂移。
2. Windows 设备或音频改动在真实交互桌面运行 `mixer_tests.exe` 和显式硬件测试。GUI/试听改动还要运行 `scripts/test-windows-preview-ui.ps1`，它会保存短录音，操作本程序窗口并短暂回放。界面绘制改动再执行 `test-windows-layout.ps1` 与 `test-windows-visual.ps1`，后者覆盖快速滚轮/拖动残影、动效与资源收敛。
3. 修改 `CMakeLists.txt` 项目版本、`CHANGELOG.md` 和用户文档。重新编译后不要沿用旧 EXE 的真机通过标记。
4. 将新 EXE 与测试程序部署到新的版本目录，先计算 SHA-256；保留可回退版本和失败日志。不要覆盖正在运行的文件。
5. 更新 `docs/test-results/windows-runtime.json`，记录主程序和硬件测试程序各自哈希、运行时间、检查范围和限制。旧版记录移至带版本号文件，不能把历史 90 秒成绩直接标成新版本结果。
6. 使用打包脚本生成 ZIP；核对压缩包内容和哈希，并在 Windows 重新校验后解压。机器可读记录必须与分发 EXE 对应。

核心测试由 CTest 自动运行，硬件测试不会自动运行。只改文档时无需重复完整设备回归，但仍需重新打包并校验分发内容。

## 局域网 Windows 流转

现有环境使用专用 SSH 别名 `partymic-windows`，本机 SSH 配置路径由维护者自行提供。不要把私钥、密码、个人配置或录音放入源码包。

```bash
ssh -F /path/to/windows-ssh.conf partymic-windows whoami
scp -F /path/to/windows-ssh.conf build-windows/PartyMic.exe partymic-windows:PartyMicRemote/VERSION/
```

通过 PowerShell `New-ScheduledTaskPrincipal -LogonType Interactive -RunLevel Limited` 在当前登录用户桌面启动测试。任务只用于运行自己的程序，结束后删除明确命名的临时任务。调试参数应通过 `-EncodedCommand` 或脚本文件传入，避免混用 Bash 和 PowerShell 字符串转义。

交互桌面中可执行：

```powershell
.\mixer_tests.exe
.\windows_audio_smoke.exe --run
.\windows_audio_smoke.exe --fidelity
.\windows_audio_smoke.exe --soak 90
.\windows_audio_smoke.exe --diagnose 30
.\windows_audio_smoke.exe --player cloudmusic.exe 60
.\test-windows-preview-ui.ps1 -Executable .\PartyMic.exe -LogDirectory .\tests -AudioTestExecutable .\windows_audio_smoke.exe
```

`windows_audio_effects.exe` 仅探测 Brio 90 在 Other/Communications 两种私有流类别下的效果状态，初始化后不启动采集、不更改设备效果或保存音频；不要将“接口可用”写成消回声效果通过。

`--diagnose` 仅用于收集连续性诊断，完成不代表验收通过；严格 Smoke / Soak 仍要求原有 <1 ms 静音阈值，Soak 额外要求捕获不连续为 0。helper 的音源与后台采集线程注册 MMCSS；采集释放包后分析，主线程打印不会暂停读包。

音频 runner 必须同时检查退出码和 `ALL ... CHECKS PASSED` 完成标记。传递原生命令行参数时使用 `[string[]]`，避免一个参数退化为字符串索引，造成“进程退出正常但测试根本没运行”。

## 界面与线程维护

当前 Win32 界面采用三页侧栏、可缩放卡片、per-monitor DPI 和内容区域垂直滚动；侧栏和页头固定。新增控件必须设置页面掩码和宽度锚点，同步内容高度，检查小窗口滚动截图。主题与参考来源见 [ui-design.md](ui-design.md)。自动化只枚举目标 PartyMic PID 的窗口并发送普通控件消息，不使用全局钩子。关键控件 ID 在 `main.cpp` 的枚举中定义；更新时同步 GUI 测试。

UI 线程控制工作线程启停。`AudioEngine` 使用互斥锁读取设置，状态/峰值使用锁或原子；`AudioPreview` 的录音数据只由自己的线程持有，停止必须 join，结束标志置空前先释放流。不可从音频线程直接更新 HWND，也不可将保存录音的磁盘操作放入实时混音线程。

试听前必须停止混音，播放到明确选择的物理设备；停止试听不自动恢复混音。不要为“实时监听”移除互斥约束，否则外放可能再次进入麦克风。录音是用户显式操作，只保存在本地桌面文件夹，不进入测试结果 JSON 或发行 ZIP。

## 仍需推进

真人主观对比、语音客户端编码/降噪、60 分钟稳定性、端到端延迟、热插拔、可选麦克风 AEC、设置持久化、应用代码签名。腾讯 LOL/ACE 兼容性没有认证；不得通过关闭或绕过反作弊来“验证”。


## GitHub 维护

公开仓库为 `Kique261/PartyMic`，主分支 `main`。提交源码、图标、脚本及不含私人路径的数值证据；`build*`、`dist/`、录音、密钥与连接配置不入库。发行页提供 EXE / ZIP、源码包和 SHA256SUMS；保留 0.5.0 本地回退包。当前 0.5.2 为预览版；合成音源验收不等于当前网易云、真人通话或游戏兼容性通过。旧版和中间候选失败继续保留。

2026-10-19 用户已完成 `workflow` 授权，配置已移到 `.github/workflows/build.yml` 并推送。Windows MSVC 与 Linux 构建、核心测试实际通过（运行 `39196630425`）。首次 MSVC 编译暴露 Windows SDK 的 `small` 宏与局部变量冲突，已重命名修复。后续提交继续检查对应 Actions 结果；不要把访问令牌写入脚本或 Markdown。
