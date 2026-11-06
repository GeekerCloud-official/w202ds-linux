# W202DS Linux 6.18.35

W202DS Linux 6.18.35 内核、板级设备树及硬件模块源码。
本仓范围为 Linux，不包含 U-Boot、EDK2/UEFI 或 LK 工程及刷写包。

本 Git 仓库根目录为 `pushup/w202ds-linux/6.18.35/`，工作分支为 `6.18.35`。

## 目录结构

```text
6.18.35/                  # Git 根目录，分支 6.18.35
├── .gitmodules
├── kernel/               # Linux stable 子模块，W202DS 本地分支
├── devicetree/
├── Devices/
├── config/
└── integration/
```

| 目录 | 用途 |
| --- | --- |
| `kernel/` | 基于官方 v6.18.35 的内核子模块和 W202DS 适配提交。 |
| `devicetree/` | W202DS 设备树源码及相关文件。 |
| `Devices/` | 18 组硬件归属，含模块源码、DTSI、构建入口及实现/待办说明。 |
| `config/` | 最新 Server 构建使用的完整内核配置。 |

已导入 2026-11-05 Server 使用的 `6.18.35-w202ds-m23-wifi` 源码及对应模块。
包含 DRM、GPU、VPU、WCN/Wi-Fi、基带、PMIC、充电、霍尔、键盘、Sensor Hub 和历史 Linux 触控实现。
各项实际完成度见 [设备清单](Devices/README.md)，构建见 [BUILD.md](BUILD.md)。

各硬件目前发布 `devicetree/`、`README.md`、`source/`、`firmware/` 和 `build.mk`。
`device.json`、`reference/`、模块编译产物、`userspace/` 和 `SOURCES*.json` 保留在本地。
GPU 源码目前只发布已在 W202DS 上验证的 `Devices/gpu/source/panfrost/`。
设备 NV、账号密钥和完整系统 rootfs 不纳入公共源码。
Linux 树内通用 EFI 支持属于内核，正常保留。快充、Vulkan、蓝牙、KDE 等未验收能力不列为已完成。

已进行板级源码分离：实现归 `Devices/`，内核保留功能位置接入；硬件 DTSI 也随设备维护。
已取得的硬件固件放对应设备的 `firmware/`。

2026-11-05 完整构建及 Server 上机已完成：378 件树内模块、50 件独立外部模块均编译通过，运行内核 Build ID 与新产物匹配。GPU、VPU、蜂窝数据和键盘/霍尔的验证范围见 [设备清单](Devices/README.md) 中对应硬件说明。KDE 尚未启动验收。

`Devices/` 下 18 个目录已简化为硬件名称；模块名和构建目标保留原名。`config/server.config` 的下一次构建名称改为 `6.18.35-w202ds`，当前真机仍运行已验证的 `6.18.35-w202ds-m23-wifi`，未因改名再次刷写。

旧 `w202ds-research/bsp/Devices` 指向本版本的 `Devices/`；旧 `bsp/kernel` 指向本版本的 `integration/kernel/`，保留 initramfs、兼容配置、构建记录和交付包入口。实际 Linux 源码始终在本版本 `kernel/`，不混入集成脚本。

## 内核 Git

`kernel/` 保留官方 Linux stable `v6.18.35` 基线，适配在独立分支提交；主仓库仅用 submodule 固定该提交。查看差异：

```sh
git -C kernel diff --stat v6.18.35..HEAD
git -C kernel log --oneline v6.18.35..HEAD
git submodule status kernel
```

子模块初始化、当前提交和备用恢复方式见 [内核 Git 说明](KERNEL-GIT.md)。
设备组件按已确认内容在主仓库分批提交，实际入库范围以 `git ls-files Devices/` 为准。
