$(eval $(call module,w202ds-charging-control,charging/source/w202-bq-control,w202_bq_control.ko))
.PHONY: w202ds-charging-driver
w202ds-charging-driver: w202ds-charging-control
SERVER_MODULES += w202ds-charging-control
