# w202ds-cpu-driver（处理器调频）

```text
cpu/
└── README.md
```

CPU 调频使用内核树内的 `sprd-cpufreq-v2`（`kernel/drivers/cpufreq/sprd-cpufreq-v2*`）和 `sprd_sip_svc`，
均为内建驱动，没有独立 `.ko` 构建目标；所需内核选项见 `config/fragments/cpu-dvfs.config`。

调频链路：设备树 CPU 簇参数 → SPRD SIP 固件 DVFS_INIT → 固件返回的频率 / 电压表 → 运行时生成的 OPP 和 cpufreq 表 → schedutil → 固件按频点索引调压调频。
Linux 不直接设置 CPU 电压，设备树不需要静态 `opp-table`。

| 策略 | CPU | 内核 | 频率范围 |
| --- | --- | --- | --- |
| policy0 | 0–3 | Cortex-A55 | 614.4 MHz – 2002 MHz，10 档 |
| policy4 | 4–6 | Cortex-A76 | 614.4 MHz – 2210 MHz，11 档 |
| policy7 | 7 | Cortex-A76 | 614.4 MHz – 2210 MHz，11 档 |

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 三簇调频注册 | 已验证 | 三个策略均由 `sprd-cpufreq-v2` 管理，`stats/time_in_state` 记录到多档频率切换。 |
| 调频策略 | 已验证 | 默认 schedutil；另编入 performance、powersave、userspace、ondemand、conservative。 |
| 延迟初始化 | 可用，有约 45 秒空窗 | 驱动探测后等待 `voltage_offset_uv` 写入，45 秒内无写入则以原厂电压（偏移 0）初始化并注册 cpufreq；此前 CPU 不调频，CPU 散热设备也尚未创建。 |
| 电压偏移（降压 / 升压） | 不支持 | 只验证过默认偏移 `0,0,0`；本机固件的非零偏移安全性未确认，不提供也不建议降压。 |
| 温控限频 | 待验证 | `cpufreq-cpu0/4/7` 散热设备已绑定 `soc-thmzone`；负载下的实际限频效果见[温度监测与热管理](../thermal/README.md)。 |

更新日期：2026-11-08
