# pj4edk2-handoff

独立 Linux 控制模块，输出 `pj4edk2-handoff.ko`。从原项目
`bsp/integration/boot-handoff/pj4edk2-handoff/` 迁入源码；不含 EFI/LK 工程。

加载默认 `release=0`，通过 `symbol_get` 查询可选早期 console。
没有 provider 也能加载；`/sys/module/pj4edk2_handoff/parameters/` 提供：

| 属性 | 用途 |
| --- | --- |
| `console_available` | 是否找到早期 console |
| `console_active` | 早期 console 是否仍在写屏 |
| `release` | 写 1 停止并等待早期绘制结束；单向操作 |
| `releases` | 实际停止次数 |

本模块不操作显示寄存器、不释放显示时钟，也不依赖 DRM 头文件或符号。
正常 DRM 接管通过通用 aperture API 完成，不需要先调用此模块。
旧 multi-user 自动释放脚本没有迁入，避免改变当前尽早接管策略。

在 CM2 使用外层 `Devices/Makefile` 的 `w202ds-drm-handoff` 目标，独立输出和收据。
它是可选目标，不强制加入默认 Server 模块组；新的内建 provider 和完整交接仍需真机验收。
