# 当前开发状态与交接

更新日期：2026-10-19。当前 0.5.2 预览版，公开仓库 `Kique261/PartyMic`。本轮严格合成音源 Smoke / Fidelity / 90 秒 Soak 和 70 次启停通过，GitHub Windows MSVC / Linux CI 已授权启用并通过。最终哈希和各项结果见 [验证记录](verification.md)与[机器数据](test-results/windows-runtime.json)；开发恢复先读 [连续性交接](handoff-2026-10-19-continuity.md)。

## 0.5.2 完成

- 进程枚举从音频线程移到独立监视线程；进程检查耗时曾达数百毫秒，已与音频流处理隔离。
- 启动按实际帧预填充；共享输出缓冲从本机 22 ms 增为 60 ms，采集队列上限 120 ms，校正目标仍 20 ms。以一定延迟换取调度余量，端到端延迟未测。
- helper 的音源与连续采集线程申请 MMCSS，分析在释放包之后，打印日志不会暂停采集；严格断音阈值保留，Soak 新增观察端不连续为 0。
- 90 秒连续阶段实际约 90.593 秒，音乐/人声欠载、队列丢帧、观察端不连续和输出空 padding 次数为 0，最长连续零样本为 1 帧。测试前 CPU 采样 97%；未关闭后台程序。
- 五频点最大绝对幅度偏差约 0.00145 dB；70 次启停句柄 196 → 196，90 秒阶段 237 → 237。记录仅适用于本次测试，不代表任意负载下绝无断音。

最终 EXE 的屏幕序列、视觉/布局、Brio 录音和 HDMI 试听流程也通过；真人主观听感仍待确认。

## 历史失败继续保留

0.5.1 仍是预览版，完整记录见 [旧验证文档](verification-0.5.1.md)及版本 JSON。其 Smoke 约 58.8 ms 静音、Soak 约 9.1 秒失败不改写。

0.5.2 的首个严格候选 Smoke 通过，但 Fidelity 在 8 kHz 出现 960 帧丢弃，Soak 约 21 秒检出 448 帧静音，之后才调整缓冲并重新完整验收。诊断完成与严格验收分开记录；详情见最新交接和机器数据。

## 0.5.0 已完成

- 修复时钟校正造成的高频衰减，完成 64 抽头窗函数 sinc；新增正负漂移、独立双声道频响与队列不连续后的历史清理回归。Fedora、Windows 核心通过。
- 新硬件测试 `windows_audio_smoke.exe --fidelity` 覆盖整个数字路径的五个频点；实际 VB-CABLE 幅度误差 <0.002 dB。Smoke、70 次启停和 90 秒 Soak 通过。
- 切页改为 200 ms 内容快照淡入，原生控件只布局一次，快速导航衔接当前画面；修复焦点通知提前取消过渡。最终主程序通过完整录音试听、布局、视觉与帧间隔测试。
- 新增只查询能力的 `windows_audio_effects.exe`：Brio 私有流在 Communications 类别下报告 AEC 开启并支持参考端点控制，Other 类别下 AEC 关闭。未向主程序加入 AEC，也未设置设备效果。

用户的发闷报告来自本程序录音回放；不能归咎于网易云源文件损坏。数字高频损失和外放被麦克风拾回可能同时存在；修复数字路径不等于回声问题全部解决。

## 仍需完成的验证与功能

1. 网易云专项：0.5.0 测试时会话存在但音乐采集 RMS 为 0，未进入请求的 60 秒实播阶段。确认播放后重新运行 Player 模式，保留先前失败记录。
2. 真人音质：保持音乐片段、增益和试听输出一致，比较麦克风静音、外放混音和耳机混音。记录人声清晰度、音乐层次与削波；自动测试中的麦克风有活动不代表受控真人讲话通过。
3. 回声消除：可优先研究实际可用的 Windows 通信流 AEC，设置参考为真实 HDMI 输出，只处理麦克风分支。需检查系统通信压低、其他增强处理、输出设备选择、设备时钟及双讲效果；接口可用不等于效果合格。没有新增“仅音乐”录音模式，可先显式静音麦克风做对照并恢复原设置。
4. 动效：0.5.2 本窗口计时和四组屏幕序列回归通过；本轮准备约 63–105 ms，最大绘制间隔 47 ms，屏幕采样间隙可达 183 ms。未测 DWM 呈现或显示器同步，可继续优化。系统关闭动画、多屏 DPI 与其他刷新率仍需验收。
5. 稳定性：60 分钟、热插拔、休眠唤醒、端到端延迟、代码签名仍待推进。本机 sanitizer 运行库缺失，ASan/UBSan 未完成。

不要将以上项目提前标为通过。0.5.0 完成的改动与尚待确认的听感分别记录，后续新 EXE 应重新记录哈希和对应结果。

## 复现与证据

参考 [开发文档](development.md)。核心测试运行 `ctest --test-dir build --output-on-failure -V`；硬件 runner 支持 Smoke、Fidelity、Soak、Player、Diagnostic，并同时核对退出码与完成标记。硬件与 GUI 测试在已登录实体交互桌面执行，不能用 SSH session 0 或 RDP 重定向麦克风代替。

完整证据目录：`~/.local/state/partymic/remote-20261018/v0.5.0-validation/`。`tests` 为核心/硬件日志，`gui-tests-run1` 为首个淡入候选的失败记录，`gui-tests-run2` 为最终 EXE 的 GUI 通过记录，`extra-tests` 为设备能力与网易云静音结果。实际录音只保存在 Windows 桌面文件夹，不随源码或发行包分发。

早期临时频响诊断曾测得线性插值在 16 kHz 约 −3.01 dB、20 kHz 约 −4.22 dB。现在回归已纳入仓库 `tests/mixer_tests.cpp`，不再依赖 `/tmp/partymic-frequency-*` 临时程序。

## 研究依据

- 微软 [IAcousticEchoCancellationControl](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iacousticechocancellationcontrol) 与 [参考播放端点设置](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iacousticechocancellationcontrol-setechocancellationrenderendpoint)。
- 微软 [IAudioEffectsManager](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioeffectsmanager)；文档源位于 MicrosoftDocs/sdk-api。探测工具使用官方 [Audioclient.h 声明](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/Audioclient.h) 与 ksmedia.h 的效果 GUID，补足当前 LLVM-MinGW 缺少的新接口声明。
- Xiph [SpeexDSP](https://github.com/xiph/speexdsp/tree/7a158783df74efe7c2d1c6ee8363c1e695c71226)：阅读 `include/speex/speex_echo.h`、`doc/manual.lyx`、`COPYING`，仅做研究，未引入依赖。若采用，须保存许可并验证参考信号、延迟和双讲效果。

继续使用普通用户系统音频与本程序界面 API；不增加游戏/ACE 交互，不改变默认音频设备、驱动安全设置或反作弊服务。
