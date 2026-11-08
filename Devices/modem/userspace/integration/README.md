# KDE 移动网络接入

保留本项目的驱动、原厂固件/NV 启动服务；数据连接使用 ModemManager → NetworkManager → KDE 原生网络界面。没有新增轮询守护进程。`w202-cellular` 的 AT/MM 两种后端继续保留为独立 Server 的选择，与 NetworkManager 不同时管理 CID1。

基础运行文件由上层 `make install DESTDIR=...` 安装。新 rootfs 还必须创建 `w202modem` 系统账号，将本机私有运行目录的组及可写 NV 工作文件归属改为目标账号，并确保它能穿过 `/var/lib/w202ds`。不要复制另一发行版的 UID；本次旧 989 在 KDE 已属于 dnsmasq，实际新账号为 985。目标系统执行 `systemd-sysusers /usr/lib/sysusers.d/w202-modem.conf`；持久账号不靠每次开机重新迁移。

独立 Linux 启动需要匹配内核模块、`../../firmware/boot-prepare/` 中本机已校验的两个 AVB 输入安装到 `/usr/lib/firmware/w202-diag/`，并在 `/etc/default/w202-modem-runtime` 显式设置本机私有状态路径及 `W202_MODEM_BOOT_PREPARE=1`。保留原校验，NV 不写原厂分区。

NV 必须使用完整私有镜像：`${W202_MODEM_STATE}/nv/{nr_fixnv1_a,nr_fixnv2_a,nr_runtimenv1,nr_runtimenv2}.img`。本机前两项各 52,428,800 字节，后两项各 57,671,680 字节；NV 有效载荷分别为 `0xec0000`、`0xf40000`。镜像目录 root:root 0700、镜像 0600。启动辅助工具校验后分配 loop，将对应块节点放进私有 `android/dev/block/by-name/`，节点仅向 `w202modem` 开放，禁止其它块设备出现在私有根。原厂 NV 服务会 `O_TRUNC` 加脏扇区写，不能把这些入口改回普通文件。完整且校验有效的旧工作文件可自动迁移；已有短读或校验失败必须停服，从同一设备的可信 NV 输入恢复，辅助工具不会读取或改写物理分区。

不可变运行固件可在 `/etc/default/w202-modem-runtime` 设置 `W202_MODEM_FIRMWARE_DIR=/usr/lib/firmware/w202ds/modem`，目录需为规范绝对路径、root 所有且不可被组或其它用户写入，包含 `nr_modem_a.img`、`nr_phy_a.img`、`pm_sys_a.img`、`ch_sys_a.img`、`nr_deltanv_a.img`。启动时只读绑定到私有根 `android/firmware/`，旧名称为兼容原厂程序的别名，不是原厂 A 槽入口。已有文件完整性 manifest 仍须通过。留空时继续使用已有的私有固件目录。Linux 只需一套确定版本的运行固件，NV 主副本仍保留；NV 镜像、IMEI、密钥不发布。

停服后 `unmount-root` 释放固件绑定及该私有状态对应的 loop，不能在 diskserver 运行时手动拆除。诊断可执行 `w202-modem-storage verify --state "$W202_MODEM_STATE" --check-nv`；只用于检查本机私有镜像。

固定的 ARM64 W202DS MM 包安装前缀为 `/opt/w202ds/modemmanager`，目录和文件归 root。将本目录 `ModemManager.service.d/w202ds.conf` 安装到 `/etc/systemd/system/ModemManager.service.d/`；将 MM 工程的 `src/plugins/w202ds/77-mm-w202ds.rules` 安装到 `/etc/udev/rules.d/`。依赖发行版自己的 ModemManager 单元、D-Bus 策略、NetworkManager 与桌面授权，不替换系统 libmm-glib。启动顺序为运行服务完成 AT 初始化后才启动 MM，重启运行服务也会重启 MM。

完成安装后 reload systemd/udev，先停用 `w202-cellular`，再启用 MM。首次运行中安装规则可触发 `spipe/stty_nr0` 和 `net/sipa_eth0` 的 change；后续自动扫描即可。当前中国移动 SIM 的标准连接创建方式：

```sh
nmcli connection add type gsm ifname stty_nr0 con-name 'W202DS Cellular' \
  apn cmnet connection.autoconnect yes ipv4.route-metric 900 ipv6.method disabled
nmcli connection up 'W202DS Cellular'
```

APN 按 SIM 配置。本次 IPv4 已验证，IPv6 未验收。Wi-Fi metric 600 优先，蜂窝 900 为备用。绑定蜂窝接口的流量验证须用 IPv4，避免普通 DNS/默认 Wi-Fi 路由使测试经过别的出口。实际首次普通域名 HTTPS 有超时，随后指定已解析的 IPv4 地址、保留 TLS 主机名与证书校验，经过蜂窝接口的请求成功；不把第一次超时隐藏成全程通过。

2026-11-07 在 p77 KDE 实测：MM 识别插件、SIM/5G 注册、NM 建连、主动断开重连、基带运行服务重启后自动恢复，以及恢复后的蜂窝 DNS/HTTPS。整机冷启动、移动网络断网自动恢复及 GUI 点击仍待用户验收。固定包及本轮源码/证据仅在本地 `../releases/kde-networkmanager-20261107-v1/` 保留，不随 GitHub 源码发布；板上二进制仍以该交付所记版本为准。

2026-11-08 当前源码补上 SIM 热插拔：原厂 `+ECIND` 第 3 组或注册变化触发 `+CPIN?` 确认，明确拔卡后调用 MM 标准停用／重探测流程，让 NM 收到旧 modem、SIM 和 bearer 对象移除。无卡时保留通知端口，插卡可恢复识别；不新增独立管理服务。本地 daemon 编译和模拟回归通过，ARM64 `1.24.0-w202ds-sim-hotplug-20261108` 已替换 `/opt/w202ds/modemmanager` 内的 daemon、私有 libmm-glib 和私有 mmcli。未替换发行版系统 libmm-glib，仍使用原有 systemd／NM 接入；无卡启动实测正确显示 SIM_MISSING，没有旧 SIM、注册或附着状态。基带运行服务和热点保持运行。

本轮插卡实测通过：收到 `+ECIND: 3,100` 后首次 CPIN 返回 SIM busy，后续第 3 组通知触发 CPIN READY，MM 标准重探测后由已有 NM 配置自动恢复 5G／CID1 连接。蜂窝源地址 DNS 与绑定 `sipa_eth0`、保留证书校验的 HTTPS 200 通过；MM 进程未重启，热点保持连接。联网时热拔卡由用户暂缓；KDE GUI、PIN 锁卡和长期稳定性仍待验收。

测试时通过 `/run/systemd/system/ModemManager.service.d/zz-sim-hotplug-debug.conf` 临时启用 `--debug`。测试暂停后已删除该文件、daemon-reload，并通过 MM 的 SetLogging 恢复 INFO；没有为清理而重启当前已联网进程，下次启动使用原永久命令。独立日志跟随已停止，旧运行包及本轮日志在本地保留，上传暂存已清理。没有更换内核、写原厂分区或重启整个基带运行环境。测试说明见 [插件测试](../ModemManager/src/plugins/w202ds/tests/README.md)。

同日用户验收：手机通过 Wi-Fi 5／5 GHz／80 MHz 热点使用平板的蜂窝出口，端到端测速为 663 Mbps、103 Mbps，用户确认蜂窝联网与热点共享通过；两项数值未标注上下行方向。此项不覆盖整机冷启动、长期稳定性或 KDE GUI 操作。

同日补充：NetworkManager 会自动枚举普通网口，不能只靠 ModemManager 的适配识别内部接口。将 `NetworkManager/conf.d/31-w202-sipa-internal.conf` 安装到 `/etc/NetworkManager/conf.d/`，排除 `sipa_dummy*`。`sipa_dummy0` 是 SIPA 共享接收／NAPI 辅助网口；保持驱动创建的 UP 状态，不把它关闭，也不对其运行 DHCP。真实蜂窝数据仍由 MM/NM 管理 `sipa_eth0`。

当前设备已 reload 配置，删除只绑定 dummy 的错误自动有线连接，并设 dummy 为 unmanaged、保持 UP；修复后没有新的 dummy DHCP 失败。Wi-Fi 和蜂窝分别绑定真实接口、指定实际 IPv4 并保留 TLS 主机名验证，HTTPS 均返回 200。路由器会为部分域名返回 fake IP，蜂窝专项测试应使用蜂窝 DNS 得到的实际地址，避免将 Wi-Fi 的 fake IP 用到蜂窝接口。未因此修改全局 DNS 或路由；冷启后的持久规则仍待实机回归。
