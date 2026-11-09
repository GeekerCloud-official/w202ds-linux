# W202DS ALSA 扬声器与麦克风适配

四颗功放已由本机 I²C ID 确认为 **AW88166**（0x2066），地址为 bus8 的
0x34–0x37，共用 AON GPIO61。使用 Linux 上游 `aw88166` codec，原厂 ACF
保持原字节，仅安装名改为上游要求的 `aw88166_acf.bin`。

扬声器链路是 `AP01 PCM → AUDCP/VBC → master IIS0（SYS_IIS3）→ 四颗 AW88166`。
AP 普通 I²S 是蓝牙路径，AGCP_IIS0 是 DP 后端，两者均不是此声卡的 CPU DAI。

麦克风链路是 `UMP9620 MIC1（主麦）/ MIC3（背麦）→ ADC_1 / ADC_3 → ADD0（MIC13）→
AGCP 数字 codec → VBC ADC0（IIS1）→ DSP → AP01 录音 FIFO → AGCP DMA 请求 3/4`。
同一声卡 `Speakers` 的 PCM 0 为扬声器，PCM 1 为麦克风（左声道 MIC1、右声道 MIC3）。

```text
audio/
├── build.mk                  六个独立外置模块及 mailbox 依赖
├── source/
│   ├── w202_audio_amp.c      共享 reset 与 codec 依赖
│   ├── w202_audio_dsp.c      AUDCP 启动（doorbell 100 + AGCP 上电等待）、邮箱、共享内存 IPC
│   ├── w202_audio_vbc.c      AP01 播放／录音 DAI、扬声器路由、IIS1 源选择与 DSP profile
│   ├── w202_audio_pcm.c      左右独立 SPRD DMA、planar PCM（播放与录音）
│   ├── w202_audio_mic.c      UMP9620 模拟 codec + AGCP 数字 codec 的 ASoC 组件
│   ├── w202_audio_card.c     扬声器（四个上游 codec）和麦克风两条 DAI link 的声卡
│   └── w202_audio.h          已核对的消息及 startup ABI
├── devicetree/
│   ├── w202ds-alsa.dtsi              仓库设备树用（禁用旧 DP 节点）
│   └── w202ds-alsa-preserve-dp.dtsi  保留 w202_dp 节点的变体，也是内核
│                                     W202DS_AUDIO_LK_DT_FIXUP 启动改写的来源
├── userspace/
│   ├── w202-audio-start / w202-audio.service   按顺序加载模块
│   ├── w202-audio.conf       modprobe 黑名单，由启动服务显式加载
│   ├── w202ds.conf           PipeWire/ACP profile set：hw:%f,0 输出、hw:%f,1 输入
│   └── 89-w202ds-audio.rules 为 Speakers 声卡指定 ACP_PROFILE_SET
└── firmware/prepare.py       校验原厂输入、裁剪 AVB 容器和生成参数包
```

硬件格式限定为 **48 kHz、S16_LE、双声道、NONINTERLEAVED**（播放与录音相同）。
其他应用格式由 ALSA plug / PipeWire 转换。当前实现不含耳机、通话、副麦 MIC2、
运行时省电和完整 suspend/resume；DSP、AP access 和相关电源域持续开启。

## 构建和设备树

匹配内核先合入 `config/fragments/audio.config`，完成 `Image modules` 的完整构建。
随后按仓库构建策略执行：

```sh
make -C Devices KDIR=/path/to/prepared/kernel OUT=/path/to/modules w202ds-audio-driver
```

这会构建 `w202_audio_{amp,dsp,vbc,pcm,mic,card}.ko`，并复用 modem mailbox 模块。
音频目标未加入默认 SERVER_MODULES。设备树使用 `w202ds-alsa-test.dts` 或在
匹配的基线上包含 `w202ds-alsa.dtsi`。它修改预留内存的映射属性和 GPIO 所有权，
**仅允许启动时使用，不能作为运行中 overlay**。

此配置禁用占用同一 GPIO61 的旧 DP 节点，将音频 reservation 标记为 no-map，
并通过 genpd 保证 DSP、时钟、DMA 和 codec 的依赖顺序。录音另需 `sprd-pcm-audio`
的 `normal_c_l/normal_c_r`（AGCP DMA 请求 3/4，与原厂 DT 一致）和
`audio-codec@1000` 改为 `zte,w202ds-mic-codec`。

若沿用 LK 传入的板级 DT 启动（不使用仓库设备树），内核开启
`CONFIG_W202DS_AUDIO_LK_DT_FIXUP`（kernel 子模块，`config/fragments/audio-lk-dt.config`），
在 `early_init_dt_scan()` 之前把 `w202ds-alsa-preserve-dp.dtsi` 的同等改动应用到
LK DT 在内核 BSS 中的副本；FDT fixmap 仍映射 LK 原始地址。两种方式只选一种。

启动顺序见 `userspace/w202-audio-start` 与 `w202-audio.service`（`sysinit.target` 前），
显式加载 `w202-audio.conf` 中保持黑名单的模块。DSP 模块晚于内核 10 秒 deferred-probe
超时加载时，AGCP 时钟门控 `56200000.clock-controller` 不会自动重试，启动脚本在 DSP
绑定后只做一次 bind；不要 unbind/rebind 内置音频时钟。

安装位置：`w202-audio-start` → `/usr/libexec/`，`w202-audio.service` →
`/usr/lib/systemd/system/`（`systemctl enable`），`w202-audio.conf` → `/usr/lib/modprobe.d/`，
`w202ds.conf` → `/usr/share/alsa-card-profile/mixer/profile-sets/`，
`89-w202ds-audio.rules` → `/usr/lib/udev/rules.d/`。PipeWire 默认 profile set 只探测
PCM 0，没有该 udev 规则时桌面看不到麦克风。

## 协议来源与验证边界

正式内核基线为 `102e847e249218775eef465c7b7c0a5cf78f040a`，使用
`CONFIG_LOCALVERSION_AUTO=y` 保留真实 Git 后缀。本机原厂模块通过 REA
反编译核对启动寄存器、IPC、VBC 和 GPIO 行为；实际 32 位
`audio.primary.whale.so` 核对参数选择编码，来源和 ABI 见
[source/PROTOCOL.md](source/PROTOCOL.md)。不引入整套原厂内核音频驱动。

2026-11-08：正式内核 `6.18.35-w202ds-g102e847e2492` 已完成 Image、393 个
内核树内模块和全部 44 个配套外置模块的完整构建，并安装、重启验证。
音频对象 `W=1` 通过；固件准备脚本的六项边界测试通过。
修复 PCM open 约束返回值和本机 AP DMA 中断号后，50 次连续 open/close、
144000 帧 / 2.997 秒硬件播放、标准 ALSA plug 与 12 秒 PipeWire 播放通过。
四颗功放播放时均为 SYSST=0x0311、SYSCTRL=0x5240（上电且解除静音），
停止后恢复关闭。这些底层和寄存器验证先在临时 `audio1` 内核完成；正式
内核又通过 50 次 open/close 与 144000 帧 / 2.997 秒播放。

正式内核上的 KDE/PipeWire 已播放左右声道提示音，用户确认实际听到声音。
Chrome 157 的 B 站 AAC 双声道音频已接入同一 PipeWire 扬声器输出，
后台已捕获 Chrome 播放流，视频未静音，播放期间 ALSA PCM 为 RUNNING。
AAC 由浏览器软件解码，采样率和布局由 PipeWire 转为本声卡支持的格式；
视频 H.264 的 VPU 硬解是独立路径。当前结果不覆盖麦克风、耳机或通话。

`w202ds-alsa.dtsi` 将 AP DMA 修正为 GIC INTID 88 / SPI 56，依据为本机播放时
的 GIC pending 与 DMA 状态；厂商 W205DS-ESIM 参考设备树的 SPI 55 在本机不适用。

## 麦克风与 GNOME 实机验证（2026-11-09）

内核 `6.18.35-w202ds-m23-audio-20261109`（LK DT + `W202DS_AUDIO_LK_DT_FIXUP`，
GNOME 48 / PipeWire）。原厂 `snd-soc-sprd-codec-ump9620*.ko`、`snd-soc-sprd-vbc-v4.ko`
的寄存器、DAPM 通路、电源事件和 IIS1 源选择经 REA 核对，见 PROTOCOL.md。

- 开机 `w202-audio.service` 自动完成：AUDCP 在 doorbell 100 后 48 ms 内发出启动通知，
  `/dev/snd/pcmC0D0p` 与 `pcmC0D1c` 就绪，PipeWire 同时出现扬声器输出和麦克风输入。
- 扬声器：GNOME 下播放，用户确认实际听到声音。
- 麦克风：`plughw:0,1` 48 kHz S16 立体声 5 秒，安静环境左（MIC1）RMS −65 dBFS、
  右（MIC3）RMS −55 dBFS，无数字静音；扬声器播放 440 Hz 时两路在 440 Hz 处明显突出。
  录下人声后回放，用户确认能听到清楚的录音。
- 未验证：副麦 MIC2、耳机、通话、录音与播放长时间并发、挂起恢复后的音频。
