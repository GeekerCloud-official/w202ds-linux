# WCN 蓝牙用户态

SC2355／Marlin3 的厂商初始化与 BlueZ 接入。源码从 CM2 的 GeekerCloud 蓝牙项目
`7a9eadb4ae503ded740e1b69faac45f1972ead24` 导入并适配本仓库；
初始化程序来自 [mu300-linux](https://github.com/dikeckaan/mu300-linux)
`833616a6c03a` 的 `tools/bt-init/mu300-bt-init.c`，保留 MIT 版权和许可证。
没有复制其他平板的 NV、蓝牙地址或配对数据。

```text
userspace/
├── Makefile
├── README.md
├── bt-init/                 # w202-bt-init.c、MIT LICENSE
├── rootfs/usr/
│   ├── lib/systemd/system/w202-bluetooth.service
│   └── libexec/w202-bluetooth-start
└── tests/test-init.py       # 本地 PTY 模拟 H4 控制器
```

启动顺序为：加载标准 `hci_uart` 和厂商 `sprdbt_tty` → 仅复位蓝牙 rfkill →
向 `/dev/ttyBT0` 写入 PSKey（176 字节）、RF（252 字节）并启用 →
`btattach -P h4` 注册 HCI → 标准 BlueZ／KDE 管理配对和蓝牙配置。
`0xFCA1` 回包是两字节模式和启用状态，`00 00 01` 表示启用成功；它不是标准 HCI 状态码。

模块：`wcn_bsp`、`sprdbt_tty`、`bluetooth`、`hci_uart`，桌面协议另有
`rfcomm`、`bnep`、`hidp`。系统依赖 BlueZ（含 `btattach`）、kmod、systemd、
util-linux 及基本 shell 工具。启动服务按 `w202-wifi.service` 之后排序，但不依赖
Wi-Fi 联网，不重启 Wi-Fi，也不卸载公共 WCN 总线。没有安装来源项目中的 Wi-Fi
恢复脚本、udev 重开规则或休眠钩子；这些功能需要分别适配和验证。

地址优先使用 `/var/lib/w202ds/bdaddr`，其次只读挂载本机 `prodnv` 读取 `btmac.txt`，
无有效地址时从本机 machine-id 派生本地管理地址。缓存仅留在设备，不写入固件参数或源码。

本地构建和测试：

```sh
# 在本目录；测试运行本机原生二进制。
make CROSS_COMPILE= OUT=/absolute/output/native test
make OUT=/absolute/output/arm64 DESTDIR=/absolute/output/rootfs install
```

输出 ARM64 静态程序、脚本、systemd 单元及 `../firmware/bt_configure_*.ini`。
`install` 只暂存，不加载模块或启用服务。部署时运行 `depmod -a` 和
`systemctl daemon-reload`，再启动／启用 `w202-bluetooth.service`；单元自动拉起 BlueZ。
初始化在服务内完成，BlueZ 动态发现 HCI；`Type=simple` 不表示初始化已完成。
使用 `bluetoothctl show` 核对 `Powered: yes`，不能仅看单元已 active。

本地测试覆盖真实程序的命令顺序、参数长度、地址字节序、无关／分段事件、
PSKey／RF／启用被拒绝，以及地址格式校验。2026-11-07 本机和 ARM64 均通过
`-Wall -Wextra -Werror` 构建；已在 W202DS 当前 KDE 内核上完成 HCI 启用及附近设备扫描。

2026-11-07 已与所接 Debian 主机完成 BR/EDR 数字确认配对，两端持久保存配对并互相信任；
主机到平板 L2CAP echo 3 发 3 收，零丢包。没有以 echo 结果代替业务传输验收。

经典蓝牙 Central／Peripheral 均经实际 HCI 角色与加密数据短测确认：每种角色
30×128 字节逐包往返正确，认证加密开启。发起连接的一端可能经角色切换成为 Peripheral。

待验收：文件等业务数据传输、LE／HID、A2DP／SCO、冷启动和休眠恢复。
共用芯片发生固件复位后需要重新初始化蓝牙；目前没有增加自动重置 Wi-Fi 的恢复机制。
