# W202DS 固定内屏 DRM/KMS

本项目负责 DPU/DSI/DSC、内屏、DRM fbdev 与显示状态接口。显示接管通过 Linux 通用 aperture API 完成。

`defer_modeset=1` 为默认值。probe 只读取设备树/硬件身份、获取资源并注册 KMS；不启用显示时钟、不接管 DPU IRQ、不重置 DPU/DSI/面板、不改背光、不创建 fbdev，也不修改扫描输出。所有 atomic 提交及手动 DSI 启动请求在显示就绪前返回错误。已有启动画面继续由原持有者维护。

外层用户态在适当阶段向 `/sys/devices/platform/w202-r6p0-lk-dsi/display_enable` 写1，才启用时钟和中断并执行显示初始化。若 DT 提供有效的 `pocketjs,boot-display` 区域合同，创建原生 DRM 黑框 console；普通启动则创建全屏 fbdev。写入同步返回初始化结果，请求不能写0回退。`defer_modeset=0` 仅供明确选择立即启用行为。

启用流程先分配 RGB565 framebuffer、持有 DPU/DSI 时钟，再通过 Linux 通用 aperture API 移除继承的 framebuffer 所有者，复制旧画面并初始化显示。`pj4edk-boot-display` 的 aperture remove 回调同步停止写屏后释放其时钟引用。首次 KMS 提交锁存成功后开启背光，异步 console 只改黑框内变化的字形，框外保留原图。DPU 连续扫描，不逐行 atomic commit。

`boot_console_status` 提供接管、帧数、字形数及 CPU 绘制计时；向 `boot_console` 写0停止黑框 writer 并启用全屏 fbdev。写入 `preserve` 则只停 writer，保留画面且不创建全屏 fbdev，供桌面 compositor 随后提交。引导 framebuffer 保留至 DRM 注销或重启，避免释放仍可能扫描的内存。登录阶段由外层集成判定，驱动提供显示状态与控制接口。

2026-11-05 首次真机短测确认：约4.58秒原生黑框 active，旧 writer 停止更新；DSI 60 Hz、持续 vsync、无 DPU 错误或锁存超时，首次锁存约16.56 ms。首帧 CPU 绘制约1.47 ms。随后 17:00 记录确认该模块随完整 Server 常驻运行，登录 TTY 和霍尔息屏策略已接通。长期运行与 KDE 仍待验收，短测统计不等于全部面板观感验收。

2026-11-06 KDE 标准版已使用该接口启动：内核捕获有效黑框描述并完成 DRM 提交，SDDM 查询为 1920×1200 / Panfrost。KDE 开合屏幕恢复出现 vblank 等待超时，且触控异常复位后有 framebuffer 错误，尚未通过稳定性验收。

在 Linux 仓库根目录，本地独立构建入口是 `make -C Devices KDIR=... OUT=... w202ds-drm-driver`；研究项目统一入口为 `tools/bsp-build modules`，参数相同。`w202ds-drm-snapshot` 是单独的只读诊断目标。驱动保留原有不提供 module_exit 的行为，运行中的显示驱动不能热替换；候选版本应在下一次匹配的引导中测试。

## 可选启动显示描述

仅当 `/chosen/pocketjs,boot-display` 完整且布局有效时，使用继承画面和框内日志。
缺失、长度错误、未知版本、地址/格式/旋转不符、矩形越界均回到普通 fbdev；不因可选描述损坏阻止 DRM 激活。
资源分配或真正的硬件初始化失败仍返回错误。协议内容见 `../pj4edk-boot-display/README.md`。

CM2 主机测试（无硬件访问）：

```sh
mkdir -p output/tests
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  6.18.35/Devices/drm/source/baseline/tests/boot-display-contract.c \
  -o output/tests/boot-display-contract
output/tests/boot-display-contract
```

该测试覆盖缺失、长度、20 项非法字段、越界和 EFI/旧 LK 两种有效布局；真机交接仍需另验。
