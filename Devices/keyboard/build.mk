$(eval $(call module,w202ds-keyboard-input,keyboard/source/keyboard-m22,w202_zsk.ko))
.PHONY: w202ds-keyboard-driver
w202ds-keyboard-driver: w202ds-keyboard-input
SERVER_MODULES += w202ds-keyboard-input
