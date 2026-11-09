# w202ds-camera-driver（摄像头）

```text
camera/
├── build.mk
├── devicetree/
├── source/w202-flash/          后置补光灯 AW36518A 的 LED 驱动（手电筒模式）
├── userspace/70-w202-torch.rules
└── README.md
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 摄像头识别 | 原系统有型号记录，Linux 待验 | 原系统报告 GC13A0 与 SC520CS；前后摄对应关系未确认。 |
| 图像采集与预览 | 待完成并验证 | 尚无 Linux 实际采集与预览验收结果。 |
| 拍照 | 待完成并验证 | 传感器及 ISP 链路尚未验收。 |
| 录像 | 待完成并验证 | 尚无 Linux 实际录像验收结果。 |
| 闪光灯控制 | 手电筒可用 | 后置 AW36518A 常亮（torch）已实机点亮；拍照闪光（preflash / highlight）未实现。 |

## 后置补光灯（手电筒）

设备树已有原厂节点 `i2c@22a0000/flash-ic@63`（`sprd,flash-aw36518a_gpio`，
`flash-torch-en-gpios` = GPIO111），`w202_flash_aw36518.ko` 按该 compatible 自动加载，
注册 LED class 设备 `white:torch`，`max_brightness` 为 8，写 0 关闭。

寄存器顺序取自本机原厂 `vendor_dlkm` 的 `flash_ic_aw36518a_gpio.ko`（SHA256 f8164f9c…，REA 反编译）：
探测时写 0x07=0x89、清 0x01；开灯写 0x69=0x02、0x31=0x0c，手电电流写 0x05，
ENABLE 0x01=0x13（LED 开、TORCH 引脚模式）后拉高使能引脚；关灯先拉低引脚再清 0x01。
没有使能引脚时用 0x01=0x0b 的 I²C 手电模式，与原厂相同。亮度 1–8 线性映射到
不超过原厂手电档位 0x4a 的电流码，持续点亮不超过厂商出货设置。关机、卸载模块时熄灯。

`userspace/70-w202-torch.rules` 安装到 `/etc/udev/rules.d/` 或 `/usr/lib/udev/rules.d/`：
把 `brightness` 的属组改为 `video` 并加组写权限，桌面用户（`video` 组）无需 root 即可开关。

构建：

```sh
make -C Devices KDIR=/path/to/prepared/kernel OUT=/path/to/modules w202ds-camera-flash
```

需要内核 `CONFIG_LEDS_CLASS`。2026-11-09 在内核 `6.18.35-w202ds-m23-audio-20261109` 上验证：

- 加载日志 `AW36518A id 0x1a, torch pin mode`；用户目视确认补光灯点亮。
- 写 `brightness` 8 后读寄存器 0x01=0x13、0x05=0x4a；写 0 后 0x01=0x00，灯灭。
- 安装 udev 规则后，用户 `w200` 不经 root 直接写 `brightness` 开关成功。

未验证：拍照闪光、长时间常亮的温升、挂起恢复后状态。

更新日期：2026-11-09
