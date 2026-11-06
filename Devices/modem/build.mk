MODEM_BUILD_HEADERS := WCN_BSP_DIR=$(DEVICES)/wcn/source/wcn_bsp
# Linux CP initialization; verified on Server 2026-11-05. Runtime requires matching firmware/services.
$(eval $(call module,w202ds-modem-avb-seed,modem/source/boot_prepare,w202_avb_seed.ko,w202ds-modem-trusty,))
$(eval $(call module,w202ds-modem-cp-prepare,modem/source/boot_prepare,w202_cp_prepare.ko,,))
$(eval $(call module,w202ds-modem-systimer,modem/source/sprd_modem,misc/sprd_systimer.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-timesync,modem/source/sprd_modem,misc/sprd_time_sync.ko,w202ds-modem-systimer,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-timesync-cp,modem/source/sprd_modem,misc/sprd_time_sync_cp.ko,w202ds-modem-timesync,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-trusty,modem/source/sprd_modem,trusty/trusty.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-trusty-irq,modem/source/sprd_modem,trusty/trusty-irq.ko,w202ds-modem-trusty,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-trusty-pm,modem/source/sprd_modem,trusty/trusty-pm.ko,w202ds-modem-trusty,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-trusty-log,modem/source/sprd_modem,trusty/trusty-log.ko,w202ds-modem-trusty,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-trusty-virtio,modem/source/sprd_modem,trusty/trusty-virtio.ko,w202ds-modem-trusty,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-trusty-ipc,modem/source/sprd_modem,trusty/trusty-ipc.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-shm,modem/source/sprd_modem,trusty/sprd_shm.ko,w202ds-modem-trusty,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-mailbox,modem/source/sprd_modem,mailbox/unisoc-mailbox.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-virt-bus,modem/source/sprd_modem,sipc_virt_bus/sprd-sipc-virt-bus.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-power,modem/source/sprd_modem,power_manager/sprd_power_manager.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sipc-core,modem/source/sprd_modem,sipc/sipc-core.ko,w202ds-modem-mailbox w202ds-modem-power,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-spipe,modem/source/sprd_modem,sipc/spipe.ko,w202ds-modem-sipc-core,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-spool,modem/source/sprd_modem,sipc/spool.ko,w202ds-modem-sipc-core,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-slog,modem/source/sprd_modem,sipc/slog_bridge.ko,w202ds-modem-sipc-core,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sbuf,modem/source/sprd_modem,sipc/sbuf_bridge.ko,w202ds-modem-sipc-core w202ds-modem-power,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sblock,modem/source/sprd_modem,sipc/sblock_bridge.ko,w202ds-modem-sipc-core,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sipx,modem/source/sprd_modem,sipc/sipx.ko,w202ds-modem-sipc-core,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-loader,modem/source/sprd_modem,modem_loader/sprd_modem_loader.ko,w202ds-modem-sipc-core w202ds-modem-power,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-seth,modem/source/sprd_modem,seth/seth.ko,w202ds-modem-sipc-core w202ds-modem-sipx,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sipa-sys,modem/source/sprd_modem,sipa_sys/sipa-sys.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sipa-core,modem/source/sprd_modem,sipa_v3/sipa-core.ko,,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sipa-eth,modem/source/sprd_modem,sipa_v3/sipa_eth.ko,w202ds-modem-sipa-core,$(MODEM_BUILD_HEADERS)))
$(eval $(call module,w202ds-modem-sipa-delegate,modem/source/sprd_modem,sipa_delegate/sipa-dele.ko,w202ds-modem-sipc-core w202ds-modem-sipa-core,$(MODEM_BUILD_HEADERS)))
.PHONY: w202ds-modem-driver w202ds-modem-driver-reference
w202ds-modem-driver: w202ds-modem-trusty w202ds-modem-trusty-irq w202ds-modem-trusty-pm w202ds-modem-trusty-log \
       w202ds-modem-trusty-virtio w202ds-modem-shm w202ds-modem-mailbox w202ds-modem-virt-bus w202ds-modem-power \
       w202ds-modem-sipc-core w202ds-modem-spipe w202ds-modem-loader w202ds-modem-sipa-core w202ds-modem-sipa-delegate
w202ds-modem-driver-reference: $(filter w202ds-modem-%,$(MODULES))
SERVER_MODULES += w202ds-modem-driver
# Build dependencies are not a live-device unload/reload plan. SIPC/SIPA remain loaded.

SERVER_MODULES += w202ds-modem-avb-seed w202ds-modem-cp-prepare
