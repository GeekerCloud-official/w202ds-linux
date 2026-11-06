# W202DS CH Sensor Hub IIO

本驱动经现有 SIPC dst9/channel4 的两个 sbuf RX 环接收 CH 固件数据，用两个阻塞接收线程等待事件，不注册/覆盖 spipe notifier，不复位 CH 或 modem。传感器共享同一传输和固件，保持一个模块拥有接收环。

已在 `6.18.35-w202ds-m23-wifi` / p78 Server 热加载、读取全部 10 个 IIO 属性并卸载，之后原始协议查询恢复正常。确认固件 `20250605`，并要求准确的器件身份：

| IIO 名称 | 通道 | 单位 |
| --- | --- | --- |
| w202-lsm6dsoq | in_accel_{x,y,z}_input / in_anglvel_{x,y,z}_input | m/s² / rad/s |
| w202-mmc5603 | in_magn_{x,y,z}_input | Gauss（Hub µT 转换） |
| w202-ltr578 | in_illuminance_input | 采用 Hub 光照协议解释；实物标定待验 |

驱动发送原厂 40 字节 AP/CH 硬件时钟映射并每 10 秒续同步。每次直接读取发起 batch/enable，等待新事件后 disable；固定点整数解析 float，不使用内核 FPU。双环独立处理碎片、CRC 与长度，`hub_stats` 给出计数和 Hub 微秒时间戳。卸载停止自己的传感器及接收线程。

CM2 独立构建：

```sh
make -C /path/to/Devices KDIR=/matched/kernel/output OUT=/new/work/modules sensorhub-iio
```

自动先构建匹配的 mailbox、power manager、SIPC core 符号提供者，目标产物是 `OUT/sensorhub-iio/w202_hub_iio.ko`；不会安装或重载正在运行的基础模块。协议解析的独立主机测试：

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror test_protocol.c -o /work/test-protocol
/work/test-protocol
```

`w202_hub_protocol.h` 被驱动与该测试共同使用，覆盖真实帧、双环碎片、CRC 损坏恢复、最大帧和数值异常拒绝。硬件验证原件在 `archive/linux/server-sensor-data-20261104-r63/r63-hub-iio-test-v2.json`。

当前仅提供直接读取：匹配内核没有启用 IIO_BUFFER。物理动作/方向、光照变化、校准、缓冲、高频率、系统挂起和长期稳定性待验。盖子合上时读到光线 0，不能据此宣称光照响应已验收。尚未接入固定 Server bundle 或自启服务，r63 测试结束已卸载。

原始 `w202-hubctl` 和本模块会消费同一接收环，不能并行采样。时钟同步诊断模块不应与本模块的周期同步重复运行。
