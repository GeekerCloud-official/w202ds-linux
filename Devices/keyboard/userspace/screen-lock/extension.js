// SPDX-License-Identifier: GPL-2.0-or-later
//
// W202DS screen lock: phone-style screen off / on on top of GNOME Shell's own screen shield.
//
//  - Screen off (power key, through ScreenOff() on D-Bus) raises the shield without its slide-down animation, so
//    the lock screen is never seen while the screen goes dark.
//  - Waking shows the lock screen clock. While the shield is not locked, a swipe up (or a key, a scroll down)
//    lifts it and returns to the session without a password; a tap does nothing, as a stray touch must not
//    unlock (GNOME's tap action also fires on the release of a failed drag). The lift is this extension's own
//    slide followed by GNOME's non-animated deactivate(false): GNOME's animated deactivate(true), which no GNOME
//    code path uses, cancels its own slide-out and leaves the shield unable to show again (the next screen off
//    then keeps an invisible, input-grabbing lock screen over a frozen-looking session).
//  - The shield locks once the screen has been off for org.gnome.desktop.screensaver lock-delay seconds (0: at
//    once; lock-enabled false: never). Wall-clock time is compared on wake as well, so time spent suspended
//    counts. With a non-zero delay a suspend no longer locks at once.
//  - Whatever lights the outputs while the shield is up (gnome-settings-daemon on a charger plug, a notification,
//    a key) also drops the shield's black curtain, so a lit screen is never left black.
//
// Everything else (authentication, notifications, the idle fade) is GNOME's.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {ScreenShield} from 'resource:///org/gnome/shell/ui/screenShield.js';
import {UnlockDialog} from 'resource:///org/gnome/shell/ui/unlockDialog.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

const OBJECT_PATH = '/io/github/w202/ScreenLock';
const IFACE = `<node>
  <interface name="io.github.w202.ScreenLock">
    <method name="ScreenOff"/>
    <method name="ScreenOn"/>
  </interface>
</node>`;
const MUTTER = ['org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig', 'org.gnome.Mutter.DisplayConfig'];
const MODIFIERS = [Clutter.KEY_Shift_L, Clutter.KEY_Shift_R, Clutter.KEY_Shift_Lock, Clutter.KEY_Caps_Lock,
    Clutter.KEY_Control_L, Clutter.KEY_Control_R, Clutter.KEY_Alt_L, Clutter.KEY_Alt_R,
    Clutter.KEY_Super_L, Clutter.KEY_Super_R];
const LIFT_TIME = 300;    // ms, GNOME's CURTAIN_SLIDE_TIME

export default class W202ScreenLock extends Extension {
    enable() {
        const ext = this;
        this._shield = Main.screenShield;
        this._settings = new Gio.Settings({schema_id: 'org.gnome.desktop.screensaver'});
        this._offAt = 0;        // wall-clock µs when the shield came up, 0 while it is down
        this._lockTimeoutId = 0;
        this._lifting = false;  // our slide-up of the lock screen is running
        this._raiseWhenDownId = 0;
        this._injections = new InjectionManager();
        const unlocked = () => ext._shield?.active && !ext._shield.locked;

        // Waking: keep the lock screen up (the clock) instead of returning straight to the session, after
        // locking first if the screen was off for longer than the lock delay.
        this._injections.overrideMethod(ScreenShield.prototype, '_onUserBecameActive', orig => function () {
            if (!ext._shield || !this._isActive)
                return orig.call(this);    // idle fade not finished yet: GNOME cancels it
            ext._lockIfDue();
            if (this._becameActiveId) {
                this.idleMonitor.remove_watch(this._becameActiveId);
                this._becameActiveId = 0;
            }
            this._longLightbox.lightOff();
            this._shortLightbox.lightOff();
            ext._setPowerSave(0);
            return undefined;
        });

        // Suspend: with a lock delay, keep the shield up without locking; the wake check above locks if due.
        this._injections.overrideMethod(ScreenShield.prototype, '_prepareForSleep', orig => function (lm, about) {
            if (!ext._shield || !about || ext._lockDelay() <= 0)
                return orig.call(this, lm, about);
            if (!this.active)
                ext._raiseShield();
            return undefined;
        });

        // The lock screen while not locked: a key or a scroll down, which would show the password prompt, lifts
        // the shield. The tap action calls this too, on any touch or click release; only a swipe lifts for those.
        this._injections.overrideMethod(UnlockDialog.prototype, '_showPrompt', orig => function () {
            if (!unlocked())
                return orig.call(this);
            const type = Clutter.get_current_event()?.type();
            if (type === Clutter.EventType.TOUCH_END || type === Clutter.EventType.BUTTON_RELEASE)
                return undefined;
            ext._lift(LIFT_TIME);
            return undefined;
        });
        this._injections.overrideMethod(UnlockDialog.prototype, '_swipeBegin', orig => function (tracker, monitor) {
            if (!unlocked())
                return orig.call(this, tracker, monitor);
            if (monitor !== Main.layoutManager.primaryIndex)
                return undefined;
            // pick the lock screen up where it is, also in the middle of a lift
            const group = ext._shield._lockDialogGroup;
            group.remove_all_transitions();
            const progress = Math.min(1, Math.max(0, -group.translation_y / global.stage.height));
            tracker.confirmSwipe(global.stage.height, [0, 1], progress, 0);
            return undefined;
        });
        this._injections.overrideMethod(UnlockDialog.prototype, '_swipeUpdate', orig => function (tracker, progress) {
            if (!unlocked())
                return orig.call(this, tracker, progress);
            ext._shield._lockDialogGroup.translation_y = -progress * global.stage.height;
            return undefined;
        });
        this._injections.overrideMethod(UnlockDialog.prototype, '_swipeEnd', orig => function (tracker, duration, end) {
            if (!unlocked())
                return orig.call(this, tracker, duration, end);
            if (end >= 1) {
                ext._lift(duration);
            } else {
                ext._shield._lockDialogGroup.ease({
                    translation_y: 0,
                    duration,
                    mode: Clutter.AnimationMode.EASE_OUT_CUBIC,
                });
            }
            return undefined;
        });
        // Typing on the lock screen starts the password prompt in a class handler; catch keys first.
        this._injections.overrideMethod(UnlockDialog.prototype, 'open', orig => function (...args) {
            if (!this._w202KeyId) {
                this._w202KeyId = this.connect('key-press-event', (actor, event) => {
                    if (!unlocked() || MODIFIERS.includes(event.get_key_symbol()))
                        return Clutter.EVENT_PROPAGATE;
                    ext._lift(LIFT_TIME);
                    return Clutter.EVENT_STOP;
                });
            }
            return orig.apply(this, args);
        });

        this._shield.connectObject('active-changed', () => this._onActiveChanged(), this);
        this._settings.connectObject('changed', () => this._armLock(), this);
        this._onActiveChanged();

        // Outputs lit while the shield's curtain is down: show the lock screen.
        this._powerSaveId = Gio.DBus.session.signal_subscribe(null, 'org.freedesktop.DBus.Properties',
            'PropertiesChanged', MUTTER[1], MUTTER[2], Gio.DBusSignalFlags.NONE, (...args) => {
                const [, changed] = args[5].deepUnpack();
                if (changed.PowerSaveMode?.unpack() === 0 && this._shield.active)
                    this._shield._wakeUpScreen();
            });

        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._dbus.export(Gio.DBus.session, OBJECT_PATH);
    }

    disable() {
        // Kept enabled on the lock screen (session-modes); this runs only when the extension is switched off.
        this._dbus.unexport();
        this._dbus = null;
        Gio.DBus.session.signal_unsubscribe(this._powerSaveId);
        this._shield.disconnectObject(this);
        this._settings.disconnectObject(this);
        this._disarm();
        if (this._raiseWhenDownId)
            this._shield.disconnect(this._raiseWhenDownId);
        this._raiseWhenDownId = 0;
        if (this._lifting)
            this._shield._lockDialogGroup.remove_all_transitions();
        this._injections.clear();
        this._injections = null;
        this._settings = null;
        this._shield = null;
    }

    _log(msg) {
        console.log(`${this.uuid}: ${msg}`);
    }

    _lockDelay() {
        if (!this._settings.get_boolean('lock-enabled'))
            return -1;
        return this._settings.get_uint('lock-delay');
    }

    _onActiveChanged() {
        if (this._shield.active) {
            if (!this._offAt)
                this._offAt = GLib.get_real_time();
            this._armLock();
        } else {
            this._offAt = 0;
            this._disarm();
        }
    }

    _armLock() {
        this._disarm();
        if (!this._offAt || this._shield.locked)
            return;
        const delay = this._lockDelay();
        if (delay < 0)
            return;
        const left = Math.max(0, delay - Math.floor((GLib.get_real_time() - this._offAt) / 1000000));
        // an idle source even for 0: active-changed is emitted from inside ScreenShield.activate()
        this._lockTimeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, left, () => {
            this._lockTimeoutId = 0;
            this._lockIfDue();
            return GLib.SOURCE_REMOVE;
        });
    }

    _disarm() {
        if (this._lockTimeoutId)
            GLib.source_remove(this._lockTimeoutId);
        this._lockTimeoutId = 0;
    }

    _lockIfDue() {
        const delay = this._lockDelay();
        if (delay < 0 || !this._offAt || !this._shield.active || this._shield.locked)
            return;
        if (GLib.get_real_time() - this._offAt >= delay * 1000000) {
            this._log(`locking: screen off for ${delay} s or more`);
            this._shield.lock(false);
        }
    }

    _setPowerSave(mode) {
        Gio.DBus.session.call(MUTTER[0], MUTTER[1], 'org.freedesktop.DBus.Properties', 'Set',
            new GLib.Variant('(ssv)', [MUTTER[2], 'PowerSaveMode', GLib.Variant.new_int32(mode)]),
            null, Gio.DBusCallFlags.NONE, -1, null, null);
    }

    // Raise the shield without locking, in the order lock() and the idle path use: the shield's own modal grab
    // (LOCK_SCREEN) first, then the unlock dialog's (UNLOCK_SCREEN) on top of it. In the reverse order the
    // lock-screen grab ends up on top, taking key focus from the dialog and disabling its swipe gesture.
    _raiseShield() {
        if (!this._shield._becomeModal()) {
            this._log('cannot raise the lock screen: modal grab refused');
            return false;
        }
        this._shield.activate(false);
        return true;
    }

    // D-Bus

    ScreenOff() {
        const shield = this._shield;
        if (this._lifting) {
            // a lift was under way: the lock screen stays
            shield._lockDialogGroup.remove_all_transitions();  // onStopped clears _lifting, no deactivate
            shield._lockDialogGroup.translation_y = 0;
        }
        if (shield.active && Main.sessionMode.currentMode !== 'unlock-dialog') {
            // GNOME is finishing a deactivation (its last slide, ~250 ms): raise the shield again once it is down
            this._raiseWhenDown();
        } else if (shield.active) {
            // the lock screen is showing: put its black curtain back before the outputs go off
            shield._activateFade(shield._shortLightbox, 0);
        } else {
            this._raiseShield();
        }
        this._setPowerSave(3);
        this._log(`screen off${this._shield.locked ? ' (locked)' : ''}`);
    }

    ScreenOn() {
        if (this._shield.active)
            this._shield._wakeUpScreen();
        this._setPowerSave(0);
        this._log(`screen on${this._shield.locked ? ' (locked)' : ''}`);
    }

    // Slide the lock screen up from where it is, then let GNOME take the shield down without its own animation.
    _lift(duration) {
        const group = this._shield._lockDialogGroup;
        this._lifting = true;
        group.remove_all_transitions();
        group.ease({
            translation_y: -global.stage.height,
            duration,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onStopped: () => {
                this._lifting = false;
            },
            onComplete: () => this._shield.deactivate(false),
        });
    }

    _raiseWhenDown() {
        if (this._raiseWhenDownId)
            return;
        this._raiseWhenDownId = this._shield.connect('active-changed', () => {
            if (this._shield.active)
                return;
            this._shield.disconnect(this._raiseWhenDownId);
            this._raiseWhenDownId = 0;
            this._raiseShield();
        });
    }
}
