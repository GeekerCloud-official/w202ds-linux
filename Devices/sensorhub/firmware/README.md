# 传感器 Hub 公共通道固件

CH/Sensor Hub 原厂 ch_sys_a/b 实体固件已归位；2026-11-04 Server 已查询版本20250605，时钟同步后 IMU/磁力计/光线事件与 IIO 直接读取通过。

| 本目录实际文件 | 字节数 |
| --- | ---: |
| [stock-partitions/ch_sys_a.img](stock-partitions/ch_sys_a.img) | 16,777,216 |
| [stock-partitions/ch_sys_b.img](stock-partitions/ch_sys_b.img) | 16,777,216 |

同一设备内的 IMU（LSM6DSOQ）、磁力计（MMC5603）、光线（LTR578）共用这一份 CH 固件，已取消三个独立顶层目录。

`stock-partitions/` 保存原厂完整容器，包括原头部和填充；内部微码尚未完整解码，
不直接当作通用 `request_firmware` 输入。A/B 分开保存，归档不代表已运行或驱动验收。

来源、SHA256、格式及缺项见 [manifest.json](manifest.json)。
原始分区备份和解包基线继续独立保全。
