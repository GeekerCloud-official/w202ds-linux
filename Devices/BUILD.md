# 独立模块构建

`Makefile` 汇总各硬件 `build.mk`；`build-module.py` 每次构建一件 `.ko`，并在编译前后核验
内核 Git/配置来源、生成文件哈希、release、符号提供者、源文件和模块格式。
工程没有对 LK、EDK2 或其他工作区目录的构建依赖。
硬件目录使用简短名称，例如 `hall/`；分组目标仍为 `w202ds-hall-driver`，独立模块目标仍使用 `w202ds-<硬件>-<子驱动>`，可通过 `list` 查看。目录改名不改变构建目标或 `.ko` 名称。

```sh
export LOCALVERSION=
make -C Devices list
make -C Devices KERNEL_OUT="$PWD/output/kernel-debug" w202ds-hall-driver
make -C Devices KERNEL_OUT="$PWD/output/kernel-debug" w202ds-drm-driver
make -C Devices KERNEL_OUT="$PWD/output/kernel-debug" all
```

没有内核输出时先按 debug 配置构建内核；已有输出必须匹配当前源码及配置，发现变化就拒绝。
`PROFILE=desktop`、`MODEL=...`、`FRAGMENTS="/first.config /last.config"` 可选择配置及外部覆盖。
也可以直接使用顶层 `python3 -B scripts/build.py`，完整参数见 [构建说明](../BUILD.md)。
`KERNEL_OUT`/兼容参数 `KDIR` 接受完整内核输出、导入根目录或其 `build/`，
模块默认输出到 `<内核输出根>/modules/<目标>/`；`OUT` 可以指定其他独立位置。

```sh
# 只允许复用已有匹配内核，不构建内核。
make -C Devices KERNEL_OUT="$PWD/output/kernel-debug" SKIP_KERNEL=1 w202ds-hall-driver

# 明确使用旧内核，跳过 Git/配置新旧检查。
make -C Devices KERNEL_OUT="$PWD/output/kernel-debug-imported" \
  SKIP_KERNEL=1 SKIP_KERNEL_CHECK=1 w202ds-hall-driver
```

跳过必须显式提供内核输出目录；`SKIP_KERNEL_CHECK=1` 同时跳过内核编译。
包内文件完整性、符号依赖和 vermagic 仍检查，收据记录是否跳过新旧检查。
只改外置模块自己的源码可以正常增量构建；改内建驱动、配置或内核源码应选择新内核输出。

每个目标输出自己的模块、`Module.symvers`、`build.log` 和 `build.json`；提供者按实际符号依赖先构建。
`all` 包含当前 Server 模块及已验证的 Linux 基带启动准备。诊断、历史触控为显式目标。
目标能独立编译不代表它能独立运行；例如 SIPC、Trusty、WCN 共享硬件状态，不能据此随意热卸载。

完整新内核先构建 `Image modules`，使用全局 `Module.symvers`，无需额外 provider。
若既有输出没有收集 cfg80211 导出，可显式使用已核对且匹配该输出的收据：

```sh
make -C Devices \
  KERNEL_OUT=/absolute/matched/imported-kernel-output \
  OUT="$PWD/output/frozen-modules" \
  SKIP_KERNEL=1 SKIP_KERNEL_CHECK=1 \
  WCN_KERNEL_PROVIDER=wcn/kernel-providers/server-20261105/cfg80211.json w202ds-wcn-wifi
```

该 provider 限定配置/全局符号表/release/实际 cfg80211.ko 的哈希；不能用于别的内核配置。
它的符号来自原 M23 私有构建，最新输出里的 cfg80211.ko 与原模块逐字节相同。
历史构建记录仅在本地保留；新构建使用实际源码、内核输出及显式指定的 provider。


额外的独立测试目标：`w202ds-drm-backlight`、`w202ds-usb3-legacy-phy`。
二者在当前 Server 内核内建；独立 `.ko` 用于验证模块模式，不能在同一内建实现上重复加载。
只有 DTSI 的硬件目录暂不注册空模块目标，构建进度以各目录 README 为准。
`w202ds-wcn-driver` 包含 `w202ds-wcn-wifi` 与 `w202ds-wcn-bluetooth`（`sprdbt_tty.ko`），二者均以 `w202ds-wcn-bus` 为符号提供者；蓝牙用户态在 `wcn/userspace/` 以 `make` 单独构建。
