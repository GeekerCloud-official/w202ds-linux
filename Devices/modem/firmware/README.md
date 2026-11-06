# 蜂窝基带固件

nr_modem_a/b、nr_phy_a/b、pm_sys_a/b 原厂分区实体已归位；CH 固件由 sensorhub 统一存放

| 本目录实际文件 | 字节数 |
| --- | ---: |
| [stock-partitions/nr_modem_a.img](stock-partitions/nr_modem_a.img) | 52,428,800 |
| [stock-partitions/nr_modem_b.img](stock-partitions/nr_modem_b.img) | 52,428,800 |
| [stock-partitions/nr_phy_a.img](stock-partitions/nr_phy_a.img) | 52,428,800 |
| [stock-partitions/nr_phy_b.img](stock-partitions/nr_phy_b.img) | 52,428,800 |
| [stock-partitions/pm_sys_a.img](stock-partitions/pm_sys_a.img) | 1,048,576 |
| [stock-partitions/pm_sys_b.img](stock-partitions/pm_sys_b.img) | 1,048,576 |

共用固件归属：[sensorhub](../../sensorhub/firmware/README.md)。本硬件不重复复制该固件。

`stock-partitions/` 保存原厂完整容器，包括原头部和填充；内部微码尚未完整解码，
不直接当作通用 `request_firmware` 输入。A/B 分开保存，归档不代表已运行或驱动验收。

pm_sys 是 SoC 电源管理协处理器，保留在现有基带运行依赖内；不是 UMP9620 PMIC 固件。
原厂容器不是通用 request_firmware 输入；不修改当前 loader 和固定 bundle。
nr_deltanv、fixnv、runtimenv 仍是独立私有输入，不作为公共固件复制。

来源、SHA256、格式及缺项见 [manifest.json](manifest.json)。
原始分区备份和解包基线继续独立保全。
