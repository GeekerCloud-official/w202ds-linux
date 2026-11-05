# 扬声器、麦克风与音频链路固件

AUDCP 原厂 l_agdsp_a/b 实体已归位；AW883xx ACF/monitor 固件尚未取得

| 本目录实际文件 | 字节数 |
| --- | ---: |
| [stock-partitions/l_agdsp_a.img](stock-partitions/l_agdsp_a.img) | 6,291,456 |
| [stock-partitions/l_agdsp_b.img](stock-partitions/l_agdsp_b.img) | 6,291,456 |

`stock-partitions/` 保存原厂完整容器，包括原头部和填充；内部微码尚未完整解码，
不直接当作通用 `request_firmware` 输入。A/B 分开保存，归档不代表已运行或驱动验收。

已由参考源码确认名称、但尚未取得的文件：

- `aw883xx_acf.bin`
- `aw883xx_monitor.bin`

来源、SHA256、格式及缺项见 [manifest.json](manifest.json)。
原始分区备份和解包基线继续独立保全。
