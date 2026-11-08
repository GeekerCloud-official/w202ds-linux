# W202DS ALSA 扬声器适配

四颗功放已由本机 I²C ID 确认为 **AW88166**（0x2066），地址为 bus8 的
0x34–0x37，共用 AON GPIO61。使用 Linux 上游 `aw88166` codec，原厂 ACF
保持原字节，仅安装名改为上游要求的 `aw88166_acf.bin`。

扬声器链路是 `AP01 PCM → AUDCP/VBC → master IIS0（SYS_IIS3）→ 四颗 AW88166`。
AP 普通 I²S 是蓝牙路径，AGCP_IIS0 是 DP 后端，两者均不是此声卡的 CPU DAI。

```text
audio/
├── build.mk                  五个独立外置模块及 mailbox 依赖
├── source/
│   ├── w202_audio_amp.c      共享 reset 与 codec 依赖
│   ├── w202_audio_dsp.c      AUDCP 启动、邮箱、共享内存 IPC
│   ├── w202_audio_vbc.c      AP01 DAI、扬声器路由与 DSP profile
│   ├── w202_audio_pcm.c      左右独立 SPRD DMA、planar PCM
│   ├── w202_audio_card.c     一个 CPU DAI、四个上游 codec 的声卡
│   └── w202_audio.h          已核对的消息及 startup ABI
├── devicetree/w202ds-alsa.dtsi
└── firmware/prepare.py       校验原厂输入、裁剪 AVB 容器和生成参数包
```

硬件格式限定为 **48 kHz、S16_LE、双声道、NONINTERLEAVED**。其他应用格式
由 ALSA plug / PipeWire 转换。当前实现不含麦克风、耳机、通话、运行时省电和
完整 suspend/resume；DSP、AP access 和相关电源域持续开启。

## 构建和设备树

匹配内核先合入 `config/fragments/audio.config`，完成 `Image modules` 的完整构建。
随后按仓库构建策略执行：

```sh
make -C Devices KDIR=/path/to/prepared/kernel OUT=/path/to/modules w202ds-audio-driver
```

这会构建 `w202_audio_{amp,dsp,vbc,pcm,card}.ko`，并复用 modem mailbox 模块。
音频目标未加入默认 SERVER_MODULES。设备树使用 `w202ds-alsa-test.dts` 或在
匹配的基线上包含 `w202ds-alsa.dtsi`。它修改预留内存的映射属性和 GPIO 所有权，
**仅允许启动时使用，不能作为运行中 overlay**。

此配置禁用占用同一 GPIO61 的旧 DP 节点，将音频 reservation 标记为 no-map，
并通过 genpd 保证 DSP、时钟、DMA 和 codec 的依赖顺序。

启动顺序见 `userspace/w202-audio-start` 与 `w202-audio.service`，显式加载保持黑名单的模块，
应在内置 audio clock 的 deferred-probe 超时前启动。不要 unbind/rebind 内置音频时钟。

## 协议来源与验证边界

正式内核基线为 `102e847e249218775eef465c7b7c0a5cf78f040a`，使用
`CONFIG_LOCALVERSION_AUTO=y` 保留真实 Git 后缀。本机原厂模块通过 REA
反编译核对启动寄存器、IPC、VBC 和 GPIO 行为；实际 32 位
`audio.primary.whale.so` 核对参数选择编码，来源和 ABI 见
[source/PROTOCOL.md](source/PROTOCOL.md)。不引入整套原厂内核音频驱动。

2026-11-08：临时验证内核 `6.18.35-w202ds-audio1` 与全部 44 个配套外置模块
完整构建通过，音频对象 `W=1` 通过；固件准备脚本的六项边界测试通过。
修复 PCM open 约束返回值和本机 AP DMA 中断号后，50 次连续 open/close、
144000 帧 / 2.997 秒硬件播放、标准 ALSA plug 与 12 秒 PipeWire 播放通过。
四颗功放播放时均为 SYSST=0x0311、SYSCTRL=0x5240（上电且解除静音），
停止后恢复关闭。软件与寄存器验证不替代实际听感验收。

`w202ds-alsa.dtsi` 将 AP DMA 修正为 GIC INTID 88 / SPI 56，依据为本机播放时
的 GIC pending 与 DMA 状态；厂商 W205DS-ESIM 参考设备树的 SPI 55 在本机不适用。
