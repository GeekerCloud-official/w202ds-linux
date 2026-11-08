# WCN 共用固件（Wi-Fi STA／AP、蓝牙、GNSS）

原厂 WCN/GNSS 启动固件、两份 Wi-Fi 板配置及蓝牙初始化参数已存在。

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

蓝牙两份 INI 来自 GeekerCloud 蓝牙项目 `7a9eadb4ae503ded740e1b69faac45f1972ead24`，
作为厂商参数输入交给 `userspace/bt-init` 序列化；不是可执行固件。
PSKey 的地址是占位值，运行时使用本机地址覆盖，不能用于辨认或克隆另一台平板。
这些原厂参数未确立新增的再分发许可证，不能视作初始化工具 MIT 许可证的一部分。
