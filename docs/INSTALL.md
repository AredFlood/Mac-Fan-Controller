# Mac Fan Controller 安装和使用说明

Mac Fan Controller 是一个 macOS 风扇监测和控制工具。安装包会把应用放到 `/Applications`，并安装一个本地 helper 到 `/Library/PrivilegedHelperTools/com.codex.macfanctl`。控制风扇需要管理员权限，因为 macOS 要求读写 SMC 相关接口必须通过受权限保护的本地工具完成。

软件会自动检测当前 Mac 型号、平台、风扇数量和可用控制策略。支持目标是 2015 至今有风扇的 Mac；无风扇机型会显示为只读或无风扇。

## 安装

1. 下载并解压 `MacFanController-<version>-Installer.zip`。
2. 双击 `MacFanController-<version>.pkg`。
3. 按安装器提示输入 Mac 登录密码。
4. 安装完成后打开 `/Applications/Mac Fan Controller.app`。

如果 pkg 被 macOS 拦截，可以用终端安装：

```sh
sudo installer -pkg MacFanController-<version>.pkg -target /
open "/Applications/Mac Fan Controller.app"
```

也可以按 Apple 官方方式，在尝试打开后进入“系统设置 > 隐私与安全性”，找到被拦截的软件并选择 `Open Anyway`：

https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac

## 使用

- 滑条最左边是 `AUTO`，代表系统自动控制风扇。
- 滑条越往右，目标转速越高。
- `最高` 会直接切到最高目标转速，请短时间使用。
- `AUTO` 按钮会恢复系统自动控制。
- 默认开启 `退出时恢复 AUTO`，退出软件时会自动把风扇交还给系统。
- 下方会显示风扇实际转速、最低/最高/目标转速、温度和其他可读取的 SMC 参数。

## 安全建议

- 不确定时保持 `AUTO`。
- 不要长时间固定在最高转速。
- 如果系统拒绝控制或状态区显示错误，先点 `AUTO`，再退出软件。
- 合盖、睡眠、重启前建议确认模式是 `AUTO`。

## 卸载

先恢复自动控制：

```sh
/Library/PrivilegedHelperTools/com.codex.macfanctl auto
```

然后删除文件：

```sh
sudo rm -rf "/Applications/Mac Fan Controller.app" /Library/PrivilegedHelperTools/com.codex.macfanctl
```

## 命令行状态

```sh
/Library/PrivilegedHelperTools/com.codex.macfanctl status
```

如果输出里 `"ok": true`，说明 helper 可以正常读取风扇状态。
