# W202DS CH Sensor Hub IIO

本驱动经现有 SIPC dst9/channel4 的两个 sbuf RX 环接收 CH 固件数据，用两个阻塞接收线程等待事件，不注册/覆盖 spipe notifier，不复位 CH 或 modem。传感器共享同一传输和固件，保持一个模块拥有接收环。

## 前提：CH 共享内存必须非缓存映射

spipe_ch 的环头与收发缓冲在 CH SRAM `0x6500d000`，该区域没有 `struct page`，由 `sipc-core` 的 `memremap` 映射。若映射为写回缓存（`w202_hub_map_audit` 读到 `MAIR=0xff`），AP 写入的命令与写指针停留在 AP 缓存里，不监听 AP 缓存的 CH Cortex-M4 读不到：TX 读指针永远为 0，只有缓存被偶然写回后再响一次门铃才会被处理；而 `sbuf_write` 只在环由空变非空时响门铃，环一旦积压便不再通知 CH。`sipc-core` 必须用不回退到 WB 的 `MEMREMAP_WC`（审计结果 `attr=2 MAIR=0x44`）。

## 固件与器件

探测时依次查询固件版本（0xcd）、传感器列表（9），发送 AP/CH 时钟映射（0xc4，此后每 10 秒续同步）与 host normal（0xc5）。CH 在收到时钟映射前不上报任何数据。接受以下两个已用实测数据核对上报格式的 CH 映像，其余版本拒绝（`-EPROTONOSUPPORT`）：

| CH 固件 | 加速度/陀螺 | 磁力计 | 光线 |
| --- | --- | --- | --- |
| 20250605 | acc/gyro_lsm6dsoq（ST） | mag_mmc5603（MEMSIC） | light_ltr578 |
| 20250219 | acc/gyro_lsm6dsoq（ST） | mag_mmc5603（MEMSIC） | light_stk3x1x（Sensortek） |

句柄固定为 1 加速度、2 磁力计、4 陀螺、5 光线；光线设备按 CH 报告的芯片命名。

## IIO 接口

| IIO 名称 | 通道 | 单位 |
| --- | --- | --- |
| w202-lsm6dsoq | in_accel_{x,y,z}_raw × in_accel_scale；in_anglvel_{x,y,z}_raw × in_anglvel_scale | raw 为 µm/s² 与 µrad/s，scale 0.000001 得 m/s² 与 rad/s |
| w202-mmc5603 | in_magn_{x,y,z}_raw × in_magn_scale | raw 为 nT，scale 0.00001 得 Gauss |
| w202-ltr578 或 w202-stk3x1x | in_illuminance_input | lux（Hub 光照协议直接给出） |

`raw` + `scale` 是 iio-sensor-proxy 轮询加速度驱动（`iio-poll-accel`）识别的接口。三个设备都带 `hub_stats`：固件版本、各句柄样本计数与 Hub 微秒时间戳、两个环的坏帧计数。

## 采样方式

读某个传感器时若它未开启，先发送 batch（200 ms 周期）与 enable，并等待开启后的第一个样本（约 0.6 s）。此后传感器保持上报，读取直接返回最近样本（约 10 ms）；样本超过两个周期加 100 ms 未更新时等待新样本，最长 2.5 秒。某个传感器 5 秒无人读取即由空闲任务关闭。浮点值以定点整数解析，不使用内核 FPU。卸载时关闭本模块开启的传感器并停止接收线程。

## 构建与测试

CM2 独立构建：

```sh
make -C /path/to/Devices KDIR=/matched/kernel/output OUT=/new/work/modules sensorhub-iio
```

自动先构建匹配的 mailbox、power manager、SIPC core 符号提供者，目标产物是 `OUT/sensorhub-iio/w202_hub_iio.ko`；不会安装或重载正在运行的基础模块。协议解析的独立主机测试：

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror test_protocol.c -o /work/test-protocol
/work/test-protocol
```

`w202_hub_protocol.h` 被驱动与该测试共同使用，覆盖真实帧、双环碎片、CRC 损坏恢复、最大帧和数值异常拒绝。

开机加载与 iio-sensor-proxy 配置见 [../../userspace/README.md](../../userspace/README.md)。本模块独占 CH 的 spipe_ch 接收环和周期时钟同步，不要同时运行其它直接读取该接收环或做时钟同步的工具。

## 待验证

匹配内核没有启用 IIO_BUFFER，只提供直接读取。STK3x1x 实测有光照响应（约 2.3 lux 步长，约 9 lux 以下读 0）；陀螺仪逐轴方向、磁力计校准、系统挂起与长期稳定性待验。
