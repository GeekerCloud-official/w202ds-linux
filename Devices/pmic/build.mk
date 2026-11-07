$(eval $(call module,w202ds-pmic-telemetry,pmic/source/w202-telemetry,w202_ump9620_power.ko))
$(eval $(call module,w202ds-pmic-typec,pmic/source/ump9620-typec,w202_ump9620_typec.ko))
.PHONY: w202ds-pmic-driver
w202ds-pmic-driver: w202ds-pmic-telemetry
SERVER_MODULES += w202ds-pmic-telemetry w202ds-pmic-typec
# UMP9620 MFD/regmap/ADC providers remain in the matched kernel.
# BLTC/vibrator are not yet independent module targets.
