# CH 原始协议诊断

`w202-hubctl` 是 Perl 客户端，不需要 Time::HiRes，用现有 spipe 接口执行有界查询/采样。不会复位 CH 或卸载 SIPC。已完成 selftest、实机身份查询和时钟同步后的四类事件采样。

```sh
perl w202-hubctl selftest
sudo perl w202-hubctl info
sudo perl w202-hubctl sample --period 10
```

`--period` 直接使用原厂 wire 值，不能把它当成已验收的 Hz。可选 `--host-normal` 发送原厂 host-normal 命令。两个环各自拼包，检查长度和 CRC；退出时向本次启用的传感器发送 disable，并报告清理失败。

仅 query/batch/enable 不足以让本机 CH 输出数据；先需要原厂硬件时钟映射，可由同级 `w202-hub-timesync` 诊断模块发送。本工具与 `w202-hub-iio` 都消费相同 RX 环，使用前应确认另一采样所有者已停止。

实机时钟同步后 accel30/mag30/gyro28/light31 帧及四项 disable 记录见 `archive/linux/server-sensor-data-20261104-r63/r63-hub-timesync-test.json`。动作、光照与标定仍待验。
