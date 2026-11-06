WCN_KERNEL_PROVIDER ?=
WCN_BUILD_HEADERS := WCN_BSP_DIR=$(DEVICES)/wcn/source/wcn_bsp
$(eval $(call module,w202ds-wcn-bus,wcn/source/wcn_bsp,wcn_bsp.ko))
$(eval $(call module,w202ds-wcn-wifi,wcn/source/sprd_wlan_combo,sprd_wlan_combo.ko,w202ds-wcn-bus,$(WCN_BUILD_HEADERS),$(WCN_KERNEL_PROVIDER)))
.PHONY: w202ds-wcn-driver
w202ds-wcn-driver: w202ds-wcn-wifi
SERVER_MODULES += w202ds-wcn-wifi
# This import includes the verified Wi-Fi path; BT/GNSS remain outside this build.
