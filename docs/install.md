# 安装、升级与卸载

## 安装 PartyMic

1. 在 [PartyMic 发行页面](https://github.com/Kique261/PartyMic/releases) 下载 `PartyMic-0.5.3-Setup.exe`。
2. 退出正在运行的 PartyMic，双击安装程序。
3. 按向导选择安装位置和是否创建桌面快捷方式，点击安装。
4. 从开始菜单或桌面打开 PartyMic。

安装仅对当前 Windows 账户生效，默认位置为 `%LOCALAPPDATA%\Programs\PartyMic`，无需管理员权限。支持 Windows 11 x64，不需要额外安装 C++ 运行库。

安装程序尚未进行代码签名。请确认下载来自本仓库发行页面；也可下载发行页的 `SHA256SUMS.txt`，用 PowerShell 的 `Get-FileHash` 对照校验。

## 首次安装虚拟声卡

PartyMic 需要标准 **VB-CABLE** 将混音提供给语音软件。电脑已有 `CABLE Input` 和 `CABLE Output` 时可跳过此步骤。

1. 打开 [VB-Audio 官方 VB-CABLE 页面](https://vb-audio.com/Cable/)，下载 Windows 驱动包。
2. 解压官方驱动包，以管理员身份运行 `VBCABLE_Setup_x64.exe`。
3. 按官方安装程序提示完成安装，并重启 Windows。
4. 打开 PartyMic，刷新设备，确认能选择 `CABLE Input`。

VB-CABLE 属于 VB-Audio 的 donationware，授权及捐赠说明以其官网为准。PartyMic 不附带该驱动，安装向导中的官网链接只会打开浏览器。安装 PartyMic 本身不需要联网；下载驱动需要联网。

不要将 Windows 的默认播放或默认通信播放设备设置为 CABLE Input。它们应保持为实际耳机或音响；语音软件的麦克风才选择 CABLE Output。

## 升级

退出 PartyMic 后运行新版 Setup EXE，选择原安装目录即可更新。安装版会沿用此前的安装位置；便携版文件夹可自行保留作为备份。

升级不会删除桌面的 `PartyMic录音` 文件夹。旧便携版快捷方式不会自动移除，请使用新建的安装版快捷方式。

## 卸载

打开 Windows“设置 → 应用 → 安装的应用”，找到 **PartyMic** 并选择卸载；也可使用开始菜单中的卸载入口。

卸载前请退出 PartyMic。卸载会移除程序、安装版快捷方式和应用列表入口，保留录音及用户自行放入安装目录的其他文件。VB-CABLE 不会随 PartyMic 卸载，如需移除，请使用 VB-Audio 官方卸载流程。

下一步：[开始混音与录音试听](usage.md)。
