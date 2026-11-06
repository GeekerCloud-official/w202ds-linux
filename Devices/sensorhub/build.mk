$(eval $(call module,w202ds-sensorhub-iio,sensorhub/source/w202-hub-iio,w202_hub_iio.ko,w202ds-modem-sipc-core))
.PHONY: w202ds-sensorhub-driver
w202ds-sensorhub-driver: w202ds-sensorhub-iio
SERVER_MODULES += w202ds-sensorhub-iio
# Raw hubctl and IIO must not own the CH RX rings at the same time.
