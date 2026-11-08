WCN_KERNEL_PROVIDER ?=
WCN_BUILD_HEADERS := WCN_BSP_DIR=$(DEVICES)/wcn/source/wcn_bsp
$(eval $(call module,w202ds-wcn-bus,wcn/source/wcn_bsp,wcn_bsp.ko))
$(eval $(call module,w202ds-wcn-wifi,wcn/source/sprd_wlan_combo,sprd_wlan_combo.ko,w202ds-wcn-bus,$(WCN_BUILD_HEADERS),$(WCN_KERNEL_PROVIDER)))
$(eval $(call module,w202ds-wcn-bluetooth,wcn/source/sprdbt_tty,sprdbt_tty.ko,w202ds-wcn-bus,$(WCN_BUILD_HEADERS)))
# GNSS power/NMEA and 3D fix are tested; keep opt-in pending suspend/resume and long-term validation.
$(eval $(call module,w202ds-wcn-gnss,wcn/source/gnss-power,w202ds_gnss_power.ko,w202ds-wcn-bus,$(WCN_BUILD_HEADERS)))
.PHONY: w202ds-wcn-driver
w202ds-wcn-driver: w202ds-wcn-wifi w202ds-wcn-bluetooth
SERVER_MODULES += w202ds-wcn-wifi w202ds-wcn-bluetooth

# Optional stock Linux Bluetooth modules, built from an explicit matching kernel
# source with a separately prepared BT-enabled KDIR. Never change a shared KDIR.
ifneq ($(strip $(WCN_BT_KERNEL_SOURCE)),)
$(eval $(call module,w202ds-bluetooth-core,$(WCN_BT_KERNEL_SOURCE)/net/bluetooth,bluetooth.ko))
$(eval $(call module,w202ds-bluetooth-uart,$(WCN_BT_KERNEL_SOURCE)/drivers/bluetooth,hci_uart.ko,w202ds-bluetooth-core))
$(eval $(call module,w202ds-bluetooth-rfcomm,$(WCN_BT_KERNEL_SOURCE)/net/bluetooth/rfcomm,rfcomm.ko,w202ds-bluetooth-core))
$(eval $(call module,w202ds-bluetooth-bnep,$(WCN_BT_KERNEL_SOURCE)/net/bluetooth/bnep,bnep.ko,w202ds-bluetooth-core))
$(eval $(call module,w202ds-bluetooth-hidp,$(WCN_BT_KERNEL_SOURCE)/net/bluetooth/hidp,hidp.ko,w202ds-bluetooth-core))
endif
