# 当前 debug 内核

debug 继承 desktop 的屏幕、键盘及正常交互基础，并保留原 `config/server.config`
的调试和诊断设置。旧 LK framebuffer 诊断实现已删除；其余配置以历史 Server
经同一 Kconfig 规范化的结果为对照。当前仅生成和验证配置，没有重新编译或安装内核。

## 调试与诊断功能

| 能力 | 当前设置与行为 |
| --- | --- |
| 早期屏幕日志 | `PJ4EDK_BOOT_DISPLAY=y`，在保留的启动画面上输出日志，供原生显示模块接管前使用。 |
| 详细启动日志 | `PRINTK_TIME=y`；强制命令行含 `loglevel=7`、`log_buf_len=4M`，systemd 日志送入 kmsg。 |
| 查询完整配置 | `IKCONFIG=y`、`IKCONFIG_PROC=y`，支持从 Image 提取配置及 `/proc/config.gz`。 |
| debugfs | `DEBUG_FS=y`，提供驱动与内核调试文件系统；具体节点由相应驱动提供。 |
| SysRq | `MAGIC_SYSRQ=y`，含串口 SysRq 支持。 |
| 电源调试 | `PM_DEBUG=y`、`PM_ADVANCED_DEBUG=y`；当前 `SUSPEND=n`，不是已支持休眠恢复的声明。 |
| 锁死检测 | 软锁死检测与 buddy 方式硬锁死检测；默认没有启用“检测到锁死就 panic”的开关。 |
| 符号与栈信息 | `KALLSYMS=y`、`STACKTRACE=y`；没有 `KALLSYMS_ALL`。 |
| 崩溃日志设施 | 共同层的 `PSTORE/CONSOLE/PMSG/RAM=y`；实际保存依赖匹配的后端和保留内存。 |
| 内存基础检查 | Kconfig 默认的 `SLUB_DEBUG=y`、`DEBUG_MEMORY_INIT=y` 等；`SLUB_DEBUG_ON=n`，不会默认开启所有 SLUB 检查。 |
| 诊断恢复 | `W202DS_DIAG_RECOVERY=y`，提供 `/dev/w202diagctl`。debug 初始计时 120 秒；有权限的用户态可取消，或重新设置 30 秒恢复。 |
| 诊断入口 | `CMDLINE_FORCE=y`，`rdinit=/w202diag`；需要配套诊断 initramfs。命令行 `panic=10` 覆盖编译默认 `PANIC_TIMEOUT=5`。 |

当前未开启 ftrace/function tracer、Kprobes、KGDB、动态调试、DWARF 调试信息、lockdep/
PROVE_LOCKING、KASAN/KCSAN/UBSAN。`DEBUG_KERNEL=y` 不代表这些功能都已启用。

## W202DS 相关内建功能

| 功能 | 选择 | 所属层 |
| --- | --- | --- |
| 早期屏幕日志 | `CONFIG_PJ4EDK_BOOT_DISPLAY=y` | debug 增量 |
| PWM 背光控制 | `CONFIG_W202DS_BACKLIGHT=y` | 板级，共同能力 |
| USB2 PHY 兼容 | `CONFIG_USB_W202DS_DUMMY_PHY=y` | 板级，共同能力 |
| DRM/KMS 公共辅助能力 | `CONFIG_DRM_W202DS_BRINGUP_HELPERS=y` | 板级，共同能力；此项选择框架依赖，不是整条显示驱动 |
| 诊断恢复接口与恢复实现 | `CONFIG_W202DS_DIAG_RECOVERY=y` | 板级，共同能力；desktop 初始计时为 0 |

旧 `/dev/w202fbcon` 接口及其实现、配置和构建入口已删除；维护中的 initramfs
没有调用它。当前面板模块使用独立 DRM 路径，普通 framebuffer/DRM fbdev 支持继续保留。
DRM 前的早期日志由 `Devices/drm/source/pj4edk-boot-display/` 提供。

## 内建的平台与基础驱动

| 类别 | 当前内建内容 |
| --- | --- |
| 时钟与安全服务 | UMS9620 时钟、SPRD SIP 服务。 |
| 引脚与总线 | QOGIRN6PRO pinctrl、SPRD GPIO/EIC/PMIC EIC、I2C、SPI、ADI、UART 和串口控制台、PCIe。 |
| 电源域与同步 | UMS9620 IPA 电源域、SPRD 硬件自旋锁。 |
| PMIC 与电源 | SC27xx MFD/ADI 框架、UMP9620 稳压器、SC27xx ADC/RTC、SPRD/SC27xx efuse；PMIC watchdog 对象由现有 MFD 选择带入。 |
| 存储 | SPRD SDHCI/eMMC、块设备基础、USB 存储/UAS。 |
| USB | DWC3/Gadget、xHCI、UMS9620 PHY，以及 ACM、NCM、ECM、RNDIS、Mass Storage、FunctionFS。 |
| 显示与输入基础 | DRM/KMS、fbdev/console 公共设施、背光类、PWM、输入子系统、evdev、UART 键盘所用串口。 |
| 温度与灯 | R5P0 thermal；GPIO、SC27xx BLTC/KPLED、AW9523B、ZTE LDO LED 驱动保留原选择。配置启用不代表全部已实机验收。 |
| 文件系统 | EXT4、VFAT、exFAT、SquashFS、tmpfs 等，另有多种文件系统配置为模块。 |

## 配套模块

正常交互需要内核 Image 和配套模块共同组成，单独生成 `.config` 不会构建或安装这些模块。

| 功能 | 配套模块或构建方式 |
| --- | --- |
| 面板与显示 | `w202_r6p0.ko`，由 `Devices/drm/build.mk` 构建。 |
| 键盘 | `w202_zsk.ko`，由 `Devices/keyboard/build.mk` 构建。 |
| GPU | 树内 `DRM_PANFROST=m`；本板实际安装板级 `panfrost.ko` 与 `w202_gpu_support.ko`，由 Devices 构建。 |
| Wi-Fi/蓝牙 | `CFG80211=m`、`BT=m`、HCI UART 等树内模块，以及板级 `wcn_bsp.ko`、`sprd_wlan_combo.ko`、`sprdbt_tty.ko`。 |
| 霍尔、VPU、基带、传感器 | 通过各自的 `Devices/*/build.mk` 构建；外部模块目标不是把所有 CONFIG 改成 y/m 就能转换。 |
| 电池监测、Type-C、充电控制 | 相应板级外部模块；其底层 PMIC、regmap、ADC 等支撑为内建。 |

当前 `SOUND=n`，没有开启完整音频链路。触控、摄像头等代码或参考资料存在不等于默认构建和验收通过。
完整驱动构建方式见 [Devices/BUILD.md](../../Devices/BUILD.md)。

生成报告中的 y/m/n 数量是 Kconfig 选项数量，不是独立驱动数量。
例如 `BT_BREDR=y` 是 `BT=m` 的子功能，不能据此把蓝牙整体算成内建驱动。
