# Mac Fan Controller

Mac Fan Controller 是一个原生 macOS 风扇监测和控制工具。

- 使用 SwiftUI 提供滑条控制，从 `AUTO` 到最高目标转速。
- 自动检测当前 Mac 的型号、平台、风扇数量和可用控制策略。
- 显示风扇转速、目标转速、温度和其他可读取的 SMC 参数。
- 默认开启“退出时恢复 AUTO”，退出应用时把风扇交还给系统。
- 提供 universal2 构建，支持 Apple Silicon 和 Intel Mac。

## 安装

从 GitHub Release 下载 `MacFanController-<version>-Installer.zip`，解压后双击 pkg 安装。详细步骤见 [docs/INSTALL.md](docs/INSTALL.md)。

## 构建

```sh
make build
```

生成：

- `build/Mac Fan Controller.app`
- `dist/MacFanController-<version>.pkg`

生成完整发布包：

```sh
make package
```

生成：

- `dist/MacFanController-<version>-Installer.zip`

完整校验：

```sh
make verify
```

默认构建 universal2，包含 `arm64` 和 `x86_64`。如需单架构构建：

```sh
ARCHS=arm64 make package
ARCHS=x86_64 make package
```

## 命令行

安装后可以查询状态：

```sh
/Library/PrivilegedHelperTools/com.codex.macfanctl status
```

恢复系统自动控制：

```sh
/Library/PrivilegedHelperTools/com.codex.macfanctl auto
```

设置百分比，`0` 等同于 `AUTO`，`100` 等同于最高：

```sh
/Library/PrivilegedHelperTools/com.codex.macfanctl set-percent 60
/Library/PrivilegedHelperTools/com.codex.macfanctl max
```

## 型号支持

设计目标是覆盖 2015 至今有风扇的 Mac：MacBook Pro、带风扇的 Intel MacBook Air、Mac mini、iMac、iMac Pro、Mac Studio 和 Mac Pro。无风扇机型会自动显示为只读或无风扇。

型号覆盖说明见 [docs/MODEL_SUPPORT.md](docs/MODEL_SUPPORT.md)。
