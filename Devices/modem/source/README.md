# 基带 Linux 模块源码

`sprd_modem/` 来自社区 [dikeckaan/mu300-linux](https://github.com/dikeckaan/mu300-linux/tree/47131da85865395179d08b72713a62e7829826ab) 的 `modules/sprd_modem/`，基线为 `47131da85865395179d08b72713a62e7829826ab`。
它包含原厂衍生的 SIPC、SIPA、Trusty、mailbox 和 modem loader。各源文件保留原作者及许可证；本目录同时保留 GPL 文本和上游仓库许可证。

## W202DS 改动

2026-11-06 核对：导入基线共 105 个文件，98 个保持一致，7 个有 W202DS 改动。
上游原样导入单独记录为主仓库提交 `bb7ca0a`，现有适配另行提交，便于查看真实增量。

| 文件 | 改动 |
| --- | --- |
| `sprd_modem/Kbuild` | 通过显式 `WCN_BSP_DIR` 使用共享兼容头，适应硬件目录拆分。 |
| `sprd_modem/modem_loader/sprd_modem_loader.c`、`.h` | 改为按进程维护读写锁所有权，处理进程退出与资源释放，保留原厂 loader 使用的接口。 |
| `sprd_modem/sipa_delegate/sipa_delegate.c` | 将本机 DT 未提供的旧资源设为可选，正确返回 DT 解析错误，并收敛不支持的解绑入口。 |
| `sprd_modem/sipc/smem.c` | 修正非 RAM 共享 SRAM 的映射与释放，避免缓存属性使 PM 固件内容损坏、校验失败。 |
| `sprd_modem/sipc/spipe.c` | 修正驱动与设备类的退出释放顺序。 |
| `sprd_modem/trusty/trusty-pm.c` | 适配 Linux 6.18 的 syscore 注册接口与恢复回调。 |

在主仓库根目录查看改动：

```sh
git diff bb7ca0a..HEAD -- Devices/modem/source/sprd_modem
```

`boot_prepare/` 是本项目新增的 Linux 启动准备模块：`w202_cp_prepare.c` 复现 CP RAM/PHY 电源和复位步骤，`w202_avb_seed.c` 按原厂 ABI 提交匹配的 AVB 输入。实现和使用限制见其 [README](boot_prepare/README.md)。

## 构建输入与输出

完整内核和独立模块在 CM2 构建。从主仓库根目录使用外层入口：

```sh
export LOCALVERSION=
make -C Devices KDIR=/matched/kernel/output OUT=/new/output w202ds-modem-driver
```

输入为匹配的 Linux 6.18.35 内核输出、模块源文件和外层传入的 `WCN_BSP_DIR` 兼容头；输出为独立 `.ko`、符号表及构建回执。依赖顺序由外层 `Devices/Makefile` 和各硬件的 `build.mk` 管理，构建不自动安装或加载模块。

## 固件边界

`../firmware/stock-partitions/` 的六个原厂基带、PHY 和 PM 镜像与原始备份逐文件 SHA256 一致，没有修改内部固件代码。
`../firmware/boot-prepare/` 另外保存 AVB 准备输入；其中 vbmeta 内存副本的 flags 按原厂 LK 流程还原，详情见 [说明](../firmware/boot-prepare/README.md)。这些输入与 Linux 驱动改动分别维护。
