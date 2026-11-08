# w202ds-pmic-driver（电源管理与电池监测）

```text
pmic/
├── build.mk
├── devicetree/
├── README.md
└── source/
    ├── COPYING
    ├── w202-telemetry/       # 电池、FGU、ADC 与外电状态
    └── ump9620-typec/        # Type-C／PD，使用主线 TCPM
```

`source/` 保留板级实现、测试与 GPL 许可证；主线和原厂参考副本已移除，历史版本仍可从 Git 取得。FGU 来源见 [telemetry README](source/w202-telemetry/README.md)。`build.mk` 注册两个独立目标：`w202ds-pmic-telemetry` 生成 `w202_ump9620_power.ko`，`w202ds-pmic-typec` 生成 `w202_ump9620_typec.ko`，均加入 Server 模块集合。

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 电池电压读取 | 已验证 | 已取得实测读数。 |
| 电池电流读取 | 已验证 | 可读取有符号电流；充放电状态的完整校准待完善。 |
| 电池温度读取 | 已验证 | 可读取温度；测量精度与校准待验。 |
| USB 输入电压读取 | 已验证 | 已取得实测读数。 |
| 库仑计数读取 | 已验证 | 可读取累计电荷数据。 |
| 电量百分比（SOC） | 满充校正与 RTC 保存真机通过 | 对照 Android 补软件锚点校正与双区保存，旧 27.4% 已校正；冷启动、整周期误差与容量学习待验。 |
| PPS 默认启用、自动升压与固定档回退 | 早期热加载通过；本次实物重插失败 | 最新双 C 为普通 5 V／3 A 声明，主路输入上限 2 A；RX=0，无 PD 合同。默认 9.8 V 配置保留，11 V 不进默认策略。 |
| 振动马达 | 待验证 | 尚无 Linux 实物振动验收结果。 |
| 呼吸灯 | 待验证 | 尚无 Linux 灯光控制验收结果。 |
| 普通重启及 bootloader / fastboot 重启 | 待回归验证 | 已有重启处理实现，当前完整内核下的真机行为待验。 |

## 最新 FGU 与双 C 对照（2026-11-07，北京时间）

用户重新要求优先 PD 并参考原厂 Android FGU。双 C 下的显式 PD 初始化／一次块复位候选仍无源报文，已撤下并恢复正式 Type-C；当前约 5 V、Rp 3 A，主路 auto、输入上限 2 A。电池约 4.35 V、Full、约 0 mA，低输入电流与充电终止相符，不能直接当作 100 mA 软件限流。PD/PPS 仍未修复，电源输出档位待用户确认。

FGU 旧实现每次加载都回旧 RTC 27.4%，缺校正与保存。现按本机 Android 格式补双 RTC 保存、持续满充条件确认和库仑软件锚点，不复位硬件计数器。18:22 UTC 候选确认后两区为 `0x5064`／`0xa064`，UPower 为 100%／fully-charged；实现、边界与后续待办见 [telemetry README](source/w202-telemetry/README.md)。交付目录为 `modules/6.18.35-w202ds-m23-wifi/fgu-android-20261107-v1/`。以下为阶段记录。

## 实物复测（2026-11-07 01:13–01:19，北京时间）

用户已完成真实拔线和插回。拔线 VBUS 0.159 V、STATUS=0、attempts 清零，DBG1[9] 仍为 1，旧门控错误已获实测证据。插回能回普通 5 V／500 mA，auto 保留、从路关闭；但 RX=0，PD/PPS 没有恢复。在线 113 次采样电池净功率最高约 0.343 W；此前 9 W 为历史成功结果。当前 Rp 为默认档，和此前成功时 Rp 3 A 不同，连接路径仍待确认，不能直接归因充电器或本机。此次没有故障锁存，故真实 fault 后解锁仍未验。模块及策略未改、未重载；详细结果见 source/ump9620-typec/README.md，原始证据在 modules/6.18.35-w202ds-m23-wifi/pd-physical-replug-20261107-v1/。

## 原厂流程对照（2026-11-06 16:29 UTC）

用户测试器约 2.4 W／5 V／0.5 A。原厂公开 `sc27xx_pd.c`／`sprd_tcpm.c` 与本机原厂 `sc27xx_pd.ko` 都已取得；此次核对本机二进制的插拔、时钟和协议清理路径，补正常拔线 FIFO／消息 ID 清理、按原厂状态判断断开、AON 时钟回读和 Type-C VBUS 比较器。另声明标准 self-powered 供主线 TCPM 使用；独立复位上限跨 CC 开合保留。当前安装 `pd-cc-rearm-20261106-v1`，初始化的 CC 断开／重接与普通 5 V 回退已看到，PD 仍 RX=0，PPS 未恢复，用户物理长拔线与故障解锁待验证。详细差异及本机二进制校验见 source/ump9620-typec/README.md；不将缺失 AON 操作等同于已确定根因。

## 先前停充排查（2026-11-06 15:56 UTC）

当前 KDE 已安装受限 PD 恢复及 telemetry Full 状态传递版，交付在 `modules/6.18.35-w202ds-m23-wifi/charge-full-pd-20261106-v1/`。旧控制器的一次 Hard Reset 永久锁存已改为两次连续复位预算、稳定合同后补充预算、真实 CC 拔线解锁。补齐默认 Rp 的 500 mA 回退接口，避免 PD 失败后 TCPM 返回 0 mA 导致完全停充。此刻只恢复 5 V 低功率充电，电池约 +0.32 A／1.4 W；源 RX=0，快充未恢复，物理重插和长期回归待验。Full 只传递充电器状态，不校准 SOC；热加载后旧 RTC 锚点回到 27%，旧 42% 也不是准确电量。下面为此前阶段记录，不代表最新协议状态。

后续 15:56:39–15:58:08 的 45 次采样：40 次 Charging、5 次 Full，充电期间净功率均值 1.402 W；末端主芯片按终止条件报告 Full，电池约 4.357 V、电流约 0，UPower 15:58:34 为 fully-charged。SOC 仍只估算 28%，Full 不是准确容量校准的证明。PD 未收到源端消息（RX=0、失败发送=3），两次有限复位没有恢复快充；不将当前 fault=none 误记成协商成功。

## 断电重接联动修复

用户验收发现重新上电后约 1 W，真机实际主路已误入永久停充。修复位于 charging worker／PPS 客户端：临时退出失败保留 auto，退出有独立待完成状态及重试间隔，并保留短暂 extcon 断开事件。当前 PMIC／Type-C 与 telemetry 二进制未修改；普通服务加载新 charging 后 PPS 9.8 V 恢复。真实再次拔插验收与最新结果见 charging README 和该硬件 `pps-replug-20261106-v1/` 交付，旧记录不替代新版本验收。

## 当前 PPS 结果（2026-11-06）

本轮默认已改为 `activate=1 pps=1 target_mv=9000 pps_max_mv=9800`，Type-C 加入 Server 模块构建集合，外层启动脚本先准备双充电芯片 OVP，再加载控制器并启用主路自动充电。charging 模块自动选择 PPS、按源与板上限升至最高 9.8 V，默认请求电池 2.64 A，保持全部原厂范围与保护；无新守护程序或内核／UEFI 修改。14:13–14:15 UTC 无参数热加载、自动升压、停充回固定档和恢复 PPS 已通过，PD errors=0、fault=none。当前轮次交付为 `modules/6.18.35-w202ds-m23-wifi/pps-default-20261106-v1/`，安装验证随 evidence 保存；完整重启、物理拔插及长期回归待验。后文 13 点和固定档段落为历史记录。

14:17 UTC，当前 KDE p77 已安装默认策略模块和开机脚本；卸载后经普通 `w202-power.service` 从磁盘无参数加载，自动 PPS 9.8 V／预算 3 A、PD 无错误，服务 active/enabled。旧模块及脚本备份在设备 `/var/lib/w202ds/pps-default-20261106/before/`。无需修改 telemetry、内核或 UEFI，未执行整机重启。

后续 KDE 联调补充标准 `power_now`、估算能量与供电 `scope`：UPower 与 KDE 系统监视器已读取约 9 W，停充／恢复通过。内部 TCPM／充电芯片为 Device，电池与唯一外电为 System，避免 PPS online=2 被 UPower 当作反复插拔导致持续 0 W。电流仍为标准有符号 `current_now`，UPower 1.90.9 没有原始 Current 属性；KDE 完整接入情况见研究项目 KDE profile 的 POWER-INTERFACES.md。能量／健康度仍是基于原 DT 与 SOC 的估算，未改成虚构的精确测量。

`ump9620-typec/` 使用原有 Linux TCPM；PPS 通过标准电源接口控制，charging 复用既有工作队列续约，不增加守护程序。telemetry 的 `w202-usb-c/voltage_max` 取 TCPM 当前合同的 `voltage_now`，不把源 APDO 的最高 11 V 误当作正在使用的电压；自己的 `voltage_now` 仍是 ADC 实测。

13:18:49–13:20:49 UTC 的 60 次采样全部保持 PPS 9.8 V、主路 Charging / Good：ADC 9.079–9.119 V，电池净电流 +2.101–2.174 A、净充电功率 8.80–9.09 W（平均 8.99 W），温度 29.9–30.1°C。随后停充、退出至固定 9 V、重新进入 PPS 并小步回 9.8 V 均通过；收发错误为 0、fault=none。从路保持停充，双路 REG06=0xe6。3 A 是 PPS 协议预算，不能当成 USB 实测电流或功率；本轮未取得测试器的 PPS 功率读数。

按原厂 B 槽 DT／解析器，快充电池电流为 2.65 A、输入限流为 1.25 A；charging 向下取硬件档位 2.64 A／1.2 A。PD 固定档保持原厂 18 W 上限；PPS 请求在发送 FIFO 前独立检查源和板级边界，最高 11 V／3 A，56942 个 PPS 边界组合及固定档测试通过。原厂 APDO 的 6 A 只是声明，不代表已验证可用。

10.2 V 只曾短时充电，严格复测在约 10.1 V 已出现从路 PG 掉线；小步测试在约 10.4 V 输入掉线，11 V 虽完成协议，ADC 仍约 0.159 V。不能把协议成功视为可用充电档，也不能把这些点当作精确硬件阈值。退出 PPS 后固定 9 V 可恢复，无需拔线；切断位置仍未定位，测试器／前端／瞬态待排查。当前输入丢失会尝试退出 PPS 并回固定档，只有常规充电检查通过才恢复；退出失败保持停充。此故障分支尚未单独触发验证。

上述 PPS 阶段仅 RAM 热加载，产物及证据在 `modules/6.18.35-w202ds-m23-wifi/pd-pps-20261106-v1/`。后续 KDE 功率修复版已安装到当前 rootfs 的模块目录并 depmod，交付为 `kde-power-20261106-v1/`，配套 charging 同名目录；旧模块保存在设备 `/var/lib/w202ds/kde-power-20261106/before/`。该阶段保留 PPS 9.8 V，但当时启动配置未改，重启仍按普通充电配置；本轮自动策略已取代该配置。没有修改内核、UEFI 或引导分区，重启回归待验。下面保留固定档的阶段记录。

## USB 供电能力（2026-11-06，阶段记录）

现有 `source/w202-telemetry/` 保持只读 FGU / NTC / ADC，并增加标准 `w202-usb-c` 电源的 `online`、`voltage_max`、`voltage_now`、`current_max`。`voltage_max` 采用 TCPM 合同名义电压、无合同时回退 5 V；`voltage_now` 为 PMIC ADC 实测，不能混用。从同一 PMIC 的 CHARGE_STATUS 与设备树 Type-C 基址读取实时状态；只有 attached-sink、检测使能与 VBUS_OK 有效才接受 Rp 的 1.5 A / 3 A，BC1.2 须完成检测且为明确 DCP / CDP。未知源只给 500 mA，断开为 0；读取失败返回错误。该模块没有 PMIC 初始化或 PD 写入。

本机只读 Type-C STATUS=`0x6082`，对应 attached sink、Rp=3 A；标准源能力实测 5 V / 3 A。充电驱动按此选择至多 2 A 输入，实测电池约 +1.49 A。源码 `w202_usb_source.h` 的主机测试包含正常能力及 124 个无效连接状态组合；在研究项目根目录用宿主 C 编译器编译 `bsp/Devices/pmic/source/w202-telemetry/tests/usb-source.c`，输出放 `output/`。

构建仍使用 `tools/bsp-build modules ... w202ds-pmic-telemetry`，输出独立 `w202_ump9620_power.ko` 与构建回执。正式产物放 `modules/6.18.35-w202ds-m23-wifi/normal-charge-20261106-v1/`，匹配充电模块同名版本。SOC 仍是保存锚点的估计，重新加载模块会重建锚点；当前验证依据真实电流与库仑计数差值。

## Type-C / PD 实验（固定档阶段记录）

新增 `source/ump9620-typec/`，独立目标 `w202ds-pmic-typec` 生成 `w202_ump9620_typec.ko`。硬件回调对接原有主线 TCPM，未复制厂商协议栈；初版默认只读诊断；当前默认策略见上方说明，`activate=0` 仍可诊断。

首测 PD IRQ 和 RX 为 0，TCPM 超时后的多次 Hard Reset 导致供电反复中断、测试器闪烁。恢复原厂完整中断掩码后复测，IRQ=13、RX=7、TX/完成各 5、错误=0；Request / Accept / PS_RDY 完整，进入 SNK_READY，固定 **5 V / 2.4 A** 合同已建立。候选同时锁存第一次复位请求，阻止连续电气复位；故障分支尚待单独验证。详细边界与记录见 [控制器 README](source/ump9620-typec/README.md)。

固定 5 V 联调时控制器、telemetry 及 charging 通知联动版已热加载，未覆盖磁盘安装模块；重启恢复之前的普通充电版。telemetry 通过标准 TCPM 属性将预算从 Rp 3 A 收敛到合同 2.4 A，主路仍限 2 A，电池净充电约 +1.43–1.49 A。已补充 PD_PPS 类型，避免 sysfs USB 类型读取失败，并把它纳入 PD 预算；TCPM 的 PD_PPS 表示源支持 PPS，ONLINE=1 和固定 PDO 请求才表示实际固定合同，未启用 PPS。

用户测试器实测 4.7 V / 1.5 A（约 7.05 W），不是 2.4 A 实测输入。平板 ADC 充电时约 4.50–4.57 V；标准停充后升至 4.976–4.990 V，恢复充电后约 4.553 V，电池净电流恢复 +1.478 A。压降与充电负载相关，具体供电端／线材／测试器／接头位置及 ADC 精度未隔离，合同 5 V 不能代替物理测量。

5 V 产物与证据在 `modules/6.18.35-w202ds-m23-wifi/pd-tcpm-20261106-v2/`。

## 固定 9 V / 12 V 短测（2026-11-06，PPS 之前）

新增 `target_mv` 显式档位及标准电源属性联动，不改内核或 UEFI；高压前由 charging 准备两颗芯片 OVP，确认实际输入后才开主路充电。

| 档位 | 结果 | 实测与边界 |
| --- | --- | --- |
| 9 V | 短测通过，未持久安装 | 固定 9 V / 1.5 A 合同；用户表 9 V / 0.8 A，约 7.2 W。20 次采样 ADC 8.378–8.404 V、电池 +1.477–1.480 A、29.2–29.3°C；Charging / Good，PD 无错误。 |
| 12 V | 实际供电失败，未开启电池充电 | 协商到 SNK_READY，但用户表 12 V / 0.01 A，ADC 0.159 V、两路 PG=0。软件回退默认 5 V 未恢复输入；用户重接后回到 5 V，再恢复 9 V 正常充电。 |

合同电流是上限，不能当作 USB 实测电流；两侧电流经过降压转换，不能直接比较。12 V 失败位置仍未定位，不绕过输入保护或再次请求 12 V。模块重载不能保证源端降压，降低 OVP 前必须验证实际输入。配套三模块与证据在 `modules/6.18.35-w202ds-m23-wifi/pd-voltage-20261106-v1/`。

12:20 UTC 恢复后再次通过 9 V 的 20 次采样：ADC 8.365–8.404 V、电池 +1.480–1.483 A、29.4–29.5°C，主路 Charging / Good、PD fault=none；双路 REG06=0xa6。该阶段主路 auto、从路停充，尚未持久安装。

TODO：物理拔插后自动重建和选择 PPS、长时间运行／充满与用户验收，定位 11 V PPS 输入掉线，验证故障与睡眠分支；固定 12 V 按用户要求暂缓。SOC 持久基线和充满校准仍待做。

更新日期：2026-11-06
