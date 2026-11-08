# 传感器用户态

| 路径 | 作用 |
| --- | --- |
| `rootfs/usr/lib/systemd/system/w202-sensorhub.service` | 在 `w202-modem-runtime.service` 启动 CH 之后加载 `w202_hub_iio`，随其停止/重启。 |
| `rootfs/usr/libexec/w202-sensorhub-attach` | `modprobe w202_hub_iio` 后每秒重试绑定 `sprd-sensorhub`，最多 30 秒：CH 的 spipe_ch 环就绪前探测会延迟，开机后期没有别的事件会重新触发它。 |
| `rootfs/usr/lib/udev/rules.d/81-w202-sensorhub.rules` | 给 iio-sensor-proxy 的加速度计安装方向矩阵。磁力计没有 `in_rot_*` 罗盘通道，iio-sensor-proxy 不使用；光线传感器按 `in_illuminance_input` 自动识别。 |

## 安装

```sh
make install DESTDIR=/path/to/rootfs
```

只做暂存。`w202_hub_iio.ko` 由内核模块构建产出，放入 `/lib/modules/$(uname -r)/updates/w202ds/` 后执行 `depmod -a`；启用 `w202-sensorhub.service` 并安装 Debian 的 `iio-sensor-proxy` 属于集成步骤。GNOME 通过 iio-sensor-proxy 获得方向，但 Mutter 只在平板模式下接管面板方向：键盘驱动 `w202_zsk` 在键盘拆下时上报 `SW_TABLET_MODE=1`，此时快速设置出现“自动旋转”开关（关闭即 `org.gnome.settings-daemon.peripherals.touchscreen orientation-lock`）；装上键盘回到笔记本模式，屏幕保持横屏。

## 方向矩阵

面板原生为竖屏 1200×1920。放在键盘支架上横屏使用（GNOME 旋转“right”）时，LSM6DSOQ 的重力分量在 +X、Z 指向观看者，因此矩阵为 `diag(-1, -1, 1)`（平面内转 180°）；装上后 iio-sensor-proxy 在该姿态报告 `right-up`。
