# 2026-10-19 音频连续性与 CI 交接

这是 [当日早先 0.5.1 交接](handoff-2026-10-19.md)之后的开发记录。任务为继续音频连续性测试、尝试修复断音并解决 GitHub workflow 权限。当前分发为 0.5.2 预览版；最终结果以 [验证记录](verification.md)和[对应机器数据](test-results/windows-runtime.json)为准。

## 实现

- 原来每两秒在音频线程枚举所有进程、构造名称列表；现在用普通优先级独立监视线程做 Toolhelp 检查，音频线程只读取原子状态。仍不打开所选音乐/游戏进程句柄，退出时停止采集，不自动重连新 PID。
- 启动从固定等待 30 ms 改为等待两路真实队列各有约 30 ms 帧，最长 100 ms；适应应用 loopback 首包较迟的情况。无音频时仍允许启动，不无限等待。
- 共享 render 请求 60 ms 缓冲，capture 保留事件模式；每次采集事件或 10 ms 超时后补充输出。采集队列上限从 60 ms 增加至 120 ms，目标余量仍约 20 ms。没有改变 sinc 算法、自动压低或增益。
- 新增数值诊断：最大唤醒/混音/采集处理/进程检查耗时、初始化丢帧、输入欠载、采集不连续、输出 padding 为空次数。数值在停止后保留，不保存音频、不向音频线程加入日志 I/O。
- helper 的音源与采集线程申请 MMCSS。观察端改为独立连续采集线程，释放 WASAPI 包后分析；PowerShell / 控制台日志阻塞不会暂停读包。原 <1 ms 静音阈值不变，严格 Soak 还要求采集不连续为 0。

## 排查过程与失败保留

1. 原引擎 30 秒诊断：进程检查最大 13.368 ms，音频唤醒间隔 13.556 ms，音乐累计欠载 960 帧；最长连续静音 2626 帧，观察端存在不连续。
2. 仅隔离进程检查：检查最大 94.705 ms，音频唤醒最大 9.643 ms，说明耗时检查已不再阻塞音频。音乐累计欠载仍有 1440 帧；诊断完成不代表验收。
3. 按实际帧预填充的 30 秒诊断：音乐/麦克风欠载及溢出为 0；仍有观察端不连续，不能把完成标记当通过。
4. 修正观察端后的首次完整验收：Smoke、70 次启停和源退出通过；Fidelity 在 8 kHz 测试出现 960 帧队列丢弃；Soak 约 21.016 秒检出 448 帧（9.33 ms）静音，无观察端不连续、无输入欠载或队列溢出。此时最大唤醒间隔 15.217 ms、单次 render 8.548 ms，但仅凭各自最大值不能认定它们在同一周期发生。
5. 随后调整输出缓冲与队列余量，重新运行完整严格验收；Smoke、五频点频响与 90.593 秒连续阶段通过，输入欠载/队列溢出/观察端不连续为 0。最终 EXE 的四组屏幕序列、视觉/布局、Brio 录音和 HDMI 回放流程也通过。不通过重跑、放宽阈值或清空失败计数来掩盖问题。

远端负载取样曾达到 97–100%；未关闭其他程序，没有操作游戏、ACE、系统默认设备、系统音量或驱动。最终测试完成不等于已隔离所有外部调度问题。

## GitHub

用户已完成 workflow 授权，`.github/workflows/build.yml` 已启用。首轮 MSVC 编译暴露 Windows SDK `small` 宏冲突，改为 `loaded_small` 等局部变量后修复。运行 `39196630425` 及音频第一阶段提交的 `39197226766` 均为 Windows MSVC / Linux 构建、核心测试通过。最终音频代码 `6458f67` 的运行 `39197513489` 同样双平台通过。CI 使用只读 contents 权限；硬件验收仍独立运行。

## 证据和边界

完整日志与部署脚本在本地 `~/.local/state/partymic/continuity-20261019/`，包含 baseline、candidate、prefill、acceptance、buffered 阶段；远端在 `%USERPROFILE%\PartyMicRemote\continuity-20261019` 下分目录，旧 EXE 与失败日志保留。公开记录只含数值和文件哈希，录音不进入仓库或发行包。

还需网易云当前实播确认、受控真人讲话/耳机与音响 A/B、语音客户端通话、60 分钟稳定性和端到端延迟。无 AEC 集成、代码签名或 LOL / ACE 兼容认证。

## 微软参考

- [IAudioClient::Initialize](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize)：共享事件模式 timing 参数为 0；计时模式可请求缓冲时长，最终查询实际大小。
- [IAudioCaptureClient::GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)：捕获标记与不连续。
- [MMCSS](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service)：音频线程调度机制，不是任意高负载下实时性保证。

## 本轮收尾与继续

- 分发 0.5.2 预览版的 EXE、完整二进制 ZIP、源码 ZIP 与 SHA256SUMS；保留 0.5.1 / 0.5.0 回退版本。主程序和 helper 哈希见验证记录及 JSON，不沿用旧值。
- 优先确认网易云实播，再运行 Player 模式；如果断音复现，同时检查四段链路的数值，而不是直接更改降噪/驱动/游戏设置。
- 真人测试要使用相同音乐片段与增益，分别比较人声静音、音响混音、耳机混音。现有合成 Soak 不能替代双讲听感；GUI 音乐录音测试有意将人声增益设为 0 以隔离数字路径。
- 新输出缓冲增加延迟，后续实测后再考虑可选档位；不要重新在共享事件模式中填非零 timing 参数。
- GUI 回归本轮通过，但高负载下最大 WM_PAINT 间隔 47 ms、屏幕采样最大间隙约 183 ms，不能把这轮结果描述为零跳帧证明。
