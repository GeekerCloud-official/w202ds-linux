#!/bin/sh
set -eu
R=/run/w202-wayland-m14
L=/run/m22-keyboard/desktop-r4
test -x "$R/usr/bin/weston"
test ! -S /run/w202-wayland-runtime/wayland-w202
mkdir -p "$L" /run/w202-wayland-runtime
chmod 700 /run/w202-wayland-runtime
cat /sys/bus/platform/devices/zsk_keyboard/state >"$L/before-input-state.txt"
dmesg >"$L/before.dmesg"
timeout -s KILL 5 chvt 2
setsid sh -c 'exec chroot /run/w202-wayland-m14 /usr/bin/env PATH=/usr/bin:/usr/sbin SHELL=/bin/sh LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu/weston:/usr/lib/aarch64-linux-gnu/systemd LIBSEAT_BACKEND=builtin XDG_RUNTIME_DIR=/run/w202-wayland-runtime XDG_SEAT=seat0 /usr/bin/weston --backend=drm --drm-device=card0 --renderer=gl --socket=wayland-w202 --config=/etc/weston.ini --log=/run/m22-keyboard/desktop-r4/weston.log </dev/tty2 >/run/m22-keyboard/desktop-r4/weston-stdio.log 2>&1' &
weston_pid=$!
echo "$weston_pid" >"$L/weston.pid"
n=0
while [ ! -S /run/w202-wayland-runtime/wayland-w202 ] && [ "$n" -lt 100 ]; do
 kill -0 "$weston_pid"
 sleep 0.1
 n=$((n+1))
done
test -S /run/w202-wayland-runtime/wayland-w202
chroot "$R" /usr/bin/env PATH=/usr/bin:/usr/sbin SHELL=/bin/sh LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu XDG_RUNTIME_DIR=/run/w202-wayland-runtime WAYLAND_DISPLAY=wayland-w202 /usr/bin/weston-terminal >"$L/terminal.log" 2>&1 &
echo $! >"$L/terminal.pid"
echo M22_VERIFICATION_DESKTOP_READY
