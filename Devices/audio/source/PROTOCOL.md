# 本机 AUDCP / VBC 协议依据

本实现针对 W202DS（UMS9620 / Qogirn6pro）。REA 分析对象是本机原厂
`sprd_audcp_boot.ko`、`sprd-vbc-codec.ko`、SIPC / DSP access 模块，以及
`vendor_a/lib/hw/audio.primary.whale.so`。HAL 为 32 位，SHA256 为
`c61ef393b642b033100bdf7c8d0fc7dfc5ce1df9966a7a55ec1752a9584fa5a8`。
原厂路由及参数来自本机 ODM XML；这些二进制与 XML 不随新源码发布。

## 内存与邮箱

| 内容 | 物理位置 / 大小 |
| --- | --- |
| 固件 reservation | 0xafa00000 / 0x600000 |
| 既有 audio reservation | 0xaf700000 / 0x300000 |
| 参数共享区 | 0xaf980000 / 0x400 |
| TX / RX 环及索引 | 0xaf980400 / 0xa10 |
| profile 共享区 | 0xaf981210 / 0x1400 |

不创建重叠 reservation。IPC 仅映射上述必要区域，不能清零整块 3 MiB audio memory。
TX、RX 各 64 条 20 字节消息，结构是 `le16 command, le16 channel, le32 parameter[4]`。
尾部四个 u32 是单调读写索引。doorbell 为
`command << 48 | channel << 32 | parameter[0]`，mailbox 使用目标 core 5 / sensor 0。
启动通知 command/channel/parameter0 均为 0xbeee；唤醒 doorbell 为 100。
channel 2 / command 0x25 表示 DSP assertion。

启动向量为 `(firmware_phys + 0x80) >> 1`，必须保留原固件 0x80 头部。
`sprd_audcp_boot.ko:start_store` 的 DDR mode 0 提供 reset、bootprotect 和 vector 顺序。

## 播放与调音

- command 4 / 5 为 startup / shutdown，startup 为 0x26c 字节，结构大小编译断言。
- command 7 为 trigger，parameter2=1。normal AP01 不发送 command 6 hw_params。
- command 9 是 control：master width ID 0x26，payload 单 u32；数字增益 ID 3，
  payload 为 `{startup_reload, id, left, right}`。
- command 11 是 profile，外层共享参数为 `{selector, type=1, physical, size}`，
  消息 ID=selector、stream=-1。DSP VBC profile 长度为 0x6c4。
- HAL `set_vbc_param`（REA 0x4f744）计算 selector 为
  `(xml_mode << 24 | logical_param_id << 16) + dsp_case`。
- HAL `get_audio_param_mode`（REA 0x598b8）用 XML offset / struct size 得到 mode。
- HAL 0x881d0 静态表为 `4b0000007d100200`，ID 75 指向 0x2107d 的
  `Music\Handsfree\Playback`。本机 speaker XML mode=64，最终 selector **0x404b0000**。
  XML common 列表中的排序 68 不是此 logical ID。

AP01 VBC 位于 0x56510000，左右 FIFO 偏移为 0 / 4，config=0x20，
enable=0x2c bits8/9，clear=0x30 bit2，DMA=0x50 bits0/1；watermark 为 80 个样本。
SPRD DMA 接口沿用同版本上游 `sound/soc/sprd/sprd-pcm-dma.c` 的 linked-list 约定，
左右独立传输，每个 stereo period 仅通知一次。STOP 后同步收回回调，prepare 重建描述符。

实机 AP DMA 中断为 **GIC INTID 88（SPI 56，level-high）**。导入设备树与
W205DS-ESIM Android 13 / 5.4.276 参考包都写成 SPI 55；W202DS 实测中，
DMA 已搬运且 raw/masked 状态均为 0x3，但 SPI 55 无中断。播放时 GIC pending
指向 INTID 88，停止 DMA 后该 pending 位消失。启动设备树覆盖为 SPI 56 后，
周期中断、连续写入和 drain 均正常。不能用参考板的 IRQ 值代替本机验证。

PCM open 的 ALSA constraint API 以负数表示失败，正数可表示约束成功更新；
不能把 `snd_pcm_hw_constraint_integer()` 的正返回作为 open 提前结束条件。

协议没有 transaction ID。共享参数串行处理；任何发送失败、环损坏、assertion 或 ACK
超时都会让实例永久 failed，避免迟到 ACK 被下一条命令误认。恢复需要重新启动 DSP。

功放上游 ACF 解析器可使用本机容器；额外的 MULTIBIN 0x12 在原厂与上游均被忽略，
不应为了绕过解析而修改 ACF。共享 GPIO61 由一个保持开启的 genpd 管理，codec 不各自申请 reset。

邮箱发送完成使用匹配控制器的 `mbox_chan_txdone`，与原厂 `aud_smsg_send` 一致；
控制器 send_data 已将 u64 同步复制到硬件或内部 FIFO。DSP 命令 ACK 仍独立从 RX 环接收。
内置 audio clock 驱动不支持本轮测试中的 unbind/rebind，音频按启动服务加载，禁止热重绑时钟。
