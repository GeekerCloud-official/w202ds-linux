# W202DS 电源监测与 FGU 电量

Kbuild 生成 `w202_ump9620_power.ko`。继承原厂保留运行的 FGU、NTC、ADC 与 eFuse 校准，提供系统电池 `w202-battery` 和系统外电 `w202-usb-c`。FGU 库仑计保持运行；仅按 Android 格式保存两个 RTC 电量区，不修改计数器、IRQ 或充电策略。

电池提供标准 `voltage_now/current_now/power_now`、温度、充放电状态、电量、电荷与能量。`power_now` 用实时 V×I 得到 µW，有符号；能量按估算电荷乘 DT 最低设计电压（本板 3.45 V），沿用 UPower 固定参考转换。`energy_full/design` 不随实时电压改变；满充容量仍采用 DT 的 7,286 mAh，健康度比值不是老化学习结果。

Full 状态仅在电流死区、外电存在且主充电器 Full 时传递；明显充放电仍按 FGU 电流方向判断。读取充电器状态先于 telemetry 锁，避免与充电策略反向锁序。

## Android FGU 对照与校准

依据原厂 `sc27xx_fuel_gauge.c`、本机原厂 B 槽 `sc27xx_fuel_gauge.ko` 和既有 REA 反编译。已移除工作树中的参考副本，其原始内容见 [Git 历史中的 FGU 参考实现](https://github.com/Yeamika/w202ds-linux/blob/5da38384e9462c1198215f8fc94a5bb8b8592578/Devices/pmic/source/vendor-reference/drivers/power/supply/sc27xx_fuel_gauge.c)。原厂模块 SHA256 为 `6a3642736173cf172fc92c301709ac6d140db94f1ac02cbc5223a0f4a565b4be`；保存时序另经本机模块反汇编核对。原厂充电管理器参考的 `is_full_charged()` 会在持续高电压、低电流时调用 FGU 的 `adjust_cap()`；这不是完整原厂算法移植。

- 初始化优先读取常温电量区 `0xc8`，无效时采用 UI 区 `0xa8`。两区均使用整数百分比加十分位；继续以当前库仑计数为软件锚点。
- 每 10 秒采样；外电在线、主充电器 Full／Good、电压至少 DT 的 4.33 V、电流 −5 mA 至 DT 的 +120 mA、温度 15–40°C，连续观察至少 30 秒才建立 100% 满充锚点。−5 mA 容差覆盖终止后约 −1.5 mA 的零点偏差；采样间隔超过 20 秒重新计时。
- 之后用库仑增量累计；每 60 秒、模块卸载和正常关机保存 SOC。沿原厂 SET／CLEAR、200 µs 延时写低 12 位，并读回核验；两个区的高 4 位原样保留。首启模式不改写；写失败记录 `save_error`，CLEAR 释放会始终尝试执行。
- `capacity_source` 和 `soc_state` 显示锚点、满充确认、保存状态；标准电池接口仍由 UPower／KDE 直接使用，无新增守护程序。

2026-11-06 18:22 UTC 真机候选：旧 RTC 两区都是 27.4%，电池约 4.350–4.352 V、约 −1.5 mA、29.4°C，主路 Full／Good。持续确认后两区变为 `0x5064`／`0xa064`，保存无错误，UPower 显示 100%／fully-charged。正式安装与重载复测证据见本硬件 `modules/6.18.35-w202ds-m23-wifi/fgu-android-20261107-v1/`。

TODO：完整冷启动与跨系统交接、断电期间 OCV／POCV 校正、低温／低电量补偿、整周期误差和容量学习。保存只含 SOC、不含配对的历史库仑计数；加载／关机间的消耗及异常断电前最多约一分钟的变化仍可能丢失，不能称作精确电量计。原来的 42%→27% 是重载回旧基准，不是实际骤然掉电。

USB 对象提供实际 ADC 电压、当前合同名义电压、源预算和 online / USB 类型，`scope=System`。TCPM 控制器与充电芯片的内部电源由各自模块标记 `scope=Device`；本模块通过标准电源属性读取合同，不包含其他硬件源码。

研究项目根目录使用 `tools/bsp-build modules KDIR=/匹配内核输出 OUT=/临时目录 w202ds-pmic-telemetry`，输入为本目录源码、匹配内核与 AArch64 工具链，输出模块、build.json 和日志。`tests/usb-source.c` 验证源能力／断开边界；`tests/fgu-soc.c` 可由宿主 C 编译器运行，覆盖 RTC 编码、无效值、满充条件、观察中断和计时回绕。GCC 13 配合当前 GCC 14 的内核头构建时需显式传入 `w202ds-pmic-telemetry.vars=CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT=`，不会修改内核配置。

此前 PPS 功率曾经 UPower、KDE 验证约 9 W；最新双 C 连接仍为普通 5 V，PD RX=0，不把历史成功当作当前状态。接口矩阵见 KDE profile 的 POWER-INTERFACES.md。
