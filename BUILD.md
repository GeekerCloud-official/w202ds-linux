# 构建

在具备交叉工具链的 Linux 主机构建内核和模块。需要 GNU make、Python 3.11+、AArch64 GCC/binutils、flex、bison、bc、OpenSSL/ELF 开发包、dtc、kmod。以下从仓库根运行，输出全部放 `output/`。

默认主推 debug，它继承 desktop 的屏幕/键盘基础。配置合成模型与外部覆盖接口见
[config/kernel/README.md](config/kernel/README.md)，调试能力和内建清单见
[config/kernel/DEBUG.md](config/kernel/DEBUG.md)。仅生成配置时将 `build` 换成 `configure`；
`plan` 只展示合成输入，不创建输出。

```sh
export LOCALVERSION=
python3 -B scripts/build.py --profile debug --kernel-out output/kernel-debug --jobs 4 all
mkdir -p output/devicetree
dtc -I dts -O dtb -o output/devicetree/w202ds-server.dtb devicetree/w202ds-server.dts
```

输入为 `kernel/` 原生完整源码、`config/kernel/` 分层配置、`Devices/*/source/` 与板级 DTS。
输出为 `output/kernel-debug/arch/arm64/boot/Image`、树内模块、`output/kernel-debug/modules/<目标>/*.ko` 及逐目标 `build.json`、DTB。
`config/server.config` 保留为历史 debug 对照；旧 LK framebuffer 诊断实现已删除。
构建不安装、不加载模块、不刷写设备。完整 `modules` 步骤生成树内导出符号，不能以 `modules_prepare` 替代。

`scripts/build.py` 默认选择 debug：没有内核输出时先构建 `Image modules`，再编译外置模块；
已有输出时核验后复用。发现内核 Git 提交或工作区修改、内建驱动链接目标、配置模型/片段、
release 或已准备文件变化时拒绝，要求换新输出目录。模块自身源码可以修改并正常重编。
完整内核输出额外保存 `kernel-state.json`，导出时将这份来源信息带入压缩包。

```sh
# 只编霍尔模块；匹配的内核不存在时先编内核。
python3 -B scripts/build.py --kernel-out output/kernel-debug w202ds-hall-driver

# 强制跳过内核编译；目录必须已经存在，仍检查源码和配置是否匹配。
python3 -B scripts/build.py --skip-kernel --kernel-out output/kernel-debug w202ds-hall-driver

# 明确沿用旧内核，跳过 Git/配置新旧检查；该选项同时跳过内核编译。
python3 -B scripts/build.py --skip-kernel-check \
  --kernel-out output/kernel-debug-imported w202ds-hall-driver
```

两个跳过选项都要求显式指定 `--kernel-out`。它可以是完整 Kbuild 输出，也可以是
导入目录根部或其中的 `build/`；脚本自动定位编译文件，外置模块默认放在根部 `modules/`。
`--modules-out` 可以另选模块输出。`--model`、`--profile` 和重复的 `--fragment` 保留多级合并接口。
即使使用 `--skip-kernel-check`，仍检查包内文件哈希、release、模块符号依赖与 vermagic，
拒绝配置/生成头文件被修改后混用。输出记录会标明 `kernel_freshness_checked=false`。
沿用旧内核不会获得新配置功能，例如旧输出的 `CONFIG_CPU_FREQ=n` 不会因此变成支持 CPU 调频。
没有来源记录的早期导入包需要重新构建/导出，或显式选择跳过新旧检查。

`CONFIG_LOCALVERSION="-w202ds"` 保留板级后缀；`CONFIG_LOCALVERSION_AUTO=y` 启用
内核子模块的 Git 版本信息。原生 Kbuild 会对未提交修改附加 `-dirty`；
本项目默认构建入口会在构建或复用前拒绝脏的内核 Git 工作树和 `-dirty` release。
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

`kernel/` 内 4 个功能位置的相对链接指向 `Devices/`；复制源码时保留整个 `6.18.35` 布局和链接。
早期黑框日志、背光、USB PHY 和恢复接口在 debug 配置内建，模块归属不等于晚加载。
早期日志组件为 `pj4edk-boot-display`，由 `CONFIG_PJ4EDK_BOOT_DISPLAY` 控制。
旧 `/dev/w202fbcon` 接口及其实现、构建入口已删除。
背光、USB PHY 另有显式 `.ko` 编译目标，见 `Devices/BUILD.md`。

内核 Panfrost 是原版，板级版本仅在 `Devices/gpu/source/panfrost/` 维护。
安装时外部板级模块放到 `updates/w202ds/`，重建 depmod 和 initramfs 并核实解析路径；
不要只安装树内原生 `panfrost.ko`。本仓构建命令不执行安装。

默认诊断配置保留 120 秒恢复计时；普通系统可设 `CONFIG_W202DS_DIAG_BOOT_TIMEOUT=0`，
保留 `/dev/w202diagctl` 用户态兼容。新的标准 PMIC 重启路径、显示顺序和 USB 需要真机回归。
配套使用本仓 DTSI 编译的 DTB，不能依赖已删除的启动时 GPU FDT 修补。

## 导出内核输出供另一台机器编译模块

完整内核构建成功后，可导出 `Image` 与外置模块所需的已准备输出；包中保留
`.config`、完整 `Module.symvers`、生成头文件和必要 Kbuild 工具，不包含已编译的
`.ko`、普通 `.o`、内核链接中间产物或日志。复制压缩包后，导入时绑定本地相同提交的
干净 `kernel/` 源码，修正源码链接与构建入口；无需重新构建内核。

```sh
# 在完成内核构建的机器导出；压缩包路径可以自行指定。
python3 -B scripts/kernel-export.py --out output/kernel-debug \
  --kernel kernel --archive output/w202ds-kernel-debug.tar.gz

# 将压缩包传到本地，然后从本地仓库根目录导入到新目录。
python3 -B scripts/kernel-import.py --archive output/w202ds-kernel-debug.tar.gz \
  --kernel kernel --out output/kernel-debug-imported

python3 -B scripts/build.py --skip-kernel --kernel-out output/kernel-debug-imported \
  w202ds-hall-driver
```

导出、导入都会校验内核提交、配置和符号表；导入会核验全部文件哈希，拒绝覆盖已有
输出目录。`Image` 位于导入目录根部，模块使用其中 `build/` 作为 `KDIR`。
`scripts/build.py` 与 `Devices/` 汇总入口在每次模块编译前后重新检查来源和文件完整性；
直接调用原生 `make -C KDIR M=...` 会绕过这些项目检查。
本地仍需兼容的交叉工具链、GNU make、Python 3.11+ 和 kmod；包中的 Kbuild 工具要求
主机操作系统与 CPU 架构和导出机一致。当前入口面向 arm64 的普通 C 模块配置；
Rust、模块 BTF 或 objtool 启用时会拒绝导出，以免漏掉所需编译文件。
外置模块源码修改可复用这一输出；更改内核配置或内核接口后，重新构建并导出新的包。
流程参考 [Linux 外置模块构建说明](https://docs.kernel.org/kbuild/modules.html)。
