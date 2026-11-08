# 构建

在具备交叉工具链的 Linux 主机构建内核和模块。需要 GNU make、Python 3.11+、AArch64 GCC/binutils、flex、bison、bc、OpenSSL/ELF 开发包、dtc、kmod。以下从仓库根运行，输出全部放 `output/`。

默认主推 debug，它继承 desktop 的屏幕/键盘基础。配置合成模型与外部覆盖接口见
[config/kernel/README.md](config/kernel/README.md)，调试能力和内建清单见
[config/kernel/DEBUG.md](config/kernel/DEBUG.md)。仅生成配置时将 `build` 换成 `configure`；
`plan` 只展示合成输入，不创建输出。

```sh
export LOCALVERSION=
python3 -B scripts/kernel-build.py build --profile debug --out output/kernel-debug --jobs 4
make -C Devices KDIR="$PWD/output/kernel-debug" OUT="$PWD/output/modules-debug" all
mkdir -p output/devicetree
dtc -I dts -O dtb -o output/devicetree/w202ds-server.dtb devicetree/w202ds-server.dts
```

输入为 `kernel/` 原生完整源码、`config/kernel/` 分层配置、`Devices/*/source/` 与板级 DTS。
输出为 `output/kernel-debug/arch/arm64/boot/Image`、树内模块、`output/modules-debug/<目标>/*.ko` 及逐目标 `build.json`、DTB。
`config/server.config` 保留为历史 debug 对照；旧 LK framebuffer 诊断实现已删除。
构建不安装、不加载模块、不刷写设备。完整 `modules` 步骤生成树内导出符号，不能以 `modules_prepare` 替代。

`CONFIG_LOCALVERSION="-w202ds"` 保留板级后缀；`CONFIG_LOCALVERSION_AUTO=y` 启用
内核子模块的 Git 版本信息，源码有未提交修改时还会附加 `-dirty`。
示例中的 `LOCALVERSION=` 仅清空额外手工后缀，不会关闭自动 Git 标识；
构建外部模块时保持同一设置，以匹配内核 release。

现有 Server 配置强制使用自身命令行，入口为 `rdinit=/w202diag`，需要另行提供匹配 initramfs/rootfs。
这份配置和源码不是通用发行版安装器，也不包含任何引导固件。板级 DT 中继承的硬件/保留内存必须与启动环境一致。

desktop 基础层已保留蓝牙选项（BT、`hci_uart` H4、RFCOMM、BNEP、HIDP）。
外部工程可用重复的 `--fragment` 传入覆盖；已有蓝牙兼容片段为 `config/fragments/bluetooth.config`。

蓝牙初始化程序可独立构建、测试和暂存，不访问设备或启用服务：

```sh
make -C Devices/wcn/userspace CROSS_COMPILE= OUT="$PWD/output/wcn-bt-native" test
make -C Devices/wcn/userspace OUT="$PWD/output/wcn-bt-arm64" DESTDIR="$PWD/output/wcn-bt-rootfs" install
```

使用现有 CM2 冻结构建目录进行单模块开发时，见 [独立模块构建说明](Devices/BUILD.md) 的 cfg80211 导出选项。
构建检查与真机功能验收分别记录，不能由编译成功推断硬件完整可用。

2026-11-05 的完整构建已在 Server 上机。随后按用户要求只保留下一次构建的 `CONFIG_LOCALVERSION="-w202ds"`；当前运行版本仍为 `6.18.35-w202ds-m23-wifi`。更换 release 时还须同步 initramfs、模块目录和用户态版本约定，不能只改 Image 文件名。

基带用户态独立入口为 `make -C Devices/modem/userspace OUT=绝对输出目录 all`，不触发内核构建。目录名称已简化，内核模块目标名称不变。

## 分离后的内建与模块

`kernel/` 内 3 个功能位置的相对链接指向 `Devices/`；复制源码时保留整个 `6.18.35` 布局和链接。
早期黑框日志、背光和 USB PHY 在 debug 配置内建，模块归属不等于晚加载。
早期日志组件为 `pj4edk-boot-display`，由 `CONFIG_PJ4EDK_BOOT_DISPLAY` 控制。
旧 `/dev/w202fbcon` 接口及其实现、构建入口已删除。
背光、USB PHY 另有显式 `.ko` 编译目标，见 `Devices/BUILD.md`。

内核 Panfrost 是原版，板级版本仅在 `Devices/gpu/source/panfrost/` 维护。
安装时外部板级模块放到 `updates/w202ds/`，重建 depmod 和 initramfs 并核实解析路径；
不要只安装树内原生 `panfrost.ko`。本仓构建命令不执行安装。

标准 PMIC 重启路径、显示顺序和 USB 需要真机回归。
配套使用本仓 DTSI 编译的 DTB，不能依赖已删除的启动时 GPU FDT 修补。
