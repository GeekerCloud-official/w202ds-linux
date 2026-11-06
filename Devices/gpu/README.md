# w202ds-gpu-driver（图形处理器）

```text
gpu/
├── build.mk
├── devicetree/
├── README.md
└── source/
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| GLES 3D 绘制 | 已验证 | Panfrost / GLES 3.1：100 帧像素校验通过（2026-11-05 验证）。 |
| 3D 负载运行 | 短测通过 | glmark2 离屏短测完成；尚未验证长时间稳定性，分数不代表屏幕帧率。 |
| KDE 桌面加速 | 待验证 | 尚未启动 KDE 验收。 |
| 浏览器图形加速 | 待验证 | 尚无浏览器场景验收结果。 |
| Vulkan | 待完成 | 尚无可用性验收结果。 |
| 动态调频与功耗管理 | 待完善 | 已有可选频率控制实现，完整调频策略及功耗待验。 |

更新日期：2026-11-05

## TODO

- 移植 Vulkan（PanVK），完成 W202DS 真机验证。参考 [DroidDeck 非 Adreno 驱动](https://github.com/The412Banner/droiddeck-nonadreno-drivers) 和 [FristOneRR PanVK](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source)。
- 实现并验证 GPU 动态调频。
