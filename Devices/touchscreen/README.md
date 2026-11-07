# w202ds-touchscreen-driver（触摸屏）

```text
touchscreen/
├── build.mk
├── devicetree/
├── firmware/
├── README.md
└── source/
```

| 功能 | 可用性 | 问题描述 |
| --- | --- | --- |
| 触摸输入 | KDE 首次输入已验证 | 以启用的 `himax,hxcommon` DT 节点探测，不再依赖原厂 `lcd_name` bootargs；桌面自动加载，用户确认首次正常。 |
| 触摸固件加载与重载 | 初次加载已验证，恢复待修复 | HX83102J / 固件 2c840357 加载成功；开合后曾收到全零包并触发复位，不能标为恢复成功。 |
| 强制重启后恢复 | 待验证 | 强制重启后的固件与触摸恢复待测。 |
| 显示与触摸同步 | 有缺陷 | KDE 显示开关回调已执行，但固件恢复不稳定；与面板共用复位，不能把异常复位当作正常 DPMS 恢复。 |

输入继续走 Linux input / libinput / KWin，优先修复芯片的休眠恢复，不增加桌面触控代理。

更新日期：2026-11-06
