# Marlin3 GNSS 电源接口

`w202ds_gnss_power.ko` 绑定现有 `unisoc,marlin3_pcie` 下的 `sprd,gnss` 子节点，
通过标准 GPS rfkill 调用共用 BSP 的 `start_marlin`／`stop_marlin`，不创建新的总线或复位整个 WCN。
需要 `CONFIG_RFKILL`，不需要 `CONFIG_GNSS`。模块初始关闭。

```sh
tools/bsp-build modules KDIR=/matched/kernel/output OUT=/absolute/output/modules w202ds-wcn-gnss
sudo modprobe w202ds_gnss_power
```

rfkill 名称是 `w202ds-gnss`。平台节点只读属性 `power_requested` 表示 BSP 的 GNSS
请求位，`last_error` 是最近一次操作错误；它们都不代表物理电流、卫星信号或定位结果。
只释放本模块持有的请求，发现已有请求时返回 `-EBUSY`。原厂接口是位图而非引用计数，
不要同时运行另一个 HAL／调试程序控制 GNSS。

2026-11-08：匹配的 6.18.35 内核下，本地构建、三次开关、叶模块卸载／重载以及
替换 BSP 后重启通过；Wi-Fi、蓝牙、蜂窝服务保持 active，未重复测试无线吞吐。
WCN 故障通知仅使状态失效并屏蔽 rfkill；公共 WCN 的完整卸载生命周期、异常与休眠恢复
仍待验证。不要卸载正在使用的 `wcn_bsp`。

接收端启动、停止和 NMEA 输出由 `../../userspace/gnss` 内的独立用户态工具负责。
GNSS 电源位本身不等于导航会话，正常退出需先发 idle-on、sleep，再关闭电源请求。
