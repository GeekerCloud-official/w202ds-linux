# WCN 用户态（蓝牙）

本目录管理 SC2355 / Marlin3 蓝牙启用所需的用户空间源码和服务模板，与内核 `../source/sprdbt_tty/`、参数 `../firmware/` 同属一个硬件项目。

| 路径 | 内容 |
| --- | --- |
| `bt-init/w202-bt-init.c` | 厂商初始化：在 `/dev/ttyBT0` 上依次发送 PSKey（`0xFCA0`，176 字节）、RF（`0xFCA2`，252 字节）和启用命令（`0xFCA1`）。来自 mu300-linux `tools/bt-init`，MIT 许可，见 `bt-init/LICENSE`。 |
| `rootfs/usr/local/sbin/w202-bluetooth-start` | 加载 `wcn_bsp` / `sprdbt_tty`，等待 `/dev/ttyBT0`，取得蓝牙地址，重置蓝牙 rfkill，运行 `w202-bt-init`，最后 `btattach -B /dev/ttyBT0 -P h4`。 |
| `rootfs/etc/systemd/system/w202-bluetooth.service` | 启动上述脚本，排在 `bluetooth.service` 之前；停止后阻断蓝牙 rfkill。 |
| `rootfs/usr/local/sbin/w202-wcn-reopen`、`rootfs/etc/systemd/system/w202-wcn-reopen.service`、`rootfs/etc/udev/rules.d/70-w202-wcn.rules` | 芯片复位（`FW_ERROR`）或 Wi-Fi 发送停滞（`TX_STALL`）后：先停蓝牙，重开 Wi-Fi，再启动蓝牙。复位后旧 HCI 不可用，必须重新初始化。 |
| `rootfs/usr/lib/systemd/system-sleep/w202-wcn` | 休眠前停止蓝牙使芯片断电，恢复后重新启动蓝牙，让 Wi-Fi 与蓝牙从新加载的固件冷启动。蓝牙连接不跨休眠保持。 |

## 构建与暂存

在本地交叉工具链环境、本目录执行：

```sh
make OUT="$PWD/output" all
make OUT="$PWD/output" DESTDIR="$PWD/output/rootfs" install
```

输出静态 ARM64 程序 `w202-bt-init`，并把脚本、单元、udev 规则、休眠钩子和 `../firmware/bt_configure_*.ini` 暂存到 `DESTDIR`。`install` 只接受显式 `DESTDIR`，不加载模块、不启用服务。

## 运行依赖

- 内核：`BT`、`BT_BREDR`、`BT_LE`、`BT_HCIUART`、`BT_HCIUART_H4`；桌面另需 `BT_RFCOMM`、`BT_BNEP`、`BT_HIDP`。模块 `wcn_bsp.ko`、`sprdbt_tty.ko`，固件 `wcnmodem.bin`。
- 目标系统：BlueZ（含 `btattach`）、util-linux `rfkill`、systemd、udev；复位处理另需 NetworkManager（`nmcli`）和 iproute2。
- `w202-bluetooth.service` 依赖 Wi-Fi 启动单元 `w202-wifi.service`（按顺序加载 WCN 模块）；该单元属于 Wi-Fi 集成，不在本目录。
- 启用服务（`systemctl enable w202-bluetooth.service`）由发行版集成阶段负责。

## 蓝牙地址

`bt_configure_pskey.ini` 中的 `device_addr` 只是原厂占位值，`w202-bt-init -a` 在运行时覆盖。启动脚本按以下顺序取地址：

1. `/var/lib/w202ds/bdaddr` 缓存；
2. 只读挂载 `/dev/disk/by-partlabel/prodnv`，读取原厂 `btmac.txt` 首行（Android 中的 `/mnt/vendor/btmac.txt`）；
3. 以上均无效时，由 `/etc/machine-id` 经 SHA-256 派生本地管理单播地址。

结果写入缓存。仓库不包含任何设备的真实地址、`prodnv` 备份或 BlueZ 配对数据。

## TODO / 实验中

- 启动单元为 `Type=simple`，`bluetooth.service` 不等待厂商初始化完成；实际启动可用，但不是就绪保证。
- 配对、ACL 传输、LE 连接、HID、A2DP / SCO 及休眠后重连尚未验收。
- 历史日志中曾出现一次 BlueZ `Invalid Index (0x11)` 与 `Failed to set mode: Authentication Failed (0x05)`，当前启动未复现，原因未确认。
