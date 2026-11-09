// SPDX-License-Identifier: GPL-2.0-or-later
//
// W202DS Quick Settings toggles: location, Wi-Fi hotspot, mobile data and flashlight.
//
// Location is GNOME's location privacy setting, the same key as the Settings switch; w202-gps-location-switch
// powers the GPS receiver from it. Hotspot and mobile data bring the NetworkManager connections "Hotspot" and
// "W202DS Cellular" up and down. NetworkManager runs one connection per device, so turning on the hotspot takes
// wlan0 off the Wi-Fi network it is on; the subtitle says so. The flashlight is the LED class device *:torch,
// switched between 0 and max_brightness; LED brightness has no change notification, so it is read again each
// second while Quick Settings is open. It is switched off when the extension is. The extension stays enabled on
// the lock screen (session-modes) so that locking leaves a lit flashlight on; the other toggles are hidden there.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import NM from 'gi://NM';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {QuickToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Extension, gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

const HOTSPOT_ID = 'Hotspot';
const CELLULAR_ID = 'W202DS Cellular';
const LEDS_DIR = '/sys/class/leds';
const TORCH_SUFFIX = ':torch';
const TORCH_PREFERRED = 'white:torch';
const TORCH_POLL = 1; // s, while Quick Settings is open

const {ACTIVATING, ACTIVATED, DEACTIVATING} = NM.ActiveConnectionState;

const LocationToggle = GObject.registerClass(
class LocationToggle extends QuickToggle {
    _init() {
        super._init({title: _('Location'), toggleMode: true});

        this._settings = new Gio.Settings({schema_id: 'org.gnome.system.location'});
        this._settings.bind('enabled', this, 'checked', Gio.SettingsBindFlags.DEFAULT);
        this.connect('notify::checked', () => this._sync());
        this.connect('destroy', () => Gio.Settings.unbind(this, 'checked'));
        this._sync();
    }

    _sync() {
        this.iconName = this.checked
            ? 'location-services-active-symbolic'
            : 'location-services-disabled-symbolic';
    }

    syncSession() {
        this.visible = !Main.sessionMode.isLocked;
    }
});

// Toggles one NetworkManager connection, found by its name (connection.id). Hidden while the client is not ready,
// the connection does not exist or the screen is locked.
const ConnectionToggle = GObject.registerClass(
class ConnectionToggle extends QuickToggle {
    _init(connectionId, params) {
        super._init({...params, visible: false});
        this._connectionId = connectionId;
        this._client = null;
        this._connection = null;
        this._active = null;
        this.connect('clicked', () => this._toggle());
        this.connect('destroy', () => {
            this._client?.disconnectObject(this);
            this._connection?.disconnectObject(this);
            this._active?.disconnectObject(this);
        });
    }

    setClient(client) {
        this._client = client;
        client.connectObject(
            'notify::active-connections', () => this._sync(),
            'connection-added', () => this._sync(),
            'connection-removed', () => this._sync(),
            this);
        this._sync();
    }

    syncSession() {
        if (this._client)
            this._sync();
    }

    _sync() {
        const connection = this._client.get_connections().find(c => c.get_id() === this._connectionId) ?? null;
        if (connection !== this._connection) {
            this._connection?.disconnectObject(this);
            this._connection = connection;
            connection?.connectObject('changed', () => this._sync(), this);
        }
        const active = this._client.get_active_connections().find(a => a.get_id() === this._connectionId) ?? null;
        if (active !== this._active) {
            this._active?.disconnectObject(this);
            this._active = active;
            active?.connectObject('notify::state', () => this._sync(), this);
        }
        const state = active?.get_state() ?? NM.ActiveConnectionState.DEACTIVATED;
        this.visible = connection !== null && !Main.sessionMode.isLocked;
        this.checked = state === ACTIVATING || state === ACTIVATED;
        if (state === ACTIVATING)
            this.subtitle = _('Turning on…');
        else if (state === DEACTIVATING)
            this.subtitle = _('Turning off…');
        else
            this.subtitle = connection ? this._stateSubtitle(state === ACTIVATED) : null;
    }

    _stateSubtitle(_on) {
        return null;
    }

    // Device to activate the connection on, null to let NetworkManager choose.
    _device() {
        return null;
    }

    _toggle() {
        const client = this._client;
        const active = this._active;
        if (active && (active.get_state() === ACTIVATING || active.get_state() === ACTIVATED)) {
            client.deactivate_connection_async(active, null, (_c, result) => {
                try {
                    client.deactivate_connection_finish(result);
                } catch (e) {
                    Main.notifyError(_('Could not turn off %s').format(this.title), e.message);
                }
            });
        } else if (this._connection) {
            client.activate_connection_async(this._connection, this._device(), null, null, (_c, result) => {
                try {
                    client.activate_connection_finish(result);
                } catch (e) {
                    Main.notifyError(_('Could not turn on %s').format(this.title), e.message);
                }
            });
        }
        this._sync();
    }
});

const HotspotToggle = GObject.registerClass(
class HotspotToggle extends ConnectionToggle {
    _init() {
        super._init(HOTSPOT_ID, {title: _('Hotspot'), iconName: 'network-wireless-hotspot-symbolic'});
    }

    _device() {
        const iface = this._connection.get_setting_connection()?.get_interface_name();
        if (iface)
            return this._client.get_device_by_iface(iface);
        return this._client.get_devices().find(d => d instanceof NM.DeviceWifi && d.get_managed()) ?? null;
    }

    _stateSubtitle(on) {
        if (on) {
            const ssid = this._connection.get_setting_wireless()?.get_ssid();
            return ssid ? NM.utils_ssid_to_utf8(ssid.get_data()) : null;
        }
        // the hotspot replaces the Wi-Fi network the device is connected to
        const current = this._device()?.get_active_connection();
        if (current && current.get_id() !== HOTSPOT_ID)
            return _('Disconnects Wi-Fi');
        return null;
    }
});

const MobileDataToggle = GObject.registerClass(
class MobileDataToggle extends ConnectionToggle {
    _init() {
        super._init(CELLULAR_ID, {title: _('Mobile Data'), iconName: 'network-cellular-signal-good-symbolic'});
    }

    _sync() {
        super._sync();
        this.iconName = this.checked
            ? 'network-cellular-signal-good-symbolic'
            : 'network-cellular-offline-symbolic';
    }
});

function readNumber(file) {
    const [, contents] = file.load_contents(null);
    return parseInt(new TextDecoder().decode(contents), 10);
}

// The first LED class device named *:torch, white:torch preferred, or null.
function findTorch() {
    const names = [];
    try {
        const children = Gio.File.new_for_path(LEDS_DIR).enumerate_children(
            'standard::name', Gio.FileQueryInfoFlags.NONE, null);
        for (let info; (info = children.next_file(null));)
            names.push(info.get_name());
        children.close(null);
    } catch (e) {
        return null;
    }
    const name = names.includes(TORCH_PREFERRED)
        ? TORCH_PREFERRED
        : names.filter(n => n.endsWith(TORCH_SUFFIX)).sort()[0];
    return name ? Gio.File.new_for_path(`${LEDS_DIR}/${name}`) : null;
}

// sysfs attribute: written in place, not through a replacement file
function writeNumber(file, value) {
    const stream = file.append_to(Gio.FileCreateFlags.NONE, null);
    stream.write_all(new TextEncoder().encode(`${value}\n`), null);
    stream.close(null);
}

const FlashlightToggle = GObject.registerClass(
class FlashlightToggle extends QuickToggle {
    _init(gicon) {
        super._init({title: _('Flashlight'), gicon, visible: false});
        this._brightness = null;
        this._maxBrightness = 0;
        this.connect('clicked', () => this._set(!this.checked));
        // switched off with the extension; it stays enabled on the lock screen, so this is not on locking
        this.connect('destroy', () => {
            try {
                if (this._brightness && readNumber(this._brightness) > 0)
                    writeNumber(this._brightness, 0);
            } catch (e) {
                console.warn(`${this._brightness.get_path()}: ${e.message}`);
            }
        });
        this.rescan();
    }

    // Looks for the torch again (its driver may load later) and reads its state.
    rescan() {
        const led = findTorch();
        this._brightness = null;
        if (led) {
            try {
                this._maxBrightness = readNumber(led.get_child('max_brightness'));
                const brightness = led.get_child('brightness');
                const info = brightness.query_info('access::can-write', Gio.FileQueryInfoFlags.NONE, null);
                this.reactive = info.get_attribute_boolean('access::can-write');
                this.subtitle = this.reactive ? null : _('No permission');
                this._brightness = brightness;
            } catch (e) {
                console.warn(`${led.get_path()}: ${e.message}`);
            }
        }
        this.refresh();
    }

    refresh() {
        let brightness = null;
        try {
            brightness = this._brightness ? readNumber(this._brightness) : null;
        } catch (e) {
            // the device went away
        }
        this.visible = brightness !== null;
        this.checked = brightness > 0;
    }

    _set(on) {
        try {
            writeNumber(this._brightness, on ? this._maxBrightness : 0);
        } catch (e) {
            Main.notifyError(_('Could not switch the flashlight'), e.message);
        }
        this.refresh();
    }
});

const Indicator = GObject.registerClass(
class Indicator extends SystemIndicator {
    _init(extension) {
        super._init();

        this._hotspot = new HotspotToggle();
        this._mobileData = new MobileDataToggle();
        this._flashlight = new FlashlightToggle(
            Gio.FileIcon.new(extension.dir.resolve_relative_path('icons/w202-flashlight-symbolic.svg')));
        this._location = new LocationToggle();
        this.quickSettingsItems.push(this._location, this._hotspot, this._mobileData, this._flashlight);

        // the lock screen keeps only the flashlight
        const syncSession = () => [this._location, this._hotspot, this._mobileData].forEach(t => t.syncSession());
        Main.sessionMode.connectObject('updated', syncSession, this);
        syncSession();

        this._cancellable = new Gio.Cancellable();
        NM.Client.new_async(this._cancellable, (_o, result) => {
            let client;
            try {
                client = NM.Client.new_finish(result);
            } catch (e) {
                if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    console.error(`NetworkManager client: ${e.message}`);
                return;
            }
            this._hotspot.setClient(client);
            this._mobileData.setClient(client);
        });

        this._pollId = 0;
        Main.panel.statusArea.quickSettings.menu.connectObject('open-state-changed', (_m, open) => {
            this._stopPolling();
            if (!open)
                return;
            this._flashlight.rescan();
            this._pollId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, TORCH_POLL, () => {
                this._flashlight.refresh();
                return GLib.SOURCE_CONTINUE;
            });
        }, this);

        this.connect('destroy', () => {
            this._cancellable.cancel();
            this._stopPolling();
            Main.panel.statusArea.quickSettings.menu.disconnectObject(this);
            Main.sessionMode.disconnectObject(this);
            this.quickSettingsItems.forEach(item => item.destroy());
        });
    }

    _stopPolling() {
        if (this._pollId)
            GLib.source_remove(this._pollId);
        this._pollId = 0;
    }
});

export default class W202QuickToggles extends Extension {
    enable() {
        this._indicator = new Indicator(this);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator.destroy();
        this._indicator = null;
    }
}
