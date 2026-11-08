# W202DS 内核配置模型

默认构建 `debug`。`desktop` 是屏幕、键盘及正常交互所需的功能基础；
`debug` 继承 desktop，再叠加当前 Server 配置里的调试和诊断设置。
它们描述内核能力，KDE/Weston、系统服务和软件包由发行版工程选择。

```text
common.config → platform/ums9620.config → boards/w202ds.config
                                             ↓
                                       desktop.config
                                             ↓
                                        debug.config
                                             ↓
                                  外部覆盖片段，按传入顺序
                                             ↓
                                Kconfig olddefconfig 与核验
```

`model.toml` 声明合并顺序和继承。common 维护通用设施、网络/存储及现有可选模块配置；
platform 维护 UMS9620/ARM64 选择；boards 维护板级驱动；desktop 维护显示、输入、蓝牙等
交互能力及普通启动默认值。选项按归属维护，不另设 module-support.config。
平台片段由本项目维护，不是 Linux 上游提供的 UMS9620 defconfig。

配置来自 `config/server.config` 经同一内核 `olddefconfig`、`savedefconfig` 得到的输入。
原 Server 文件继续作为历史 debug 对照，日常修改这里的片段。
更新内核版本时应重新核验默认值，不能假定精简片段在不同源码版本上结果相同。

## 生成与构建

从仓库根目录运行，需要 Python 3.11+。`plan` 只解析配置，不调用 Kbuild，不创建输出。

```sh
python3 -B scripts/kernel-build.py plan --out output/kernel-debug
python3 -B scripts/kernel-build.py configure --out output/kernel-debug
python3 -B scripts/kernel-build.py configure --profile desktop --out output/kernel-desktop
```

`configure` 使用临时的独立 Kbuild 目录生成配置，成功后发布到 `<out>/.config`。
同时输出 `configure.log` 和 `kernel-build.json`；报告记录每个选项的来源、行号、
覆盖过程、输入哈希、最终配置哈希、内核源码提交、工具链标识和实际 release。
未知选项、错误语法、依赖或 choice 导致的设置未生效会报错，不会发布无效配置。
Kconfig 自动选择、且没有显式请求的依赖允许按源码规则补全。

完整编译入口如下；构建前会重新生成并核验配置：

```sh
python3 -B scripts/kernel-build.py build --out output/kernel-debug --jobs 4
```

这一步执行 `Image modules`，生成树内模块与全局 `Module.symvers`；
`Devices/` 外部板级模块仍通过自己的构建入口使用同一个 KDIR。
修改配置后应选择新的输出目录，避免复用旧内核和模块产物。
该入口不安装模块，不刷写或重启设备。

```sh
export LOCALVERSION=
make -C Devices KDIR="$PWD/output/kernel-debug" OUT="$PWD/output/modules-debug" all
```

## 外部覆盖

REIMUKS 或其他构建工具可以重复传入 `--fragment`；外部覆盖始终在内部继承链之后执行，
同一选项以最后一份显式设置为准。片段内重复定义同一选项会报错。
模型里的片段路径相对 `model.toml`；命令行片段和输出路径相对调用者的工作目录。
需要更多继承层时，可用 `--model /path/to/model.toml` 选择另一份模型。

```sh
python3 -B scripts/kernel-build.py configure --profile debug \
  --fragment /external/distribution.config \
  --fragment /external/user.config \
  --out output/kernel-debug-custom
```

切换 Kconfig choice 时，应同时取消旧分支并选择新分支；生成器不猜测应放弃哪个请求。
改变最终 `.config` 就产生新的内核构建配置，需要重新生成匹配的内核和模块。
仅改变发行版安装哪些已有模块，可复用匹配的内核构建。

## 两种预设的边界

debug 保留当前 `CONFIG_LOCALVERSION="-w202ds"`、自动 Git 后缀、
强制诊断命令行 `rdinit=/w202diag`、初始 120 秒恢复策略，需要匹配的诊断用户态。
完整功能与内建清单见 [DEBUG.md](DEBUG.md)。

旧 `/dev/w202fbcon` 诊断实现及其 Kconfig、构建接入已删除；维护中的 initramfs
没有调用它，当前面板使用独立 DRM 路径。早期日志组件统一为 `pj4edk-boot-display`，
由 `CONFIG_PJ4EDK_BOOT_DISPLAY` 控制。与旧内核的历史 Server 对照时，应将删除旧接口
及其自动依赖变化单独核验。

desktop 保留正常显示/键盘所需的基础支持和现有驱动 y/m 分工，
关闭显式的 debugfs、SysRq、PM 调试、锁死检测、内嵌配置与早期启动日志。
`/dev/w202diagctl` 与恢复代码保留，初始计时设为 0；正常 DRM/面板驱动与键盘仍需配套模块。
启动参数优先使用引导器提供的内容；没有参数时使用 desktop 片段中的保底值。
引导器须提供实际 rootfs/initramfs 和板级所需参数。release 使用 `-w202ds-desktop` 区分模块目录。
这是配置模型，desktop 完整启动和屏幕/键盘联调仍需另行实机验收。

Kconfig 的正常默认值和子功能可能仍含诊断支持，例如 SLUB 调试能力默认存在但没有默认开启检查。
desktop 不通过打开 EXPERT、改动大量默认值来强行关闭所有名称含 DEBUG 的选项。
debug 也不是“所有调试开关全开”：ftrace、KGDB、DWARF 和 sanitizers 仍关闭。

## 验证

```sh
python3 -B -m unittest discover -s tests -p 'test_kernel_model.py'
```

测试覆盖继承顺序、多级外部覆盖、来源记录、循环继承、配置语法和环境隔离。
真实配置兼容性还需在具备交叉工具链的主机上运行 `configure`，
比较生成的 debug 配置与规范化后的 `config/server.config` 所有选项。
