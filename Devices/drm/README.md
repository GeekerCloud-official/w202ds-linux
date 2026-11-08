# w202ds-drm-driver（屏幕）

```text
drm/
├── build.mk
├── devicetree/
├── README.md
└── source/
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 内屏显示与登录 | Server 已验证，KDE 已启动 | KDE 标准版已进入 SDDM／桌面；开合后的显示恢复仍待修复。 |
| 启动日志与显示接管 | 已有实现，待完整验收 | 黑框内显示启动日志；`preserve` 可停止日志并保留画面，等待桌面提交。完整视觉交接仍待验收。 |
| 亮度调节 | 待验收 | 已有背光控制实现，完整调节范围及恢复行为待测。 |
| 长时间显示 | 待验证 | 尚无长时间稳定性结论。 |
| 休眠与唤醒显示恢复 | 有缺陷 | KDE 开合后出现过 vblank 等待超时和 framebuffer 错误，尚未通过恢复稳定性验收。 |

早期日志组件为 [pj4edk-boot-display](source/pj4edk-boot-display/README.md)，
由 `CONFIG_PJ4EDK_BOOT_DISPLAY` 内建；可选 `pj4edk2-handoff` 模块用于手动停止其日志。
正常显示由 DRM 通过通用 aperture 接管。旧 LK framebuffer 诊断实现和
`/dev/w202fbcon` 接口已删除，普通 DRM/fbdev 支持保留。

构建、接口与验证范围见 [显示驱动说明](source/baseline/README.md)。

更新日期：2026-11-07
