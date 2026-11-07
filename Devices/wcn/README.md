# w202ds-wcn-driver（Wi-Fi / 蓝牙 / 卫星定位）

```text
wcn/
├── build.mk
├── devicetree/
├── firmware/
├── README.md
└── source/
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| Wi-Fi 联网（STA） | 原配置已验证 | 2026-11-05 验证自动连接与 SSH；本轮新参数下尚未复测 STA。 |
| Wi-Fi 热点（AP） | 用户验收通过 | 2026-11-07 用户确认 Wi-Fi 5／5 GHz／80 MHz，经蜂窝出口的端到端测速 663 Mbps、103 Mbps；独立客户端另通过 WPA2、DHCP、DNS 和 HTTPS。2.4 GHz／20 MHz 对照也通过。 |
| STA / AP 模式使用 | 分别可用 | 当前需求为二选一，未提供并发可用性结论。 |
| 蓝牙设备识别、配对与传输 | 待验证 | 尚未完成 HCI 及实际蓝牙功能验收。 |
| GNSS 卫星定位 | 待验证 | 尚无实际定位结果。 |

2026-11-07：早先手机已在 2.4 GHz 下取得地址并上网，但后续重开热点又认证失败。本轮用独立客户端对照：独立 hostapd 能认证，NM 默认创建 P2P-device 时连续认证超时；移除 P2P-device 并重开热点后恢复。重启 wpa_supplicant 使 P2P-device 重建又复现，移除后再次恢复。关闭测试客户端自身省电后，无预设 IPv4 的 DHCP 广播分配也稳定通过；不能把此前漏收的 DHCPOFFER 当成 dnsmasq 故障。WPA2／CCMP、PMF 禁用，WPS 和 key_mgmt 保留 NM 默认。

`source/sprd_wlan_combo/sc2355/init.c` 新增只读布尔模块参数 `disable_p2p`，默认关闭以保留原驱动行为。本板设置 `options sprd_wlan_combo disable_p2p=1`：只上报 managed／AP，取消 P2P 和并发组合以及 software_iftypes 旁路，由 cfg80211 限制为一个 STA 或 AP 接口。参数需在模块加载时设置。KDE rootfs 的标准 modprobe 配置由外层 `bsp/integration/boot-handoff/kde-rootfs/etc/modprobe.d/w202-wifi.conf` 管理；本机 factory_mac 私有值留在设备配置，不写入通用配置。

修复模块已本地构建并安装，真机参数为 `Y`。仅卸载 Wi-Fi 叶模块、保留 WCN 总线完成重载；再次重启默认 wpa_supplicant 后未创建 P2P-device，5 GHz 热点重新认证通过。正式热点仍由 NetworkManager shared／dnsmasq 管理，无新增守护程序。用户已确认手机验收通过；冷启动和新参数下 STA 实际联网尚未另测。

当前驱动上报 2.4 GHz HT（Wi-Fi 4）与 5 GHz VHT（Wi-Fi 5），VHT 支持 80 MHz、最多两流，未报告 HE／EHT。80 MHz 下单流／双流的理论 PHY 速率约 433／867 Mbps，并非吞吐实测；本轮客户端已观察到收发均使用 VHT／80 MHz／两流，未测最大 PHY，不能由当前驱动确定芯片硬件上限。AP 与 STA 仍二选一。

### KDE 热点配置与 80 MHz 测试

2026-11-07 最初测试：NetworkManager 1.52.1 将现有 `debian-hotspot` 保存为 5 GHz、信道 36、80 MHz，保留原名称、用户指定密码和 WPA2／CCMP。`iw dev wlan0 info` 报告 `5180 MHz, width: 80 MHz, center1: 5210 MHz`，另一台主机空中扫描收到对应的 VHT80 广播；蜂窝连接同时保持 active。但手机连接后立即断开，降为同信道 20 MHz 仍失败，当时仅广播通过；后续修复结果如下。

最终配置：5 GHz／信道 36／80 MHz，中心频率 5210 MHz，监管域 CN，热点自动连接且优先级 50。独立客户端完成 WPA2／CCMP，DHCP 的 DISCOVER→OFFER→REQUEST→ACK 分配 `10.42.0.251`，经 `10.42.0.1` 解析 DNS、使用热点源地址访问 HTTPS 并通过证书校验。此结果在安装模块及重启默认 wpa_supplicant 后复测，客户端省电开启／关闭均通过。客户端实际报告 80 MHz、VHT、两流 TX 585 Mbps，属于 PHY 速率。

2026-11-07 用户验收：Wi-Fi 5／5 GHz／80 MHz 通过，用户报告测速为 663 Mbps、103 Mbps，并确认走的是基带流量，因此蜂窝联网与热点共享链路也验收通过。记录路径为手机 → Wi-Fi 热点 → 平板蜂窝出口；这是端到端测速，不是独立的 Wi-Fi 局域网吞吐。按用户提供的两项数值记录，未标注上下行方向；与上面的独立客户端 PHY 速率分别记录。冷启动和长期稳定性仍待验证。

重载限制：`modprobe -r sprd_wlan_combo` 会连带卸载 WCN 依赖；原厂 BSP 清理不完整，再加载曾报告 `wcn_pcie`／`mdbg` 等重复注册，只能正常重启恢复。本轮后续使用 `rmmod sprd_wlan_combo`，保留 `wcn_bsp`，两次替换／恢复本机 MAC 均通过。不要重复连带卸载路径；WCN 总线生命周期仍待单独修复。

未采用早先 `mgmt_reg` 重放猜测；运行对照支持闲置 P2P 固件上下文与 AP 认证冲突，固件内部原因尚未逆向确认。Wi-Fi 6 调查按用户要求暂停。

原监管域为 `00`，5 GHz 有 `no IR`；按本机所在地使用标准 `iw reg set CN` 后启动成功。持久配置为 `/etc/modprobe.d/w202-regdom.conf` 的 `options cfg80211 ieee80211_regdom=CN`，源码副本归外层 KDE rootfs 集成；未改驱动监管限制。正常重启后仍为 CN，持久配置恢复已验证。

KDE「系统设置 → 连接 → debian-hotspot → Wi-Fi」可配置热点名称、模式、2.4／5 GHz 和信道，密码在「Wi-Fi 安全性」。本机 Plasma NM 6.3.4 没有带宽选项，80 MHz 使用现有 NetworkManager 属性配置：

```sh
nmcli connection modify debian-hotspot 802-11-wireless.band a 802-11-wireless.channel 36 802-11-wireless.channel-width 80mhz
nmcli connection up debian-hotspot
iw dev wlan0 info
```

GUI 修改配置后仍应核对实际带宽。界面能力依据对应版本的 [Wi-Fi UI](https://invent.kde.org/plasma/plasma-nm/-/blob/v6.3.4/libs/editor/settings/ui/wificonnectionwidget.ui) 和 [保存逻辑](https://invent.kde.org/plasma/plasma-nm/-/blob/v6.3.4/libs/editor/settings/wificonnectionwidget.cpp)，并核对已安装编辑器的字符串；本轮未替换 KDE 组件或添加热点服务。后续验证冷启动恢复、新参数下 STA 联网和长期稳定性。

更新日期：2026-11-07
