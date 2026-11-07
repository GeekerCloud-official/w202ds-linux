# w202ds-wcn-driver（Wi-Fi / 蓝牙 / 卫星定位）

```text
wcn/
├── build.mk
├── devicetree/
├── firmware/
├── README.md
├── source/
└── userspace/
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| Wi-Fi 联网（STA） | 已验证 | 当前网络自动连接并可通过 SSH 使用（2026-11-05 验证）。 |
| Wi-Fi 热点（AP） | 历史验证通过 | 热点曾独立运行；与 STA 模式二选一。 |
| STA / AP 模式使用 | 分别可用 | 当前需求为二选一，未提供并发可用性结论。 |
| 蓝牙控制器（HCI / BlueZ） | 已验证 | `hci0` UP RUNNING，BlueZ 显示 Powered: yes，厂商 Spreadtrum（492），HCI/LMP 5.0（2026-11-08 验证）。 |
| 蓝牙设备发现 | 部分验证 | BlueZ 缓存中有 17 条历史发现记录；本轮未重新扫描。 |
| 蓝牙配对、文件传输与音频 | 待验证 | 尚无已配对设备，未验证 ACL 传输、LE 连接、HID、A2DP / SCO 及休眠后重连。 |
| GNSS 卫星定位 | 待验证 | 尚无实际定位结果。 |

## 蓝牙

工作链路：WCN PCIe → `sprdbt_tty`（`/dev/ttyBT0`）→ `w202-bt-init` 上传 PSKey / RF 参数 → `btattach` H4（`hci_uart`）→ BlueZ。控制器不经物理 UART，`hciconfig` 中的 Bus UART 指 `hci_uart` 接口。

- 内核模块：`w202ds-wcn-bluetooth` 生成 `sprdbt_tty.ko`，符号提供者为 `w202ds-wcn-bus`；源码为 `source/sprdbt_tty/`。
- 用户态：`userspace/` 提供厂商初始化工具 `w202-bt-init`、启动脚本、systemd 单元，以及 Wi-Fi / 蓝牙共用的芯片复位与休眠处理，见 [userspace/README.md](userspace/README.md)。
- 参数：`firmware/bt_configure_pskey.ini`、`firmware/bt_configure_rf.ini` 安装到 `/usr/lib/firmware/`；芯片固件与 Wi-Fi 共用 `wcnmodem.bin`。
- 内核配置需要 `BT`、`BT_BREDR`、`BT_LE`、`BT_HCIUART`、`BT_HCIUART_H4`，桌面使用另需 `BT_RFCOMM`、`BT_BNEP`、`BT_HIDP`。
- 蓝牙地址：PSKey 中的地址仅为占位，运行时由启动脚本传入；来源依次为 `/var/lib/w202ds/bdaddr` 缓存、`prodnv` 分区中的 `btmac.txt`，均不可用时由 `/etc/machine-id` 派生本地管理地址。仓库不包含任何设备的真实地址。

更新日期：2026-11-08
