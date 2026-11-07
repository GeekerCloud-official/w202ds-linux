WCN_KERNEL_PROVIDER ?=
WCN_BUILD_HEADERS := WCN_BSP_DIR=$(DEVICES)/wcn/source/wcn_bsp
$(eval $(call module,w202ds-wcn-bus,wcn/source/wcn_bsp,wcn_bsp.ko))
$(eval $(call module,w202ds-wcn-wifi,wcn/source/sprd_wlan_combo,sprd_wlan_combo.ko,w202ds-wcn-bus,$(WCN_BUILD_HEADERS),$(WCN_KERNEL_PROVIDER)))
$(eval $(call module,w202ds-wcn-bluetooth,wcn/source/sprdbt_tty,sprdbt_tty.ko,w202ds-wcn-bus,$(WCN_BUILD_HEADERS)))
.PHONY: w202ds-wcn-driver
w202ds-wcn-driver: w202ds-wcn-wifi w202ds-wcn-bluetooth
SERVER_MODULES += w202ds-wcn-wifi w202ds-wcn-bluetooth
# Wi-Fi and the Bluetooth H4 transport are built here; Bluetooth userspace is in userspace/, GNSS remains outside this build.
