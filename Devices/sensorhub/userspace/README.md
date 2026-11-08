# 传感器桌面集成

使用 Debian `iio-sensor-proxy`，通过标准 SensorProxy D-Bus 接口向桌面提供加速度方向和环境光；不另写轮询守护程序。陀螺仪、三轴磁场由 IIO 提供，不伪装成已校准罗盘。

| 文件 | 作用 |
| --- | --- |
| `rootfs/usr/lib/systemd/system/w202-sensorhub.service` | 基带运行时启动 CH 后加载模块，随基带运行时停止。 |
| `rootfs/usr/libexec/w202-sensorhub-attach` | 加载模块并等待 CH 收发环就绪，最多重试绑定 30 秒。 |
| `rootfs/usr/lib/udev/rules.d/81-w202-sensorhub.rules` | 配置 LSM6DSOQ 安装方向矩阵。 |

## 安装

```sh
make install DESTDIR=/path/to/rootfs
```

此命令仅暂存文件。将匹配的 `w202_hub_iio.ko` 安装到 `/lib/modules/$(uname -r)/updates/w202ds/`，运行 `depmod -a`，安装发行版 `iio-sensor-proxy`，启用 `w202-sensorhub.service`。CH 由现有基带运行时启动；此处不下载固件、不写 NV、不复位处理器。

2026-11-08 本机 KDE 上，SensorProxy 已报告 `HasAccelerometer=true`、`HasAmbientLight=true`，可输出 right-up 方向和约 160 lux。SSH 会话不是本地活动桌面会话，命令行验收可用 `sudo monitor-sensor --accel --light`；不为远程测试放宽传感器访问策略。

## 方向与验证边界

原生面板为竖屏 1200×1920，键盘支架横屏时原始重力主分量在 +X；当前矩阵 `diag(-1, -1, 1)` 在此姿态报告 right-up。转动记录已捕获 normal、left-up、right-up、bottom-up 四种方向和倾斜变化；每个状态与实物方向的对应仍待标记核对。自动旋转及亮度策略由桌面管理，其界面和联动效果未验收。

本轮传感器数据读取与动态响应已获用户验收。

TODO：核对带方向标记的安装矩阵、桌面联动和挂起恢复；光强标定另验。
