# WCN 共用固件（Wi-Fi STA／AP、蓝牙、GNSS）

原厂 WCN/GNSS 启动固件、两份 Wi-Fi 板配置和两份蓝牙参数已存在

| 本目录实际文件 | 字节数 |
| --- | ---: |
| [gnssmodem.bin](gnssmodem.bin) | 323,412 |
| [wcnmodem.bin](wcnmodem.bin) | 986,032 |
| [wifi_board_config.ini](wifi_board_config.ini) | 7,351 |
| [wifi_board_config_ab.ini](wifi_board_config_ab.ini) | 6,831 |
| [bt_configure_pskey.ini](bt_configure_pskey.ini) | 5,597 |
| [bt_configure_rf.ini](bt_configure_rf.ini) | 3,182 |

同一固件由以下功能共用：wifi、bluetooth、gnss。

来源、SHA256、格式及缺项见 [manifest.json](manifest.json)。
原始分区备份和解包基线继续独立保全。

两份蓝牙参数取自原厂 odm 分区 `firmware/`，与原文件仅行尾和空白不同，内容一致；由 `w202-bt-init` 读取，安装到 `/usr/lib/firmware/`。
`bt_configure_pskey.ini` 中的 `device_addr` 为原厂占位值，运行时被实际蓝牙地址覆盖。
