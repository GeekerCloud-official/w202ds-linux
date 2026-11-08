# VPU 驱动工程

本目录直接维护 W202DS 当前使用的驱动和公共头文件。

| 源码 | 职责 | 输出模块 |
| --- | --- | --- |
| `w202_vpu_power.c` | VPU 电源域管理 | `w202_vpu_power.ko` |
| `w202_vpu_iommu.c` | 三核心 VAU 页表、映射和 runtime PM | `w202_vpu_iommu.ko` |
| `w202_vpu_codec_main.c`、`w202_vpu_buffers.c` | `/dev/sprd_vsp`、IRQ、任务生命周期和 DMA-BUF | `w202_vpu_codec.ko` |
| `w202_vpu_ranges.c` | 原生作业的 SG 物理范围校验，防止不同导出对象别名绕过输入／输出／参考隔离 | 编入 codec 和 map-test |
| `w202_vpu_h264.c`、`w202_vpu_h264_cabac.h` | 标准 H.264 controls 到寄存器的实验后端，Baseline/Main/High IDR/P、Main/High 参考／非参考 B、spatial／temporal direct、加权、每列表至多 16 项短期参考、CAVLC/CABAC、8×8 和 scaling | 编入 codec |
| `w202_vpu_video.c`、`w202_vpu_native.h` | V4L2/vb2/Media Request 和共享硬件资源接口，完整活动列表引用与 DMA 映射 | 编入 codec；V13 Request 及 B 站 High 验证通过 |
| `tests/` | 寄存器探针、DMA 和缓冲映射诊断 | `w202_vpu_probe.ko`、`w202_vpu_dma_test.ko`、`w202_vpu_map_test.ko` |

公共驱动接口在 `w202_vpu.h`，缓冲接口在 `w202_vpu_buffers.h` 和
`w202_vpu_buffer_uapi.h`。诊断模块属于显式测试目标；Server 默认使用电源、IOMMU 和 codec 三件模块。

## 构建

在仓库根目录使用匹配的内核构建输出。以下 Kbuild 命令先构建同版本 M2M helper，
再构建 VPU 驱动及诊断模块，输出目录需使用绝对路径：

```sh
KDIR=/path/to/matched/kernel/output
OUT=/path/to/vpu/modules
mkdir -p "$OUT/m2m" "$OUT/driver"
make -C "$KDIR" M="$PWD/kernel/drivers/media/v4l2-core" \
  MO="$OUT/m2m" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 \
  CONFIG_V4L2_MEM2MEM_DEV=m v4l2-mem2mem.ko
make -C "$KDIR" M="$PWD/Devices/vpu/source" \
  MO="$OUT/driver" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 \
  KBUILD_EXTRA_SYMBOLS="$OUT/m2m/Module.symvers" modules
```

`../build.mk` 声明模块依赖和独立目标。输出放入指定的 `OUT`，不写入源码目录。
codec、map-test 和 DMA-test 会争用同一硬件，运行时需要分别测试。
codec 新增 `v4l2-mem2mem` 和既有 videodev/vb2-vmalloc 依赖；独立 M2M helper
目标由 `build.mk` 从本树同版本 media 源码构建，不修改匹配内核的配置与符号基线。

## 验证范围

2026-11-05 的 Server 测试已通过 H.264 / AVC 解码，共 450 帧（含 1080p）逐帧匹配软件参考，
空闲后可进入 suspended。该路径依赖本机原厂专有用户态 AVC 库。

V13 在 Chrome 157 的 B 站匿名 360p High 上连续硬解约 62 秒：VaapiVideoDecoder、
平台标志 true、呈现 1864 帧且丢帧 0，VPU 作业／IRQ 各增 1880；随后网站登录窗暂停。
抓取的 65 秒原码流 1950 帧与软件 NV12 逐帧一致；默认 AV1 仍是 Dav1d 软件解码。
后续在两条 B 站视频观测到 1920×1080 High 硬解，各 25 秒采样增加 750 帧且无新增丢帧；
DASH 720p30／1080p30 及切清晰度、前后 seek、暂停恢复通过。
用户仍反馈不流畅，流畅度未验收；Mux HLS 样本遇到非 IDR I 后失败，不能据此认定
1080p60 吞吐通过。Chrome/Firefox 的 Baseline/Main 独立配置短测为 V9 历史结果；
视频编码、其他格式及长时稳定性仍待完成。
用户态源码在 [`../userspace/source/`](../userspace/source/) 维护；专有运行包、厂商参考
和历史移植记录保留在本地。

## V4L2 适配

实现和架构审计见 [v4l2.md](v4l2.md)。2026-11-08 在同一启动临时加载
`w202ds-vpu-codec-v13-v4l2-multiref`，新增显式／隐式加权、多活动参考、参考 B 和 temporal direct。
新增多参考 576 帧和加权 72 帧匹配软件参考，7 类多参考和 4 类加权负例通过；
92 项 Request（520 帧）及 FFmpeg VA-API 九组 65 帧回归通过。
精确范围、模块身份及本轮未重跑的历史项目见 [validation-v13.json](validation-v13.json)。
V10 的 High／scaling 历史记录见 [validation-v10.json](validation-v10.json)。
后续网站播放和帧间隔采样见 [validation-v13-playback.json](validation-v13-playback.json)，
失败样本与未完成的流畅度验证单独保留；这些测试没有修改 V13 驱动或桥接库。

以下为 V9 的历史验证：2026-11-07 正常重启后临时加载
`w202ds-vpu-codec-v9-v4l2-poc-origin`，32 项标准 Request 全部通过，覆盖
IDR/IP/IPB、MMAP/DMA-BUF、首次解码前 EXPBUF、flat matrix、参考和 controls 负例、
SG 物理别名拒绝、排队关闭、三个 CAPTURE 复用、POC/frame_num 回绕及正负共同 POC 原点。
作业使用原生参数后端，不依赖原厂 AVC 库。实际 AXI DMA 活跃期间取消后，复位、映射和
电源释放及全新 Request 的 1080p 像素恢复也已通过。

FFmpeg 经独立 VA-API→Request 桥，强制硬件帧后 hwdownload 为 NV12；Baseline IP、
Main/CABAC IP、Main IPB 和 1080p Main IPB 四组共 32 帧全部逐字节匹配软件参考。
每组 native_started 与完成 IRQ 各增加 8，结束后 opens/maps/powered/native_inflight 为 0，
没有新增 VPU 超时、故障或 quarantine。Chrome 157 与 Firefox ESR 153 均完成 Baseline、Main IPB、HTML video 三组测试，
每组实际作业／IRQ 各增加 8，完成队列中的 24 帧原始 NV12 均匹配软件参考。
Chrome 呈现短测 total=8、dropped=1，Firefox total=8、dropped=0；不是长时性能验收。
两者使用专用 profile 和进程级沙箱调试例外，Firefox 另需匹配的 Mesa GLES 符号绑定。
WebCodecs GPU 读回不与原始 NV12 位相等：Chrome 有转换差异，Firefox 导出 BGRX，
这些读回结果单独保留，没有标为像素验证通过。

V8 补齐 Request OUTPUT 必需的 buf_out_validate，ANY field 归为逐行 NONE；
V9 接受客户端添加的共同正负 POC 原点，IDR 的 top/bottom POC 相等且 frame_num 为 0，
并对所有帧写入当前 POC。输入 BSM padding 只在 SG 物理别名校验通过后写入。
首次 V3 的 S_FMT 空 priv Oops 和 V7 缺回调的 vb2 WARNING 是已保留的历史失败，
当前新 boot 的 V9 已能正常使用视频节点；不能再套用旧 boot 的禁止探测状态。

当前支持 progressive 8-bit 4:2:0、Baseline/Main/High、单 slice IDR/P/B、多短期参考和加权，
Main/High B 支持参考／非参考及 spatial／temporal direct；High 支持 8×8 和自定义 scaling。
控制上限每列表 16 项，本轮像素覆盖 L0 16 项／L1 2 项。多 slice、field、long-term、
非 IDR I 和 10-bit 仍拒绝。既有私有资源桥的 IDR、IP 和 IPB 像素结果
保留为对照；450 帧原厂路径测试与本轮标准接口测试分别记录。

V9 候选、源码输入哈希和脚本已保存到研究项目
`archive/linux/vpu-v4l2-20261107-v2/evidence/request-lifecycle-v9/`；
32 项、取消恢复和 FFmpeg 成功报告在归档 `evidence/primary/`，
统一验收报告为 `evidence/qualification-v9.json`。设备测试目录和独立浏览器 profile
已清理；本轮内存为 V13，生产磁盘仍为 V2，重启后回到 V2，A/B 固件尚未替换。
