# VPU 用户态源码

`source/` 保存 AVC 流式 worker、接口测试、码流生成和运行库打包工具。
用户态接口头文件使用同一硬件工程的 `../source/w202_vpu_buffer_uapi.h`。

本目录发布 `source/` 和本文档。`server-avc-20261103-v7/` 是本地已有运行包；
专有运行库、运行包和独立的 `libva-v4l2_request/` 第三方源码树保持本地保留。

`w202-avc-worker.c` 和 `test-vendor-avc.c` 面向设备原厂的 AArch64/Bionic ABI，
需要匹配的 Bionic 链接环境及设备自带 AVC 库。`w202-avc-trace.c` 是显式启用的诊断
挂钩。原生与 Request 客户端使用 Linux libc；Request 客户端只使用标准视频接口。
`make-avc-fixture.py`、`make-runtime-fixtures.py` 使用 PyAV、NumPy 和 libx264 生成码流
及软件参考，其中 runtime 的无约束 High 样本用于原厂路径对照，不代表当前受限后端全部支持。
`package-vpu-runtime.py` 从命令行指定的本机原厂提取物整理依赖，不附带这些库。

范围检查可独立在主机运行，无需设备或原厂库。在本目录执行，先准备独立输出目录：

```sh
cc -O2 -Wall -Wextra -Werror source/w202-vpu-ranges-test.c \
  ../source/w202_vpu_ranges.c -o /path/to/output/w202-vpu-ranges-test
/path/to/output/w202-vpu-ranges-test
```

原生适配诊断包括 `w202-vpu-native-ip.c`、`w202-vpu-request-idr.c` 和
`w202-vpu-request-ip.c`；前者用共享参数后端经私有资源桥测试硬件，后两者只使用标准
V4L2/Media ioctl。`W202_FLAT_MATRIX=1` 让原生 IP 诊断显式携带六张 flat 4×4 矩阵，
覆盖 VA-API 桥发送的控制形式；不要将其结果当作 Request 成功。

`h264-ipb-fixture.h` 是仅供诊断的 Main/High POC0 解析器，接受单 slice、无加权、无重排／MMCO、
非参考 B 和每列表一项短期参考。POC 回绕只由前一参考图像更新；记录 L0/L1 的解码索引，
最后按显示顺序输出，不放入内核。用 `-DW202_NATIVE_IPB` 编译 `w202-vpu-native-ip.c`
可构建独立 IPB 硬件诊断；8 组 88 帧（含 36 个 B）像素全部匹配软件参考。

`-DW202_REQUEST_IPB` 编译 `w202-vpu-request-ip.c` 得到标准 Request IPB 客户端，
准备 MMAP／DMA-BUF、请求复用和 L1 负例。`W202_BAD_L1=missing|invalid|self` 触发
对应负例，`W202_EARLY_EXPORT=1` 提前导出 MMAP CAPTURE 后从 DMA-BUF 映射读像素，
`W202_FLAT_MATRIX=1` 显式携带矩阵。IP 构建继续交替复用两个 CAPTURE；IPB 构建保留
本组所有帧后按 POC 输出。已严格编译并在 V9 的 32 项标准队列测试中通过。

`w202-vpu-ranges-test.c` 不打开设备节点，以穷举配对 oracle 检查内核共用的物理范围
算法；ARM64 用户态 40,009 组通过；内核 attachment 与三种 DMA 别名负例也已由 V9 Request 实测覆盖。
Request 新增 `W202_DMA_ALIAS=input|reference|reference-export`；最后一种使用
同一 MMAP 的两个独立导出对象，关闭原分配上下文后测试输出／参考别名。
`W202_CANCEL_QUEUED=1` 在开始流前解除 VMA 并关闭视频文件，检查 Request 完成及重用。
V9 候选及带 boot_id／版本和前序成功门禁的脚本在历史 `request-lifecycle-v9/`。
32 项 Request、实际 AXI DMA 活跃取消／恢复及 FFmpeg 四组 32 帧均已通过。
取消脚本复用固定的 `native-cancel-v8-active-fixture/`；旧 V7/V8 候选与失败证据保留。
IPB 客户端的 `W202_CAPTURE_REUSE=1` 只使用三个 CAPTURE，排除活跃 L0/L1 后选择
复用位置，先保存像素再按 POC 输出；相关 MMAP／DMA-BUF、提前导出及 32 帧回绕均已通过。
`W202_POC_OFFSET=65536|-65536` 检查共同 POC 原点，V9 的 IP/IPB 新增三项验证通过。
IDR 客户端逐帧输出成功作业，错误作业不输出像素。

`libva-v4l2_request/` 是 GPL-3.0-or-later 的独立 VA-API→V4L2 stateless 依赖，固定
`4f60c51f7841dd74bb61c94a4ad31e630bbdab01`。其 Git 树保持不改；本地
`source/libva-v4l2-request-w202-profile.patch` 在 V10 构建时仅移除 High10，保留已验证的 High。
目前只编译 H.264，V10 使用 libva 2.22.0 和设备 DRM UAPI 头文件。构建、动态加载已验证，
V9 的 Request、提前导出 DMA-BUF 与 FFmpeg VA-API 四组像素实测通过；Chrome/Firefox 各三组 24 帧原始输出、应用后端和 VPU 作业／IRQ 已验证。
Chrome 的可见播放短测丢 1 帧，Firefox 8 帧零丢帧；默认配置和长期播放尚未验收。
两者均使用进程级沙箱调试例外，Firefox 另用专用 Mesa GLES 库绑定；详见适配文档。
WebCodecs GPU 读回不作原始像素比对通过依据，完成队列 NV12 由只读观察器另存核对。
V9 构建脚本、输入哈希、浏览器观察器源码及证据最终在研究项目
`archive/linux/vpu-v4l2-20261107-v2/`，
标准接口路线和限制见 [v4l2.md](../source/v4l2.md)。

V10 的 fixture 解析器支持 High 的 8-bit 4:2:0 参数、8×8 和 SPS/PPS scaling fallback，
向 Request/native 客户端传递最终 resolved matrix。`make-avc-fixture.py` 可指定
`--profile high --cabac 1 --transform8x8 1 --cqm asymmetric --bframes 2 --gop 100 --frames 8`，
生成单 slice 码流、软件 NV12 及编码器日志；尺寸上限 1920×1088，允许偶数裁剪尺寸。
8×8 模式开启 subme=6 以覆盖实际 8×8 变换；自定义 8×8 参数使用 x264 的 cqm8i/cqm8p。
编码器参数出现警告时终止，避免静默忽略测试要求。
IDR 的 `W202_BAD_MATRIX=missing|zero4|zero8|unused8` 分别覆盖缺失、所用零系数和未用列表；
`W202_INVALID_PROFILE=1` 使用仍不支持的 profile_idc=110。V10 最终 92 项 Request 和
FFmpeg 9 组通过；V10 未重做浏览器 High、B 站及完整第三方 Meson 套件。
