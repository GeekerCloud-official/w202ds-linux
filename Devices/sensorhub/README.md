# W202DS Sensor Hub

一个 CH 固件和一个 IIO 模块提供 LSM6DSOQ 加速度／陀螺仪、MMC5603 磁力计、LTR578 环境光。沿用现有 SIPC 收发环，不重置基带，不修改固件或校准数据。

```text
sensorhub/
├── README.md
├── build.mk
├── devicetree/
├── firmware/
├── source/w202-hub-iio/
└── userspace/
    ├── Makefile
    ├── README.md
    └── rootfs/usr/
        ├── lib/systemd/system/w202-sensorhub.service
        ├── lib/udev/rules.d/81-w202-sensorhub.rules
        └── libexec/w202-sensorhub-attach
```

## 当前支持

2026-11-08 在 KDE、内核 `6.18.35-w202ds-g102e847e2492`、CH 固件 `20250605` 上验证。其他固件版本不据此视为通过。

用户已验收本轮四类传感器的读取及运动／光照动态响应。桌面画面联动、方向矩阵逐项对应和校准仍列入 TODO。

| 状态 | 功能 | 验证范围 |
| --- | --- | --- |
| ✅ | 加速度三轴 | 静置模长 9.78～9.81 m/s²；转动时三轴响应，标准接口捕获四种平面方向；安装矩阵与实物方向对应待确认。 |
| ✅ | 陀螺仪三轴 | 转动时三轴响应，角速度模长峰值约 7.78 rad/s；停稳后接近 0，零偏校准待验。 |
| ✅ | 磁力计三轴 | 转动时三轴变化，本轮模长约 0.55～1.62 Gauss；罗盘方向、磁场校准待验。 |
| ✅ | LTR578 光照 | 本轮光照随操作在 0～325 lux 间变化；保持亮度时不再因无新帧超时，光强标定待验。 |
| ✅ | 连续直接读取 | 静置 60 秒／277 轮及转动约 175 秒／832 轮联合采样均无读取错误、坏帧；无人读取 5 秒后关闭传感器。 |
| ✅ | 标准桌面接口 | iio-sensor-proxy 3.7 捕获 normal、left-up、right-up、bottom-up，以及倾斜／背面朝上状态和光照变化；陀螺仪与磁场由 IIO 提供。 |
| ✅ | 开机加载配置 | systemd 在基带运行时启动 CH 后加载模块；服务已启用，完整冷启动待验。 |
| ❓ | 自动旋转、自动亮度 | 方向与光照接口已提供，桌面联动待实物验收。 |
| ❌ | IIO 缓冲采样 | 当前内核未启用 IIO_BUFFER，只支持直接读取。 |

## 构建

本地使用匹配内核的模块构建入口：

```sh
make -C Devices KDIR=/matched/kernel/output OUT=/new/work/modules w202ds-sensorhub-iio
```

输入为匹配的内核配置、符号表、SIPC 提供者和本目录源码；输出为 `OUT/w202ds-sensorhub-iio/w202_hub_iio.ko`、符号表及构建记录。用户态暂存方法见 [userspace README](userspace/README.md)。

CH SRAM 必须使用非写回缓存映射。当前 SIPC 将其映射为 Device 内存，SBUF 内核缓冲区收发须使用 `memcpy_toio/fromio`；旧通用 memcpy 尾部会造成 ARM64 对齐异常并阻塞桌面。修复位于 modem 模块，不需要修改 Linux 内核或 UEFI。

## TODO

- 已捕获实物转动与光照响应；继续核对带方向标记的安装矩阵及 KDE 旋转／亮度联动。
- 校准、挂起恢复、反复冷启动和长期稳定性。
- 有需要时增加 IIO 缓冲和高频采样；不将原始磁场当作已校准罗盘。

原始 hubctl 和 IIO 模块不能同时消费同一 RX 环；独立时间同步诊断模块也不与本驱动同时启用。协议和单位见 [source README](source/w202-hub-iio/README.md)。
