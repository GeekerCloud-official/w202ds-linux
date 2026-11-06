# CH 硬件时钟映射诊断

从 DT 查找 `sprd,syst-timer` / `sprd,sysfrt-timer`，只读硬件计时寄存器，构造原厂 40 字节 AP/CH 映射。默认 `send=0` 只打印，不发送；`send=1` 发送一次，不消费 RX 环、不复位协处理器。

CM2 独立构建目标是 `sensorhub-timesync`，产物位于 `OUT/sensorhub-timesync/w202_hub_timesync.ko`。匹配 M23 与 SIPC 导出符号，使用公共 Devices/Makefile。实机默认只读、单次发送和随后的四类数据流已验，见 `archive/linux/server-sensor-data-20261104-r63/r63-hub-timesync-test.json`。

1 kHz 定时器映射使用 factor=1000000/shift=0，32768 Hz 使用 factor=1953125/shift=6。原始 Hub 事件 timestamp 观测为微秒，不能与输入映射的纳秒混用。

正式 `w202-hub-iio` 已集成周期时钟同步，本诊断不作为其运行依赖；单独测试结束正常卸载自己的模块，不触及 SIPC/SIPA。
