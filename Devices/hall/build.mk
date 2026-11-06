$(eval $(call module,w202ds-hall-input,hall/source/w202-hall-input,w202_hall.ko))
.PHONY: w202ds-hall-driver
w202ds-hall-driver: w202ds-hall-input
SERVER_MODULES += w202ds-hall-input
