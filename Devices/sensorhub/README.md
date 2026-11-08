# w202ds-sensorhub-driver（运动、磁场与光线传感器）

```text
sensorhub/
├── build.mk
├── devicetree/
├── firmware/
├── README.md
├── source/
└── userspace/
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 加速度读取（LSM6DSOQ） | 可用 | 静置模长约 9.84 m/s²；平放 z≈+9.9，四个方向均正确上报。校准待验。 |
| 陀螺仪读取（LSM6DSOQ） | 可用 | 静置接近 0（量化步长 2443 µrad/s），转动时最高约 8.7 rad/s；坐标方向未逐轴核对。 |
| 磁场读取（MMC5603） | 三轴数据可读 | 指南针朝向与磁场校准待验。 |
| 环境光读取（LTR578 / STK3x1x） | 可用（STK3x1x） | 手电照射约 110–160 lux，遮挡或暗处为 0；步长约 2.3 lux，约 9 lux 以下读 0。LTR578 机型待验。 |
| 自动旋转 | 可用 | 拆下键盘进入平板模式后 GNOME 接管方向，iio-sensor-proxy 报告 normal/left-up/right-up/bottom-up；装回键盘恢复横屏。 |
| 自动亮度 | 待验收 | iio-sensor-proxy 已提供环境光，GNOME 自动亮度联动未验收。 |
| 连续采样 | 可用 | 读取后持续上报并缓存最近样本，5 秒无人读取自动关闭；匹配内核没有 IIO 缓冲。 |
| 开机加载 | 可用 | `w202-sensorhub.service` 在 modem 运行时启动 CH 后绑定驱动，重启实测三个 IIO 设备自动出现。 |
| 冷启动与长期运行 | 待验证 | 尚无反复冷启动和长时间稳定性结论。 |

自动旋转依赖键盘驱动 `w202_zsk` 上报的 `SW_TABLET_MODE`（见 [../keyboard/](../keyboard/)）：Mutter 只在平板模式（或没有指针设备）时接管面板方向，而键盘触控板一直存在。

CH 共享内存必须非缓存映射，见 [source/w202-hub-iio/README.md](source/w202-hub-iio/README.md)。读取 `/sys/kernel/debug/clk/clk_summary` 会访问未上电的摄像头时钟寄存器并使整机挂死，勿在本设备上读取。

更新日期：2026-11-09
