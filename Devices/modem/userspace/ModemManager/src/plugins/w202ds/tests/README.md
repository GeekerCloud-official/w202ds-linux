# W202DS SIM 热插拔测试

插件以原厂 RIL `onSimStatusChanged()` 使用的 `+ECIND: 3,...` 为事件提示，用父接口不缓存的 `+CPIN?` 确认在位状态。只在明确返回 SIM_NOT_INSERTED 时移除已有 SIM；无卡状态中收到有效 READY／PIN／PUK 才重新探测。掉网和查询失败不等于拔卡。显式 SIM 通知下仍为 READY 时，使用 MM 自带的 ICCID／IMSI 检查确认是否换卡。

事件经过空闲回调合并，查询期间的新通知会在本次完成后处理；初始化中的通知延后确认，清理时停用旧回调。无卡时持有 MM 的热插拔端口上下文，继续接收插卡通知。没有周期查询、私有守护进程或内核补丁。

## 输入与运行

输入为当前 MM 源码、GLib/GIO 开发环境、Meson、Ninja、Python 3、`dbus-daemon` 和 `gdbus`。在配置时启用 `-Dplugin_w202ds=enabled -Dtests=true`；构建目录须放在本轮允许的输出位置。

```sh
ninja -C "$OUT" src/ModemManager cli/mmcli \
  src/plugins/test-plugin-w202ds src/tests/test-at-serial-port
meson test -C "$OUT" --no-rebuild --print-errorlogs \
  test-plugin-w202ds test-at-serial-port
python3 src/plugins/w202ds/tests/test-sim-hot-swap-integration.py \
  --builddir "$OUT" --output "$OUT/w202ds-sim-test"
```

单元测试检查通知边界和状态决策，包括不吞掉正常的 CPIN 回复。集成测试启动独立会话 D-Bus、当前 daemon 和抽象 Unix socket 模拟基带；Test.SetProfile 的 `net:` 数据端口只建立内存对象，不访问真实网络接口。测试用的虚拟端口支持位于 `src/mm-plugin.c`、`src/mm-base-modem.c`，不改变物理 SIPC 探测路径。非系统位置的工具可用 `--dbus-daemon`、`--gdbus` 指定，运行库搜索路径由调用环境提供。

输出为退出码、AT 交互、daemon 日志和模拟 SIM 状态快照；测试退出时清理自建进程和 socket，不操作系统 ModemManager、真实 SIM、网络配置或原厂分区。

## 当前结果与 TODO

2026-11-08 本地 x86_64 daemon 和 CLI 构建通过，两项插件单元测试、三项 AT 串口测试通过。模拟流程八项检查通过：初始识卡、忽略无关通知、SIM 故障不误删、AT 超时后继续处理排队通知、掉网时确认在位、拔卡后移除旧 SIM 并显示 SIM_MISSING、重复拔卡保持稳定、插回新卡并识别新 IMSI。整个插拔流程不重启 daemon。

同日 ARM64 交叉编译及目标机立即绑定符号的加载检查通过，`1.24.0-w202ds-sim-hotplug-20261108` 已部署。无卡启动实测为 failed／sim-missing，SIM、注册、附着字段为空，AT 通知通道保持打开。用户实际插卡后收到第 3 组 ECIND；首次 SIM busy 未误判，后续 CPIN READY 触发重探测，自动恢复 5G／NM 连接，蜂窝 DNS 及绑定接口、校验证书的 HTTPS 200 通过。MM 进程未重启，热点与基带运行服务正常。

待真机验收：联网时拔卡的 bearer 停用、NM/KDE 无卡显示、PIN 锁卡、冷启动及长期稳定性。用户暂缓热拔卡，已停止临时日志跟随并恢复 INFO 日志；不重启当前已联网进程。本轮没有替换内核或原厂基带固件。
