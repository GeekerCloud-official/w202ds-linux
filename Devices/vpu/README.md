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
│   ├── w202_vpu_h264.c             # Baseline/Main/High、I/P/B、加权、多参考、8×8 和 scaling
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
| 原生 H.264 参数后端 | 受限 IDR/P/B 通过 | V13 补齐显式／隐式加权、多活动短期参考、参考 B 和 temporal direct；像素覆盖 L0 16 项、L1 2 项。原生路径不加载原厂库，仍限制为逐行 8-bit 4:2:0、单 slice。 |
| V4L2 Request 前端 | 受限 H.264 High 通过 | V13 的旧 92 项 Request 重跑通过，520 帧匹配；新增预测和多参考负例也通过。保留 SG 别名隔离及 reset 后释放。V9 的活跃 DMA 取消／SG 别名结果另存。 |
| VA-API 桥 | B 站 High 原码流通过 | 沿用 V10 的独立桥，声明 Baseline/Main/High、屏蔽 High10。B 站缓冲 65 秒／1950 帧逐帧 NV12 SHA256 与软件一致，另有 576 帧多参考、72 帧加权及旧九组 65 帧回归。 |
| KDE 下 H.264 硬解 | 短测通过 | 2026-11-06 在 KDE 标准版解码 60 帧 640×360，逐帧匹配软件参考；零超时 / 故障，结束后 suspended。Chrome 视频播放不代表已使用此 VPU。 |
| 解码后空闲省电 | 已验证 | 测试结束后进入 suspended；尚未覆盖长时间播放及反复启停。 |
| 普通播放器与浏览器硬件解码 | 受限 H.264 短测通过 | FFmpeg 32 帧，Chrome 157 与 Firefox ESR 153 各三组 24 帧原始 NV12 匹配软件参考；两浏览器均有实际 VPU 作业／IRQ 证据。使用独立 profile、VA-API 桥与进程级沙箱调试例外，长期和默认配置未验收。 |
| Chrome B 站播放 | 匿名 360p High 连续硬解通过 | V13／Chrome 157：VaapiVideoDecoder、平台标志 true，约 62 秒呈现 1864 帧、丢帧 0，VPU 作业／IRQ 各增 1880；随后网站登录窗暂停。默认 AV1 仍走软件；登录后高清和长期播放尚未验证。 |
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
本轮临时加载 V13，新增加权、多参考、参考 B 和 temporal direct；B 站 High 原码流及
Chrome 网页硬解通过，92 项 Request 和 FFmpeg 旧用例重跑通过，详细计数见适配文档。
V8 补 Request 必需的输出校验回调，V9 支持客户端共同 POC
原点，并在物理别名检查后才写输入 padding。标准链路为
`FFmpeg → VA-API 桥 → V4L2 Request → 原生 H.264 参数后端 → VPU`，无需原厂 AVC 库。
Chrome／Firefox 独立 profile 下各三组 24 帧硬解原始输出通过；Chrome 呈现短测丢 1 帧，
Firefox 呈现 8 帧、丢帧 0。Firefox 需要进程内绑定匹配 Mesa GLES 库。浏览器 GPU
读回带转换，原始像素在完成队列中保存并核对，详见适配文档。V13 新增预测能力的
原始 NV12 另以 FFmpeg hwdownload 核对；多 slice、field、long-term、非 IDR I、10-bit
和其他 chroma format 仍不支持，L1 超过两项尚无本轮像素资格覆盖。
具体分步目标、限制及验证条件见 [V4L2 适配路线](source/v4l2.md)。

历史源码、候选与证据位于只读研究归档 `archive/linux/vpu-v4l2-20261107-v2/`，
统一报告为 `evidence/qualification-v9.json`。测试 profile 和临时目录已清理。
当前 V13 仅临时加载在内存，磁盘仍为 V2，重启后回到 V2；尚未持久安装。
V13 用例、模块／桥身份、网页和空闲证据见 [validation-v13.json](source/validation-v13.json)。
V10 历史用例见 [validation-v10.json](source/validation-v10.json)；V13 的完整截图、日志、
码流和报告保留在本轮 `w202ds-projects/vpu-v4l2-high_20261108/bilibili-high/`。
