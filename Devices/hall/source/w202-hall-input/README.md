# W202DS Hall input module

Input module for the matching M23 Linux Server kernel. Binds the stock `ah,hall_ic`
node and its `ah,gpio_irq` property. It validates controller address `0x64170000`,
offset 148 and flags 0 before acquiring the input. The legacy GPIO resolver is
used only because the original firmware property has a nonstandard name.

Polarity was physically checked on the original Android kernel on 2026-11-03:
closing the magnetic cover changed the level 1 → 0 and increased the GPIO148
interrupt count; opening changed it 0 → 1 with another interrupt. The module
reports `EV_SW/SW_LID`: 1 means closed, 0 means open. It samples the initial
state and subsequently reads in the rising/falling threaded IRQ; no timer or
periodic GPIO read is used. Read-only platform attributes are `raw_level`,
`closed` and `irq_count`. A user reading an attribute performs one GPIO read.

Input/GPIO/IRQ resources use managed lifetimes. It leaves pin bias and voltage
unchanged. An explicit `vddio-supply`, if supplied by future board DT, is enabled
with a balanced managed disable; the original DT has no such property. The
driver does not enable suspend wakeup or directly control screen/system power.
The candidate includes `server-policy/60-w202-server-lid.conf`, which sets all
three logind lid actions to `ignore`: the user normally keeps the magnetic cover
closed, while Server/SSH should continue running. Install it only in the Server
root at `etc/systemd/logind.conf.d/` before the first module test and apply the
policy before loading. The policy and the guarded `w202-hall.service` are now
installed on the actual p78 Server root, with normal service restart verified.
They are not yet integrated into a frozen rootfs bundle. Existing desktop
profiles inherit Server overlays, so the policy remains with this hardware
source instead of entering a shared profile overlay.

Build only on CM2 against the unchanged matching M23 headers, with a separate
`MO` output directory. On the actual M23 Server, 11 physical lid transitions
produced 11 EV_SW/SW_LID events and 11 interrupts; the initial closed state was
also verified with EVIOCGSW. See the hardware README for evidence. On original
Android, `ah1898` already owns the line. Do not load this M23 module into that kernel,
unbind the original driver, or run the raw GPIO helper against an owned line.
