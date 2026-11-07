# 电源键用户态

本目录管理 W202DS 电源键（设备树 `../devicetree/w202ds-keyboard.dtsi` 中的 `gpio-keys` 节点）在 GNOME 桌面上的用户空间处理：短按亮灭屏，按住 1 秒弹出电源菜单（关机 / 重启 / 挂起 / 取消）。电源键与键盘同属输入设备，因此放在 `keyboard/` 硬件项目下。

| 目录 | 内容 |
| --- | --- |
| `rootfs/` | 按键守护进程 `w202-power-key`、其 systemd 单元、logind 与 dconf 配置模板。 |
| `power-menu/` | GNOME Shell 扩展 `w202-power-menu@w202ds` 源码及其简体中文翻译。 |
| `tests/` | 通过 GNOME Shell 截图服务截屏的验收辅助脚本。 |

## 已实现

`w202-power-key` 只读（不独占）`/dev/input/by-path/platform-gpio-keys-event`，按下时从 DRM 连接器的 DPMS 状态判断屏幕亮灭，在当前活动图形会话中通过 GNOME Shell / Mutter 的 D-Bus 接口执行动作：

- 短按：屏幕亮时升起屏幕盾牌（按 GNOME 设置锁屏）并熄屏，屏幕灭时点亮。
- 按住 1 秒（按键尚未松开即触发）：若按下时屏幕是灭的，先以 `PowerSaveMode 0` 点亮，再调用会话总线 `org.gnome.Shell` 上的 `/io/github/w202/PowerMenu` `io.github.w202.PowerMenu.Open`；随后的松开不再当作短按。没有活动图形会话或扩展未运行（调用失败）时退回旧行为：记录日志后 `systemctl suspend`。
- 唤醒系统的那一次按键交给 GNOME 自己亮屏，不做处理。
- `w202-power-key --test-long-press` 按当前屏幕状态执行一次长按动作后退出，用于不按实体键的验收。

GNOME 自己的电源键动作被 dconf 设为 `nothing`，logind 忽略电源键，二者都不与本程序抢键。GNOME 48 的 `power-button-action` 只有 nothing / suspend / hibernate / interactive；interactive 打开的结束会话对话框每次只给一个确认动作，不能三选一，所以另做菜单。

扩展 `w202-power-menu@w202ds` 复用 GNOME Shell 自身组件，不另画界面：

- 对话框是 Shell 的 `ModalDialog` + `MessageDialogContent`，并沿用 Shell 关机 / 重启对话框的 `end-session-dialog` 样式（宽度一致），外观与系统对话框相同；标题及按钮文字直接取 gnome-shell 自带翻译（“关机菜单”“取消”“挂起”“重启”“关机”）。触摸点按、Tab/方向键切换、回车/空格激活、Esc 取消均由 Shell 对话框提供，初始焦点在“取消”。
- gnome-session 48 的 `Shutdown` / `Reboot` 一律弹出 Shell 的结束会话确认框（60 秒倒计时）；48 版已没有不弹框的 `RequestShutdown` / `RequestReboot`。为避免二次确认，扩展在发起请求前用 `IsInhibited(LOGOUT)` 查询抑制器：无抑制时，确认框出现后立即代为确认（发出 `ConfirmedShutdown` / `ConfirmedReboot`，与用户点确认相同）；有应用阻止注销、确认框类型不符（例如待安装离线更新的“重启并安装”）或框内列出应用时，保留 GNOME 原确认框交给用户处理。
- 锁屏时沿用 GNOME 规则：`SystemActions` 在锁屏状态下不提供关机和重启（gsd 的 interactive 电源键在锁屏上也不弹框），菜单中这两个按钮置灰并提示“解锁后才能关机或重启”，挂起和取消可用。扩展的 `session-modes` 含 `unlock-dialog`，锁屏后仍在运行。
- 打开时若屏幕盾牌处于激活状态（锁屏或熄屏），先按 GNOME 恢复唤醒时的同一路径唤醒锁屏（`ScreenShield._wakeUpScreen()`：撤掉黑色遮罩并发出 `WakeUpScreen`，由 gnome-settings-daemon 点亮屏幕），否则菜单会被锁屏的黑色遮罩盖住。
- 菜单打开时短按电源键熄屏（Mutter `PowerSaveMode` 变为非 0），或屏幕盾牌升起，菜单随之关闭。
- D-Bus 接口：`Open()`、`Close()`、只读属性 `IsOpen`。每次打开、关闭和选择都以 `w202-power-menu@w202ds:` 前缀写入 gnome-shell 日志。

## 构建输入输出

在本目录执行：

```sh
make OUT="$PWD/output" all
make OUT="$PWD/output" DESTDIR="$PWD/output/rootfs" install
```

输入：本目录源码与 `po/*.po`，需要 GNU gettext 的 `msgfmt`。输出：编译后的 `.mo` 及离线 rootfs 暂存树（扩展装到 `/usr/share/gnome-shell/extensions/w202-power-menu@w202ds/`）。`install` 只接受显式 `DESTDIR`，不启用服务。

目标系统依赖 GNOME Shell 48、gnome-session、Python 3、systemd、util-linux（`setpriv`）及 GLib 的 `gdbus`。集成时执行：

```sh
dconf update                                   # 60-w202-power-key：电源键动作 nothing，并默认启用扩展
systemctl daemon-reload
systemctl enable --now w202-power-key.service
```

新安装的扩展只在 GNOME Shell 启动时被发现，Wayland 下需要重新登录或重启。扩展通过系统 dconf 数据库的 `org.gnome.shell enabled-extensions` 默认值启用；用户若自己修改过扩展列表，以用户值为准，需保证其中仍含本扩展。

## 验收

两个辅助脚本用于不碰平板完成验收：`tests/shell-screenshot.py` 以会话用户身份运行，临时占用 `org.gnome.Screenshot` 名称（GNOME Shell 只为白名单名称截图），调用 `org.gnome.Shell.Screenshot` 保存整屏截图；`tests/uinput-key.py` 以 root 身份通过 uinput 敲一次指定键（1 = Esc，15 = Tab，28 = 回车）。

2026-11-08 在 W202DS（Debian 13，GNOME Shell 48.7，gnome-session 48.0，内核 `6.18.35-w202ds-m23-wifi`）上验证，结果均来自重启后的实机：

- 重启后 `gnome-extensions info w202-power-menu@w202ds` 为已启用 / `ACTIVE`，`gdbus introspect` 可见 `io.github.w202.PowerMenu`（`Open`、`Close`、`IsOpen`）；用户 dconf 中没有扩展列表，生效值来自系统默认。
- `w202-power-key --test-long-press`：未锁屏时打开菜单，截图为“关机菜单”及“取消 / 挂起 / 重启 / 关机”四个按钮，焦点在“取消”；`IsOpen` 为 true，Esc 后为 false，`Close()` 同样可关闭。
- 熄屏未锁时长按：先点亮（DPMS Off → On）再打开菜单。菜单打开时设 `PowerSaveMode 3`：菜单关闭（日志 `closed (screen off)`）。
- `org.gnome.ScreenSaver.Lock` 锁屏并熄屏后长按：屏幕点亮，锁屏上显示菜单，重启 / 关机置灰并提示“解锁后才能关机或重启”，挂起可用；日志 `opened on the lock screen`，Esc 取消。
- 键盘：两次 Tab 后焦点移到“重启”，回车激活。在 `gnome-session-inhibit --inhibit logout` 存在时选择重启，日志为 `logout is inhibited`，GNOME 原确认框列出阻止注销的程序并给出“仍然重启”，Esc 取消后会话回到 running 状态。去掉抑制后选择重启：日志 `end-session dialog confirmed: ConfirmedReboot`，1 秒内 logind `The system will reboot now!`，系统正常重启（无二次确认）。
- 回退：禁用扩展后执行 `--test-long-press`（`systemctl` 替换为记录参数的假程序），日志 `long press: power menu unavailable, suspend`，并调用了 `systemctl suspend`。

待用户实机确认：按住电源键 1 秒弹出菜单、松开后屏幕不熄灭；触摸点按挂起 / 重启 / 关机 / 取消；熄屏及锁屏状态下长按；菜单打开时短按电源键熄屏。关机与挂起两项按钮未在本轮自动验收中实际执行（挂起后需按电源键唤醒）。
