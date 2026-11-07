# 基带用户态

本目录管理 W202DS 基带对应的用户空间源码和服务模板，与内核 `../source/`、设备树 `../devicetree/`、固件 `../firmware/` 同属一个硬件项目。初始导入逐文件来源见 `SOURCES.json`，后续修改以 Git 记录为准。

| 目录 | 内容 |
| --- | --- |
| `at/` | SIPC AT 查询、数据连接控制、绑定蜂窝源地址的 DNS 检查。 |
| `runtime/` | 原厂 Bionic 程序的日志/属性服务桥接、私有 NV loop 存储、属性模板及私有输入契约。 |
| `rootfs/` | 本基带自己的运行服务、网络服务、账户和默认配置模板。 |
| `tests/` | AT 回包与并发锁、NV 完整性和 loop 映射检查、loader 进程锁验收源码。 |
| `ModemManager/` | 上游 1.24.0 工作树上的 SIPC 端口、W202DS 插件与 M-ETHER bearer 适配。 |
| `integration/` | 可选的 KDE / NetworkManager 接入配置；不随基础 AT 安装自动启用。 |

上述源码、配置、测试和构建文件随本仓库发布。`ModemManager/` 发布完整的当前源码快照，基于上游 1.24.0 的 `dfa41adf391b090720fb1ea56d884f61ea7fba29`，包含本项目的 SIPC/W202DS 适配；本地上游 Git 工作树保留，`.git` 不上传。它以普通源码文件纳入本仓库，克隆后即可取得。

`releases/` 是本地已测试的安装包、源码快照和验证记录；`server-runtime-20261103-v5/` 是旧 Server 运行参考包。两者均仅在本地保留，不上传 GitHub，也不参与下面的 `make` 构建。编译临时目录 `output/` 同样不上传；设备私有 NV、IMEI 和密钥不属于发布源码。

## 已实现

`w202-modem-at` 通过 `/dev/stty_nr0` 原始字符设备访问 SIPC，不依赖 termios。查询有白名单，同一个设备通过跨进程锁避免串包。`w202-cellular` 建立 CID1 IPv4 数据连接，配置 `sipa_eth0`、动态地址/DNS、源策略路由和断线重连。这里的“拨号”指建立数据连接，APN 是接入点，CID 是会话编号，不拨电话号码。

`w202-modem-runtime` 配合本项目内核模块准备 AVB/CP、启动本机原厂 loader 和 NV 服务。当前 Server 已验证 SIM 注册、数据连接和绑定蜂窝源地址的 HTTPS。历史 CAICT 下载 314.96 Mbps / 上传 21.51 Mbps 属于历史测量，不是每个内核的测速承诺。

2026-11-07 用户确认 KDE 上的手机 → Wi-Fi 5／5 GHz／80 MHz 热点 → 蜂窝出口链路验收通过，端到端测速为 663 Mbps、103 Mbps，未标注上下行方向。这是蜂窝联网与热点共享结果，不作为独立的 Wi-Fi 局域网吞吐。

## 构建输入输出

在本地交叉工具链环境、本目录执行：

```sh
make OUT="$PWD/output" all
make OUT="$PWD/output" DESTDIR="$PWD/output/rootfs" install
```

输入：本目录 C/Python 源码、属性 JSON、AArch64 GCC 和 Python 3。输出：三个静态 ARM64 工具、属性文件和离线 rootfs 暂存树。构建不需要重编内核；`install` 只接受显式 `DESTDIR`，不启用服务。安装器显式将运行脚本和命令包装器设为 0755，不依赖模板源文件的权限。

目标系统依赖 Python 3、systemd/sysusers/resolved、iproute2、kmod、util-linux（含 losetup）、Linux loop 块设备支持、核心 shell 工具，以及匹配的内核模块和本机私有运行根。默认 APN 为空，独立启动准备默认关闭；发行版集成阶段负责配置、账户创建、安装依赖和启用服务。

## TODO / 实验中

- 未适配的发行版 ModemManager 不能识别 SIPC。2026-11-07 KDE 已复用 11 月 5 日通过联网测试的 W202DS 适配二进制，接入发行版自己的 ModemManager systemd 单元及 NetworkManager；旧 `w202-cellular` 停用。详见 [桌面接入](integration/README.md)。当前源码较该固定包另有并发保护、5GS 探测和排版差异，未在本轮重新构建 MM，不把该包称作当前源码的新构建。
- 现有 runtime 显式校验 `6.18.35-w202ds-m23-wifi`。后续缩短内核名称时必须同时更新运行约定，并重新验证。
- IPv6、双卡、SIM/无线复位、长期稳定性仍待实现或验收。
- `runtime/private-inputs.json` 仅描述已有准备流程所需的输入。旧 BSP 的 `server-runtime-20261103-v5/` 包作为既有运行参考保留，其中厂商依赖不是本项目的开源实现；设备 NV、IMEI 和密钥仍由本机单独提供。

顶层旧文件名兼容链接已移除；源码直接使用 `at/`、`runtime/`。`ModemManager/` 自带的枚举模板链接属于上游构建结构，保留在其工程内部。目录整理后的三个 ARM64 工具、离线安装暂存，以及8项 AT 回包/锁测试均通过。

2026-11-07 本轮重新在本地构建三个静态工具及属性区，修复 KDE 账号与私有目录权限、补上独立 Linux 启动输入；SIM READY、5G 注册、原生 NM 连接、断开重连和运行服务重启后自动连接均通过。两次恢复后绑定 `sipa_eth0` 的 HTTPS 200 及蜂窝 DNS 通过。Wi-Fi 保持连接；冷启动与长期运行待验收，当前不宣称完成全部基带功能。

`tests/test_modem_at.py` 配合本机编译的 `modem-at-fixture.c` 检查回包和锁；测试不启用无线电。loader 锁测试仅适用于匹配的进程所有权实现，不作为自动启动程序。

2026-11-07 NV 修复已部署：`runtime/w202-modem-storage.py` 安装为 `/usr/libexec/w202-modem-storage`，提供 `prepare`、`verify`、`release`，校验镜像长度、NV 头和校验和及 loop 映射；损坏的旧文件直接报错，不补零伪造 NV。原厂 diskserver 的截断打开和脏扇区写入通过 loop 保持块设备语义。六项本地测试及真机截断／局部写对照通过；普通重启后自动联网和原厂服务实际保存 NV 后，镜像容量、校验及原厂分区哈希均通过。运行说明见 [集成说明](integration/README.md)，本地交付保留于 `releases/kde-nv-loop-20261107-v1/`。
