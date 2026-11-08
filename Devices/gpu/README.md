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
| GLES 3D 绘制 | 已验证 | Panfrost / GLES 3.1：四个频档各 100 帧像素校验通过（2026-11-08，6.18.35-w202ds-m23-wifi）。 |
| 3D 负载运行 | 待补测 | PR 原报告的 glmark2 离屏短测已完成；温控修复后四档像素校验通过，Wayland 负载测试因 KWin 阻塞未完成。 |
| KDE 桌面加速 | 待验证 | 温控修复后的首次 KDE 启动阻塞于显示缓冲区 CMA 分配路径，待重启后复测；尚不能确定阻塞根因。 |
| 浏览器图形加速 | 待验证 | 尚无浏览器场景验收结果。 |
| Vulkan | 待完成 | 尚无可用性验收结果。 |
| 动态调频 | 已验证 | 已集成到板级 Panfrost：T760 四档 153.6/384/512/655.2 MHz，simple_ondemand 调速；真机 trans_stat 四档均有驻留和切换（2026-11-08 验证）。 |
| 频档准入 | 已验证 | 按 efuse `gpu_bin` 和继承的固定 GPU 电压选择可用频档，不调节稳压器电压；512/655.2 MHz 仅在 T760 且电压满足时开放。 |
| 温控限频 | 部分验证 | 读取 `gpu-thmzone`，达到 75°C 时通过 PM QoS 限制到 384 MHz，降到 65°C 解除；限频/解除日志已在调低阈值时观察到，默认阈值下长期热验证待完成。 |
| 温控提供者重绑 | 短测通过 | managed device link 保证传感器移除前先停止 GPU；真机完成传感器解绑、重绑和 GPU 重新绑定。高频配置下传感器未就绪会延迟 GPU probe。 |
| 温度读取失败处理 | 本地用例通过 | 首次有效冷读数前和读取失败时限制到 384 MHz；恢复后沿用 65°C 解除阈值。错误与恢复路径覆盖在 12 个状态转换用例中，未在真机注入读故障。 |
| GPU 运行时省电恢复 | 待验证 | 本次测试未观察到 runtime suspend/resume，不能记为通过；整机休眠另待验证。 |

更新日期：2026-11-08

## 构建和测试

输入为本目录 `source/panfrost/` 与匹配目标内核的构建目录（含生成头文件和 `Module.symvers`）。不需要修改内核源码；输出为 `panfrost.ko`、`w202_gpu_support.ko` 和符号表。

```sh
make -C "$KDIR" M="$PWD/Devices/gpu/source/panfrost" \
  MO="$PWD/output/gpu" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CONFIG_DRM_PANFROST=m W=1 modules
cc -Wall -Wextra -Werror -O2 Devices/gpu/source/panfrost/tests/thermal-policy.c \
  -o output/thermal-policy
./output/thermal-policy
```

运行时使用标准 devfreq 的 `governor`、`available_frequencies` 和 `trans_stat`。桌面启动策略也须选择 `simple_ondemand`；系统若另行强制 `performance`，将不会按负载自动降档，但 PM QoS 温控上限仍有效。

## TODO

- 移植 Vulkan（PanVK），完成 W202DS 真机验证。参考 [DroidDeck 非 Adreno 驱动](https://github.com/The412Banner/droiddeck-nonadreno-drivers) 和 [FristOneRR PanVK](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source)。
- 在默认温控阈值下完成长时间高负载热验证。
