# W202DS Plasma 虚拟键盘联动

Plasma 6.3 默认依据最后的触屏操作弹出虚拟键盘，实体键盘接着时也会弹出。
本 KWin 声明式脚本用 Kirigami 的标准 `Settings.tabletModeChanged` 事件，将
KWin 的虚拟键盘 `enabled` 跟随键盘套拆装状态。没有定时器、轮询或独立守护程序。
实际布局、候选和输入由 Debian 的 Maliit 提供，不修改 KWin、Maliit 或内核。

前提：键盘驱动已经上报标准 `SW_TABLET_MODE`，接上为 0、拆下为 1；
Plasma Wayland、Kirigami 和 `org.kde.plasma.workspace.dbus` QML 模块可用。

## 安装

以正在运行 Plasma 的用户，在本包目录执行；先安装发行版 `maliit-keyboard`。

```sh
kpackagetool6 --type KWin/Script --install "$PWD"
kwriteconfig6 --file kwinrc --group Wayland --key InputMethod \
  /usr/share/applications/com.github.maliit.keyboard.desktop --notify
kwriteconfig6 --file kwinrc --group Plugins \
  --key w202ds-tablet-keyboardEnabled --type bool true
gsettings set org.maliit.keyboard.maliit enabled-languages "['en', 'zh-hans']"
qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure
```

开机／脚本加载时同步当前平板模式，随后仅响应状态事件。拆下后触屏点击支持
Wayland 文本输入协议的输入框弹出；接回立即禁用并收起。不强制在拆下时弹出。
XWayland 程序的文本输入支持取决于应用，未宣称全部应用兼容。

## 移除

在系统设置的 KWin 脚本页禁用“W202DS detachable keyboard”，或执行：

```sh
kwriteconfig6 --file kwinrc --group Plugins \
  --key w202ds-tablet-keyboardEnabled --type bool false
qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure
kpackagetool6 --type KWin/Script --remove w202ds-tablet-keyboard
```

如需恢复 Maliit 的默认触屏弹出行为，再在 KDE 的虚拟键盘设置中启用它。

## 实机验证

2026-11-08，Plasma 6.3.6，KWin 脚本正常加载；键盘套接上时，
`tabletMode=false`、虚拟键盘 `enabled=false`、`visible=false`，KWin 日志确认同步成功。
用户已确认调整后的联动可用。尚未单独验收全部应用、中文候选或重启后的行为。
