# w202ds-vpu-driver（视频编解码器）

```text
vpu/
├── README.md
├── build.mk                         # 独立模块目标和依赖
├── devicetree/w202ds-vpu.dtsi        # 两个编码核心、一个解码核心及电源域
├── source/                          # 当前内核驱动
│   ├── w202_vpu_power.c             # 电源域
│   ├── w202_vpu_iommu.c             # VAU IOMMU
│   ├── w202_vpu_codec_main.c        # 私有 /dev/sprd_vsp、IRQ、reset
│   ├── w202_vpu_buffers.c           # DMA-BUF 引用和映射
│   ├── w202_vpu_ranges.c            # 原生作业 SG 物理范围别名校验
│   ├── w202_vpu_h264.c             # Baseline/Main/High、受限 I/P/B、8×8 和 scaling
│   ├── w202_vpu_h264_cabac.h        # 标准 CABAC 初始化数据，按硬件顺序排列
│   ├── w202_vpu_video.c             # V4L2/vb2/Request 实验前端
│   ├── w202_vpu_native.h            # 共享作业、DMA 导入和复位接口
│   ├── w202_vpu*.h                  # 公共头文件及私有缓冲 ABI
│   ├── v4l2.md                      # 标准接口适配路线和控制模型
│   └── tests/                       # 内核诊断模块
├── userspace/
│   ├── README.md                    # 用户态源码说明
│   └── source/                      # AVC worker、测试、解析和桥的本地补丁
├── reference/                       # 本地：厂商参考、移植历史和 ABI 分析
├── modules/                         # 本地：历史模块及构建证据
├── evidence/                        # 本地：原厂设备树证据
└── device.json、SOURCES*.json        # 本地：设备清单和来源身份
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| H.264 / AVC 硬件解码 | 已验证 | 450 帧（含 1080p）逐帧匹配软件参考（2026-11-05 验证）；依赖设备原厂专有用户态库。 |
| 原生 H.264 参数后端 | IDR/P 及受限 B 帧通过 | 2026-11-07，7 个独立 IDR、7 组 80 帧 Baseline IP、9 组 96 帧 Main/CABAC 及 8 组 88 帧 IPB（含 36 帧非参考 spatial-direct B）均逐字节匹配软件参考；不加载原厂库。覆盖 1080p、裁剪、QP／去块偏移、三个 cabac_init_idc、POC 和 frame_num 回绕。 |
| V4L2 Request 前端 | 受限 H.264 High 通过 | V10 的 92 项测试通过，含 84 项像素和 8 项拒绝，520 帧匹配软件 NV12；覆盖 8×8、非对称矩阵、SPS/PPS fallback、1080p、MMAP/DMA-BUF 和三缓冲复用。V9 的取消／SG 别名结果另存。 |
| VA-API 桥 | FFmpeg High 硬解通过 | V10 声明 Baseline/Main/High，屏蔽 High10；9 组 65 帧（含 High 8×8、自定义矩阵、1080p）逐字节匹配 NV12，作业／IRQ 与帧数对应，结束后资源归零。浏览器历史短测为 V9 Baseline/Main。 |
| KDE 下 H.264 硬解 | 短测通过 | 2026-11-06 在 KDE 标准版解码 60 帧 640×360，逐帧匹配软件参考；零超时 / 故障，结束后 suspended。Chrome 视频播放不代表已使用此 VPU。 |
| 解码后空闲省电 | 已验证 | 测试结束后进入 suspended；尚未覆盖长时间播放及反复启停。 |
| 普通播放器与浏览器硬件解码 | 受限 H.264 短测通过 | FFmpeg 32 帧，Chrome 157 与 Firefox ESR 153 各三组 24 帧原始 NV12 匹配软件参考；两浏览器均有实际 VPU 作业／IRQ 证据。使用独立 profile、VA-API 桥与进程级沙箱调试例外，长期和默认配置未验收。 |
| Chrome B 站播放 | 软件解码短测通过，硬解未通过 | 2026-11-07 在 V9 下复测：AV1／H.264 High 分别使用 Dav1dVideoDecoder／FFmpegVideoDecoder，平台解码 false，VPU 作业／IRQ 增量为 0。V10 已补受限 High，但该网站实际码流及浏览器链路尚未复测。 |
| 视频编码 | 待验证 | 尚无编码功能验收结果。 |
| 其他视频格式 | 待验证 | 当前结果只覆盖 H.264 / AVC。 |

更新日期：2026-11-08（UTC）

`source/` 根目录直接放当前驱动、公共头文件和 Makefile；`source/tests/` 放内核诊断代码。
电源、IOMMU、codec 三件模块的职责和构建入口见 [源码说明](source/README.md)。
用户态源码见 [userspace/README.md](userspace/README.md)；专有运行库、运行包和外部
`libva-v4l2_request/` 依赖保留在本地。

已有原厂解码链为 `H.264 → Bionic worker → 原厂 AVC 库 → 私有 ioctl/MMIO → VPU`。
新 codec 已含 V4L2、vb2 和 Request API 实验前端，复用原私有接口的硬件独占、
runtime PM、复位和 DMA 隔离。2026-11-07 的独立 Baseline/Main 单 slice IDR/P，
以及 Main 单 L0/L1 短期参考、非参考 spatial-direct B 已通过原生真机像素测试。
V9 的 32 项标准 Request、SG 别名拒绝、实际 DMA 取消／恢复及 FFmpeg 四组 32 帧均通过。
本轮临时加载 V10，新增 High／8×8 和自定义 scaling；92 项 Request 和 FFmpeg 9 组 65 帧通过。
V8 补 Request 必需的输出校验回调，V9 支持客户端共同 POC
原点，并在物理别名检查后才写输入 padding。标准链路为
`FFmpeg → VA-API 桥 → V4L2 Request → 原生 H.264 参数后端 → VPU`，无需原厂 AVC 库。
Chrome／Firefox 独立 profile 下各三组 24 帧硬解原始输出通过；Chrome 呈现短测丢 1 帧，
Firefox 呈现 8 帧、丢帧 0。Firefox 需要进程内绑定匹配 Mesa GLES 库。浏览器 GPU
读回带转换，原始像素在完成队列中保存并核对，详见适配文档。temporal direct、加权预测、
参考 B、多活动参考、多 slice、field、10-bit 和其他 chroma format 尚不支持。
具体分步目标、限制及验证条件见 [V4L2 适配路线](source/v4l2.md)。

历史源码、候选与证据位于只读研究归档 `archive/linux/vpu-v4l2-20261107-v2/`，
统一报告为 `evidence/qualification-v9.json`。测试 profile 和临时目录已清理。
当前 V10 仅临时加载在内存，磁盘仍为 V2，重启后回到 V2；尚未持久安装。
V10 用例及模块／桥身份见 [validation-v10.json](source/validation-v10.json)。
B 站复测的截图、播放器后端和 VPU 计数另存于研究项目
`archive/linux/vpu-bilibili-20261107-v1/`；不能把本地受限码流的硬解结果套用到该网站视频。
