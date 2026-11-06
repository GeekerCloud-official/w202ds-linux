$(eval $(call module,w202ds-pmic-telemetry,pmic/source/w202-telemetry,w202_ump9620_power.ko))
.PHONY: w202ds-pmic-driver
w202ds-pmic-driver: w202ds-pmic-telemetry
SERVER_MODULES += w202ds-pmic-telemetry
# UMP9620 MFD/regmap/ADC providers remain in the matched kernel.
# BLTC/vibrator vendor references are not yet independent mainline targets.
