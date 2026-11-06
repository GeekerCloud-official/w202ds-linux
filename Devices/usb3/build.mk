
$(eval $(call module,w202ds-usb3-legacy-phy,usb3/source/legacy-phy,phy-sprd-w202-dummy.ko,,CONFIG_USB_W202DS_DUMMY_PHY=m))
.PHONY: w202ds-usb3-driver
w202ds-usb3-driver: w202ds-usb3-legacy-phy
