# W202DS GNSS

独立的 Linux 用户态导航工具，配合 `w202ds_gnss_power.ko` 使用现有 `/dev/data0_gnss`。
通过 `poll` 等待事件、校验及拆分原厂二进制帧，启动导航后将校验通过的 NMEA 输出到 stdout，
有限的诊断信息写 stderr。只依赖 Python 3 标准库，不加载 Android 库。

```text
gnss/
├── README.md
├── test_protocol.py         # 真机协议向量、分片、损坏数据与 NMEA 校验测试
└── w202ds_gnss.py           # 电源请求、唤醒、启动、输出、停止与休眠
```

## 使用

先安装与运行内核匹配的 WCN BSP、GNSS 电源模块和 `gnssmodem.bin`。
固件使用 Linux firmware loader 的标准目录，通常为 `/usr/lib/firmware/gnssmodem.bin`；
不读写 Android 固件分区或 NV。运行需要访问 rfkill 和导航字符设备的权限。

```sh
sudo modprobe w202ds_gnss_power
sudo python3 w202ds_gnss.py --duration 60 > navigation.nmea
```

默认观察 30 秒，允许 1–3600 秒，可为桌面观察选择较长的有限会话。测试结束或 SIGINT／SIGTERM 时，发送停止导航和休眠命令，
等待应答后屏蔽自己取得的 GPS 电源请求；清理等待各有 1 秒上限。不重置共用 WCN。
通过 `/run/lock/w202ds-gnss.lock` 排除本工具的多实例；已有 GNSS 电源请求时拒绝运行。
该锁不能协调不遵循它的 Android HAL／其他调试程序，不要同时启动这类程序。

可选 `--raw /private/new-file.bin` 保存原始接收流，文件须不存在，权限为 0600。
该文件可能包含位置，属于本地诊断资料，不应提交仓库。

```sh
python3 -B -m unittest discover -s . -p test_protocol.py -v
```

## 已验证与限制

2026-11-08，运行内核 `6.18.35-w202ds-00002-g53bb26e2e0d5`，没有替换内核或 DT。
在 GNSS BSP 修复已载入的系统上验证固件启动、唤醒、版本查询、导航启动应答，以及 NMEA
输出。纠正间隔配置后，12 秒测试得到 82 条语句；含退出清理的 10 秒测试得到 66 条，
包含 GGA／RMC／GLL／GSA／GSV／VTG。停止导航、休眠均收到正确应答，最终 GPS rfkill 关闭。
GSV 最高报告 3 颗 GPS 卫星；RMC 为 `V`、GGA 质量为 `0`，没有获得有效位置。

输出周期参数是导航 epoch 数，设置 `1`；不是将毫秒 `1000` 原样传入。
此传输上的 NMEA 为 `0x0c` 类别的文本，不能把旧平台的同类二进制布局直接套用。
独立 NMEA 模式关闭 Android CMCC 输出覆盖，不提供 SUPL、星历持久保存或位置伪造。
当前内核未启用 `CONFIG_GNSS`，本工具不注册 `/dev/gnss0`。

随后 300 秒连续观测得到 3184 条 NMEA、帧解析错误为 0，并由标准 gpsd 返回真实
`mode:3` 定位；观察到 GPS／北斗数据和 7 颗卫星参与解算。xgps 已显示卫星图、
信号和经纬度等信息；Marble 已下载并显示道路／地名，但其原生 gpsd 插件在本机
积压报告，实时标点仍待修复。定位精度和长期稳定性尚未验收。

- TODO：开阔环境下的独立冷启动与精度验收。
- TODO：Marble 实时标点、GeoClue、自动加载独立验证、休眠恢复及长时间稳定性。
- 实验中：仅本机固件版本经过验证；桌面使用有限会话，未启用开机常驻。

## 桌面观察

使用发行版的 `gpsd`、`gpsd-clients`、`socat` 和可选 `marble`，不创建专用地图程序。
先启动有限的本地 NMEA feed，再让 gpsd 以只读模式读取它，监听均限于本机：

```sh
sudo socat -u 'EXEC:/usr/libexec/w202ds-gnss --duration 1800' TCP-LISTEN:10110,bind=127.0.0.1,reuseaddr
# 另一个终端；先停止已有 gpsd 实例／socket，避免占用默认端口。
sudo gpsd -N -n -b -S 2947 tcp://127.0.0.1:10110
# 普通桌面用户执行。12 行卫星列表适合本机屏幕；可按需加大。
xgps -s 12 -u m 127.0.0.1:2947
```

本机 KDE 为 Wayland，Debian xgps 3.25 仍提前检查 `DISPLAY`；无 XWayland 时可仅对
此命令设置 `DISPLAY=:0 GDK_BACKEND=wayland WAYLAND_DISPLAY=wayland-0`，GTK 实际使用
Wayland。不要将这些变量写入系统全局环境。停止会话后仍执行正常的 GNSS 清理。

## 协议依据

使用 REA／Ghidra 静态分析本机原厂 `libgnssmgt.so`，核对包校验、命令表和导航状态机；
另用本机 `libnavcore.so` 确认输出周期的单位，并以真机应答和 NMEA 校验交叉验证。
原厂库仅作私有参考，不随项目上传或在 Linux 下执行。

旧版公开 [GreenEye2 HAL](https://github.com/harryxiao/vendor/tree/f0e4a4c5025b8f7a13e69db3af9446717702f4f2/sprd/modules/gps/GreenEye2/ge2_libgps/gps_lib)
仅作协议历史参考，未复制其源码或将其不同版本的结构定义当成本机格式。
本目录为独立实现，许可证 `GPL-2.0-only`。
