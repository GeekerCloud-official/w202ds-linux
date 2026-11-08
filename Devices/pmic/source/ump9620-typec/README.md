# UMP9620 Type-C / PD

适配现有 UMP9620 PHY、FIFO、CC 与中断，协议状态机使用未修改的 Linux 6.18 TCPM。控制器属于 PMIC；充电芯片配置与策略归 charging，通过标准 power_supply 协作，不包含对方源码。

## 最新双 C 对照（2026-11-07，北京时间）

用户确认新线为双 C。18:07–18:12 UTC 将已有初始化候选临时加载 30 秒，显式 `pps=0 target_mv=5000 reset_pd=1`，结果仍 RX=0：Soft Reset 发送失败、两次有限 Hard Reset 后回普通 5 V。块复位与初始化执行并未恢复收发，不把复位成功记为 PD 成功，也不据此归因线材或充电器。原始记录归档在研究项目 `archive/linux/pd-cc-20261107-v2/`。

候选已撤下，正式 Type-C 模块 SHA256 `21920946d4ba6caf573343fa8e1aae6a3cddd7b5175b13c1432476ac5a991fb6`，PPS 默认允许 9.8 V 的原设置保留。最新为普通 5 V、Rp 3 A、无 PD 合同、RX=0；主路按原厂 2 A 输入上限工作。低输入电流期间电池约 4.35 V、主芯片 Full；FGU 旧 27.4% 的校正归 telemetry，不能通过取消充满终止来提高输入电流。充电器型号／输出档位尚待用户确认。当前源码中的显式初始化和可选 reset_pd 仍属未安装实验，默认 reset_pd=false。

## 实物拔线与重接复测（2026-11-07 01:13–01:19，北京时间）

用户确认拔线后，VBUS ADC 0.159 V、STATUS=0（DETACHED_SNK）、detach IRQ=0x0002，重试 attempts=0；DBG1 仍为 0x0200，证实旧 DBG1[9] 门控不能判断未拔线。本次拔线前 fault=none，所以 rearms=0，尚不能据此认定故障锁存解锁已真机通过。

用户确认插回后，attach IRQ=0x0001、STATUS=0x6002（ATTACHED_SNK、默认 Rp）、VBUS 4.712–4.738 V。TCPM 等待电源能力、进行两次有限 Hard Reset 后回到普通 5 V／500 mA，RX 仍为 0，没有 Source_Capabilities／Accept／PS_RDY，也未进入 ONLINE=2。auto=1、无温度或充电芯片故障锁存，从路保持 inhibit-charge。150 次两秒采样中 37 次离线、113 次在线；在线电池净功率最高约 0.343 W，主路能充电，但净电流随系统负载跨越状态死区。不能再用此前约 1.4 W 或约 9 W 代替本次结果。

此前 PPS 成功时 STATUS=0x6082（Rp 3 A）、PHY_CFG2=0x0880；本次为 0x6002／0x0080。差异已保存，PHY_CFG2 未知位含义不明，未盲目写回。当前是否使用相同充电器、测试器和线尚待用户确认，尚不能区分源端／连接路径与本机收发故障。本轮全程只读，未重载模块、未修改策略或新增复位；原模块、原厂保护和回退保持，三模块磁盘哈希核对未变，无需重复构建。实物断开识别与普通充电恢复通过；PD/PPS 自动恢复失败，完整故障解锁和重启仍待测。

原始日志、寄存器、UPower 和哈希证据见 PMIC modules/6.18.35-w202ds-m23-wifi/pd-physical-replug-20261107-v1/。原厂流程对照为此前版本开发记录，见下方。

## 原厂流程对照（2026-11-06 16:29 UTC）

用户测试器读到约 2.4 W、5 V／0.5 A；16:18 实机仍为普通 5 V，电池净充电约 1.4 W，PD 已 `hard-reset-budget-exhausted`。期间 TCPM 记录 CC 断开／重接，但上一版用未经验证的 DBG1[9] 阻止了拔线解锁。原厂源码只定义该位，没有用它判断拔线。本版改用 detach 中断加原厂 DETACHED_SNK 状态；同时增加独立的每 60 秒最多两次复位限制，探测器抖动或软件 CC 开合不会清空该上限。

已读取本机原厂 B 槽 `sc27xx_pd.ko`（SHA256 `1518271aac7ee8bb8c509d1636f2fc7b0b0dd6fa77a4dfac28456985dd50951f`），与参考源码逐项核对：

| 路径 | 原厂行为与本版改动 |
| --- | --- |
| 插拔判定 | `sc27xx_pd_check_vbus_cc_status` 读 STATUS[4:0]；本版不用未知调试位代替状态。 |
| 协议清理 | 原厂每次连接变化调用 `sc27xx_pd_reset(pd, true)`；二进制 0x323c 传 true、0x3248 调用 0x1134。补正常拔线时清 FIFO、TX/RX ID 和旧合同标志，不再只清故障连接。 |
| AON 时钟 | 原厂在检查 CC 时设置 syscon 0x350 bit 5，再使能 PMIC 时钟。本版使用原 DT 的 `sprd,syscon-aon-apb`，初始化／CC 事件时设置并回读；退出仅恢复该位。现场原值 0x8424 已含 bit 5，因此缺少此步骤不能直接证明是此次故障原因。 |
| VBUS | 使用原厂 Type-C DBG1[8] 比较器；不再拿主充电器输入保持位充当 TCPM 的 VBUS。 |
| 主线端口复位 | 声明标准 `self-powered`，本板可由电池供电；TCPM 初始化时允许短暂断开 CC 再接入。此为主线适配，不声称是原厂属性。 |

16:28 最终模块已安装，SHA256 `21920946d4ba6caf573343fa8e1aae6a3cddd7b5175b13c1432476ac5a991fb6`。已看到初始化期间两次 CC 中断、断开后重接、普通 5 V／500 mA 回退；仍 RX=0，有限复位没有恢复 PD，不能标 PPS 修好。保持普通充电，实际用户长时间拔线与故障后解锁仍待验证。原厂公开驱动依赖私有 `sprd_tcpm` 和端口字段，不能直接把 Android .ko 装入当前 6.18 内核；移植的是其硬件流程，协议继续使用现有主线 TCPM。交付为 `pd-cc-rearm-20261106-v1/`，包含原厂反汇编证据及阶段补丁。

```text
ump9620-typec/
├── Kbuild
├── controller.h
├── registers.h
├── core.c             # 端口、CC、参数、生命周期与诊断
├── hardware.c         # 校准、初始化与恢复
├── protocol.c         # 收发、IRQ、源能力缓存与请求限值
├── recovery.c         # 有限次数的协议复位、稳定合同与拔线解锁
├── supply.c           # 内部TCPM控制电源的标准scope扩展
├── request-limits.h   # 最终发送边界检查
├── tests/              # 请求边界与生产恢复路径回归
└── README.md
```

## 构建输入与输出

研究项目根目录运行 `tools/bsp-build modules KDIR=/匹配内核输出 OUT=/临时目录 w202ds-pmic-typec`。输入为本目录源码、内核配置／符号表和 AArch64 工具链，输出 `w202_ump9620_typec.ko`、日志和 build.json。依赖标准 typec / tcpm，不改内核或 UEFI；已加入 Server 模块构建集合；系统启动脚本在充电芯片 OVP 准备后加载。

## 功能与接口

默认 `activate=1 pps=1 target_mv=9000 pps_max_mv=9800`，声明 sink / USB device，优先 PPS，由 charging 的标准电源客户端自动进入、逐步升压并续约。`activate=0` 保留只读快照诊断。加载前确认两颗充电芯片 VOLTAGE_MAX 覆盖所声明上限，否则不激活。默认 9.8 V 已实测，不自动尝试输入掉线的 11 V 或固定 12 V。

固定合同按源 PDO、3 A 板级预算和原厂 18 W 上限选择 9 V／5 V。显式设置 `pps=0 target_mv=5000` 可只用固定 5 V；实验参数仍接受 `target_mv=12000` 或 `pps_max_mv=11000`，不代表该板输入已通过，不属于默认策略。合同预算不是实测输入电流。默认 charging 的 max_input_mv=12000 只准备高压 OVP，不请求固定 12 V。

标准 TCPM 电源 `online=2` 激活 PPS，`voltage_now`（µV）调压，`online=1` 回固定 9 V；源端 PPS 上限为 11 V，不能请求 12 V PPS。TCPM USB_TYPE=PD_PPS 仅表示源支持，实际合同必须核对 ONLINE=2 和 APDO Request / Accept / PS_RDY。

匹配 TCPM 的 PPS 选择未取 sink APDO 交集，本模块缓存 Source_Capabilities，在写 FIFO 前验证 RDO 对象序号、PDO 类型、电压与电流，同时检查源能力和板级上限；超界不发送。宿主测试涵盖 56942 个 PPS 电压／电流边界组合及固定档、无效序号、缩小源能力与未启用 PPS 情形。set_current_limit 同样接受范围内的中间电压。

PMIC telemetry 的 w202-usb-c/voltage_max 为 TCPM 当前合同名义值，voltage_now 为 ADC；TCPM 自身 voltage_max 在 PPS 中表示源 APDO 上限，不能当作当前输入，否则会误判过压停充。

`supply.c` 用现有 `power_supply_register_extension()` 仅补充 TCPM 电源的 `scope=Device`，所有协议属性保留；模块退出时先注销扩展、释放电源引用，再注销 TCPM。系统实际输入由 `w202-usb-c` 的 `scope=System` 提供，避免桌面重复计入内部控制电源。修复 UPower 1.90.9 将 PPS online=2 当布尔值、持续重置功率等待期导致 0 W 的问题，没有修改内核 TCPM 或 UPower。

## 实验结果与限制

5 V 固定合同、9 V 固定充电，以及 5–9 V PPS 调压、续约和固定档回退已短测通过。最终 9.8 V PPS 连续两分钟，电池净电流 +2.101–2.174 A、净功率平均 8.99 W、29.9–30.1°C；停充、固定 9 V 回退和重新激活通过，PD 无错误。3 A 仅协议预算；实际充电受原厂输入／电池策略限制。10.2 V 仅短时有效，严格复测约 10.1 V 已见一路 PG 掉线；小步约 10.4 V 起实际输入掉线，11 V 只有协议成功，不能视为可用档位。退出 PPS 后固定 9 V 可恢复，切断位置未定位。该阶段交付见本硬件 `modules/6.18.35-w202ds-m23-wifi/pd-pps-20261106-v1/`。后续 `kde-power-20261106-v1/` 增加标准 scope、保持 PPS 验证通过，当时模块文件已安装、启动配置未改；本轮已补默认 PPS 参数与外层自动充电启动集成，最终验证见上级 README。

首次 PD 移植漏掉原厂完整中断掩码，IRQ/RX 为 0，超时后重复 Hard Reset 导致测试器闪烁。恢复 0x85ff 掩码及故障锁存后收发正常。初版 PPS 续约把读取的旧电压写回，曾覆盖并发调压，现改为重申电流预算，保留 TCPM 锁内的最新电压。

2026-11-06 15:45 实机再次停充时，旧控制器锁存在 `hard-reset-blocked`，且主充电器报告 Full；不能归因于系统休眠。15:46 重载仍未恢复 PD，首次 Soft Reset 即发送失败。新版允许最多两次连续 PD Hard Reset；只有合同连续维持 60 秒或实际 CC 断开才恢复预算，PPS 续约不重置稳定计时。收到源 Hard Reset 同样走有限恢复并通知 TCPM；发送超时后的 Hard Reset 可清理旧的未完成消息。连续失败／I/O 错误仍锁住 PD 引擎，保留 Rd 和 Type-C IRQ；真实拔线可重置本地协议、解除锁存并通知 TCPM，不再要求重载模块。只修改协议，不复位整机、不主动提供 VBUS。

补齐 TCPM `get_current_limit`：默认 Rp 回退已有的 500 mA USB 预算；此前缺少此接口时返回 0 mA，导致无 PD 合同时整个充电策略停充。15:56 已实测回到 5 V、500 mA 输入预算并保持电池净充电约 +0.32 A／1.4 W。此时 RX=0，驱动未收到源端消息，有限 Hard Reset 未恢复快充；不能把 `fault=none` 或两次重试本身称为 PD 成功。物理重插及新一轮 PPS 恢复仍待实物验证。15:58 主充电器随后报告 Full、净电流约 0，不再强行重复开启充电。

`controller_state` 增加累计 resets、当前 attempts、faults、rearms 和 last_fault，保留故障证据；errors 包含失败发送。诊断不读 FIFO 或清中断。`tests/recovery.c` 直接执行生产 recovery.c，验证重试上限、禁止连续复位、60 秒稳定预算、PPS 续约、时钟回绕和 I/O 失败保持锁存。完整规范恢复仍未完成。

历史固定 12 V 协商成功但输入 ADC 0.159 V、两路 PG=0，未开启电池充电；外表 12 V/0.01 A。重载默认 5 V 未使源降压，物理重接后才恢复，原因仍未定位。不绕过板级输入保护，也不能把原厂 PPS 11 V 声明当作整板固定 12 V 已验证。降 OVP 前须验证真实输入已下降。

## TODO / Experimental

物理拔插自动重建的实物验收、完整重启、长时间充满、睡眠恢复、所有故障分支及用户验收；完整规范恢复、源角色与 VCONN。11 V PPS 输入掉线待查；固定 12 V 定位按用户要求暂缓。新增输入丢失退出 PPS 的分支尚未单独触发验证。

## 来源

寄存器／eFuse 校准来自原厂 [sc27xx_pd.c](https://github.com/Yeamika/w202ds-linux/blob/e517c65be6a13e7ae8bce64a5813622c9e56fdb0/Devices/usb3/source/vendor-reference/drivers/usb/typec/tcpm/sc27xx_pd.c)，CC 参考原厂 [sc27xx_typec.c](https://github.com/Yeamika/w202ds-linux/blob/e517c65be6a13e7ae8bce64a5813622c9e56fdb0/Devices/usb3/source/vendor-reference/drivers/usb/typec/sc27xx_typec.c)，保留原作者许可证；参考副本已从当前源码树移除。原厂限制核对 B 槽 DT、sqc_bq2560x.ko、sprd_battery_info.ko 和匹配电池解析源码。标准接口依据本项目 kernel 的 tcpm.c、pd.h、tcpm.h 与 power_supply_class 文档。
