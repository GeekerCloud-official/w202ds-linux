# VPU 驱动工程

本目录直接维护 W202DS 当前使用的驱动和公共头文件。

| 源码 | 职责 | 输出模块 |
| --- | --- | --- |
| `w202_vpu_power.c` | VPU 电源域管理 | `w202_vpu_power.ko` |
| `w202_vpu_iommu.c` | 三核心 VAU 页表、映射和 runtime PM | `w202_vpu_iommu.ko` |
| `w202_vpu_codec_main.c`、`w202_vpu_buffers.c` | `/dev/sprd_vsp`、IRQ、任务生命周期和 DMA-BUF | `w202_vpu_codec.ko` |
| `tests/` | 寄存器探针、DMA 和缓冲映射诊断 | `w202_vpu_probe.ko`、`w202_vpu_dma_test.ko`、`w202_vpu_map_test.ko` |

公共驱动接口在 `w202_vpu.h`，缓冲接口在 `w202_vpu_buffers.h` 和
`w202_vpu_buffer_uapi.h`。诊断模块属于显式测试目标；Server 默认使用电源、IOMMU 和 codec 三件模块。

## 构建

在仓库根目录使用匹配的内核构建输出，本地编译：

```sh
export LOCALVERSION=
make -C Devices KDIR="$PWD/output/kernel" OUT="$PWD/output/modules" w202ds-vpu-driver
```

`../build.mk` 声明模块依赖和独立目标。输出放入指定的 `OUT`，不写入源码目录。
codec、map-test 和 DMA-test 会争用同一硬件，运行时需要分别测试。

## 验证范围

2026-11-05 的 Server 测试已通过 H.264 / AVC 解码，共 450 帧（含 1080p）逐帧匹配软件参考，
空闲后可进入 suspended。该路径依赖本机原厂专有用户态 AVC 库。

通用 V4L2、播放器和浏览器接入、视频编码、其他格式及长时稳定性仍待完成。
用户态程序在本地 `../userspace/` 维护；厂商参考和历史移植记录保留在本地 `../reference/`。
