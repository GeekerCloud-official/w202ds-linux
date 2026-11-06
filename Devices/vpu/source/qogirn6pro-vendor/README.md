# Qogirn6Pro VPU 原始参考

来源为 realme 官方
[AndroidT GPL 发布](https://github.com/realme-kernel-opensource/realme_C51_C53_Narzo-N53-AndroidT-kernel-source/tree/4381465ccaf87fcf3215b9cd42f4a685df40e5e0/kernel_modules/kernel5.4/video)，
固定 commit `4381465ccaf87fcf3215b9cd42f4a685df40e5e0`。原文件未改，逐文件 Git blob、
SHA256、URL 和本地位置见 ../../SOURCES-REALME-ANDROIDT.json。
sprd-vpu/ 包含核心和 ioctl UAPI，sprd-vpu-power/ 包含电源域。
文件自身 SPDX 优先，完整 GPL 文本见 [现存许可证](../vendor-reference/LICENSES/preferred/GPL-2.0)。

核心明确标注 QOGIRN6PRO，三个 compatible 与 W202DS 一致，本机 DT 版本0x15。
这仍是其他产品的官方发布，不证明与本机原厂二进制完全相同。
5.4 核心依赖 sprd_iommu、ION 内部对象和专有用户态，不能直接当作 6.18 的 V4L2 驱动。
原始电源域模块可以在 CM2 编译，但初始化/退出和共享 DPU/VPU 电源关系尚未审核完成，
不可自动加载。详见 [本轮记录](../../../../../../../w202ds-research/archive/linux/server-vpu-20261102-v1/README.md)。

后续在 [../m23](../../reference/legacy-bsp/source/m23/README.md) 单独维护的子域provider已通过三个核心的电源/VAU
只读实测；它不关闭显示共享顶层/PLL，也不改变这里的原始参考。编解码核心尚未移植完成。
