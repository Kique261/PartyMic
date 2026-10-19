# 当前开发状态与交接

更新日期：2026-10-18。源码与交叉编译候选为 0.5.1，分发包和正式验证记录仍为 0.5.0；完整测量、失败记录、最终 EXE 哈希见 [验证记录](verification.md)与[机器记录](test-results/windows-runtime.json)。0.4.0 证据见 [归档](verification-0.4.0.md)，保留旧版包和远端快捷方式用于回退。

## 最新收尾：0.5.1 闪烁修复候选

用户补充 0.5.0 仍有旧页/目标页交替闪烁，已用实际屏幕序列复现并初步修复绘制顺序。候选通过四组屏幕序列、视觉和布局，但帧间隔仍有 32–62 ms，未发布，也未重跑录音试听。本轮已按用户要求结束，远端测试任务已清理；下次开发先读 [2026-10-18 交接](handoff-2026-10-18.md)，不要自动占用 Windows。

## 0.5.0 已完成

- 修复时钟校正造成的高频衰减，完成 64 抽头窗函数 sinc；新增正负漂移、独立双声道频响与队列不连续后的历史清理回归。Fedora、Windows 核心通过。
- 新硬件测试 `windows_audio_smoke.exe --fidelity` 覆盖整个数字路径的五个频点；实际 VB-CABLE 幅度误差 <0.002 dB。Smoke、70 次启停和 90 秒 Soak 通过。
- 切页改为 200 ms 内容快照淡入，原生控件只布局一次，快速导航衔接当前画面；修复焦点通知提前取消过渡。最终主程序通过完整录音试听、布局、视觉与帧间隔测试。
- 新增只查询能力的 `windows_audio_effects.exe`：Brio 私有流在 Communications 类别下报告 AEC 开启并支持参考端点控制，Other 类别下 AEC 关闭。未向主程序加入 AEC，也未设置设备效果。

用户的发闷报告来自本程序录音回放；不能归咎于网易云源文件损坏。数字高频损失和外放被麦克风拾回可能同时存在；修复数字路径不等于回声问题全部解决。

## 仍需完成的验证与功能

1. 网易云专项：本次会话存在但音乐采集 RMS 为 0，未进入请求的 60 秒实播阶段。确认播放后重新运行 Player 模式，保留先前失败记录。
2. 真人音质：保持音乐片段、增益和试听输出一致，比较麦克风静音、外放混音和耳机混音。记录人声清晰度、音乐层次与削波；自动测试中的麦克风有活动不代表受控真人讲话通过。
3. 回声消除：可优先研究实际可用的 Windows 通信流 AEC，设置参考为真实 HDMI 输出，只处理麦克风分支。需检查系统通信压低、其他增强处理、输出设备选择、设备时钟及双讲效果；接口可用不等于效果合格。没有新增“仅音乐”录音模式，可先显式静音麦克风做对照并恢复原设置。
4. 动效：已测四次过渡的本窗口绘制计时，未测 DWM 呈现或显示器同步。切页快照准备约 62–85 ms，可继续减少。系统关闭动画、多屏 DPI 与其他刷新率仍需验收。
5. 稳定性：60 分钟、热插拔、休眠唤醒、端到端延迟、MSVC 与 CI、代码签名仍待推进。本机 sanitizer 运行库缺失，ASan/UBSan 未完成。

不要将以上项目提前标为通过。0.5.0 完成的改动与尚待确认的听感分别记录，后续新 EXE 应重新记录哈希和对应结果。

## 复现与证据

参考 [开发文档](development.md)。核心测试运行 `ctest --test-dir build --output-on-failure -V`；硬件 runner 支持 Smoke、Fidelity、Soak、Player，并同时核对退出码与完成标记。硬件与 GUI 测试在已登录实体交互桌面执行，不能用 SSH session 0 或 RDP 重定向麦克风代替。

完整证据目录：`~/.local/state/partymic/remote-20261018/v0.5.0-validation/`。`tests` 为核心/硬件日志，`gui-tests-run1` 为首个淡入候选的失败记录，`gui-tests-run2` 为最终 EXE 的 GUI 通过记录，`extra-tests` 为设备能力与网易云静音结果。实际录音只保存在 Windows 桌面文件夹，不随源码或发行包分发。

早期临时频响诊断曾测得线性插值在 16 kHz 约 −3.01 dB、20 kHz 约 −4.22 dB。现在回归已纳入仓库 `tests/mixer_tests.cpp`，不再依赖 `/tmp/partymic-frequency-*` 临时程序。

## 研究依据

- 微软 [IAcousticEchoCancellationControl](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iacousticechocancellationcontrol) 与 [参考播放端点设置](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iacousticechocancellationcontrol-setechocancellationrenderendpoint)。
- 微软 [IAudioEffectsManager](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioeffectsmanager)；文档源位于 MicrosoftDocs/sdk-api。探测工具使用官方 [Audioclient.h 声明](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/Audioclient.h) 与 ksmedia.h 的效果 GUID，补足当前 LLVM-MinGW 缺少的新接口声明。
- Xiph [SpeexDSP](https://github.com/xiph/speexdsp/tree/7a158783df74efe7c2d1c6ee8363c1e695c71226)：阅读 `include/speex/speex_echo.h`、`doc/manual.lyx`、`COPYING`，仅做研究，未引入依赖。若采用，须保存许可并验证参考信号、延迟和双讲效果。

继续使用普通用户系统音频与本程序界面 API；不增加游戏/ACE 交互，不改变默认音频设备、驱动安全设置或反作弊服务。
