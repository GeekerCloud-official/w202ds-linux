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
| 动态调频 | 已验证 | 已集成到板级 Panfrost：T760 四档 153.6/384/512/655.2 MHz，simple_ondemand 调速；真机 trans_stat 四档均有驻留和切换（2026-11-08 验证）。 |
| 频档准入 | 已验证 | 按 efuse `gpu_bin` 和继承的固定 GPU 电压选择可用频档，不调节稳压器电压；512/655.2 MHz 仅在 T760 且电压满足时开放。 |
| 温控限频 | 部分验证 | 读取 `gpu-thmzone`，达到 75°C 时通过 PM QoS 限制到 384 MHz，降到 65°C 解除；限频/解除日志已在调低阈值时观察到，默认阈值下长期热验证待完成。 |

更新日期：2026-11-08

## TODO

- 移植 Vulkan（PanVK），完成 W202DS 真机验证。参考 [DroidDeck 非 Adreno 驱动](https://github.com/The412Banner/droiddeck-nonadreno-drivers) 和 [FristOneRR PanVK](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source)。
- 在默认温控阈值下完成长时间高负载热验证。
