// SPDX-License-Identifier: GPL-2.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami
import org.kde.plasma.workspace.dbus as DBus

Item {
    id: root

    function syncKeyboard() {
        const enabled = Kirigami.Settings.tabletMode;
        const reply = DBus.SessionBus.asyncCall({
            service: "org.kde.KWin",
            path: "/VirtualKeyboard",
            iface: "org.freedesktop.DBus.Properties",
            member: "Set",
            arguments: ["org.kde.kwin.VirtualKeyboard", "enabled", new DBus.variant(enabled)]
        });
        reply.finished.connect(function () {
            if (reply.isError) {
                console.warn("W202DS virtual keyboard:", reply.error.message);
            } else {
                console.info("W202DS virtual keyboard enabled:", enabled);
            }
        });
    }

    Component.onCompleted: syncKeyboard()
    Connections {
        target: Kirigami.Settings
        function onTabletModeChanged() { root.syncKeyboard(); }
    }
}
