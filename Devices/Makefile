# SPDX-License-Identifier: GPL-2.0-only
# Run kernel/module builds locally; no install or hardware access occurs here.
.DEFAULT_GOAL := help
DEVICES := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PYTHON ?= python3
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
JOBS ?= 4
W ?= 1
PROFILE ?= debug
KERNEL_OUT ?= $(DEVICES)/../output/kernel-$(PROFILE)
KERNEL_INPUT := $(if $(strip $(KDIR)),$(abspath $(KDIR)),$(abspath $(KERNEL_OUT)))
KERNEL_ROOT := $(if $(wildcard $(KERNEL_INPUT)/../manifest.json),$(abspath $(KERNEL_INPUT)/..),$(KERNEL_INPUT))
MODULE_KDIR := $(if $(wildcard $(KERNEL_ROOT)/manifest.json),$(KERNEL_ROOT)/build,$(KERNEL_ROOT))
OUT ?= $(KERNEL_ROOT)/modules
SKIP_KERNEL ?= 0
SKIP_KERNEL_CHECK ?= 0
KERNEL_PREPARED ?= 0
MODEL ?= $(DEVICES)/../config/kernel/model.toml
FRAGMENTS ?=

# Module declarations live with their hardware, not in a second source tree.
# $(call module,id,source,one .ko,provider IDs,make variables,kernel receipts)
define module
MODULES += $(1)
$(1).source := $(2)
$(1).ko := $(3)
$(1).deps := $(4)
$(1).vars := $(5)
$(1).kernels := $(6)
endef

include $(sort $(wildcard $(DEVICES)/*/build.mk))

providers = $(sort $(foreach dep,$($(1).deps),$(dep) $(call providers,$(dep))))
source_path = $(if $(filter /%,$(1)),$(1),$(DEVICES)/$(1))

define module_rule
.PHONY: $(1)
$(1): $$($(1).deps) | kernel-ready
	$$(PYTHON) -B "$$(DEVICES)/build-module.py" \
	  --id "$(1)" --devices "$$(DEVICES)" \
	  --source "$$(call source_path,$$($(1).source))" --target "$$($(1).ko)" \
	  $$(if $$(filter /%,$$($(1).source)),--external-source-root "$$($(1).source)") \
	  --kdir "$$(MODULE_KDIR)" --out "$$(OUT)" --arch "$$(ARCH)" \
	  --cross-compile "$$(CROSS_COMPILE)" --jobs "$$(JOBS)" --warnings "$$(W)" \
	  $$(if $$(filter 1,$$(SKIP_KERNEL_CHECK)),--skip-kernel-check) \
	  --model "$$(MODEL)" --profile "$$(PROFILE)" \
	  $$(foreach f,$$(FRAGMENTS),--fragment "$$(f)") \
	  $$(foreach p,$$(call providers,$(1)),--provider "$$(OUT)/$$(p)/build.json") \
	  $$(foreach p,$$($(1).kernels),--kernel-provider "$$(DEVICES)/$$(p)") \
	  $$(foreach v,$$($(1).vars),--make-var "$$(v)")
endef
$(foreach m,$(MODULES),$(eval $(call module_rule,$(m))))

.PHONY: help list all kernel-ready
kernel-ready:
ifeq ($(KERNEL_PREPARED),0)
	$(if $(and $(filter 1,$(SKIP_KERNEL) $(SKIP_KERNEL_CHECK)),$(filter undefined,$(origin KDIR)),$(filter file,$(origin KERNEL_OUT))),$(error Skipping the kernel requires an explicit KDIR or KERNEL_OUT))
	$(PYTHON) -B "$(DEVICES)/../scripts/build.py" --prepare-only \
	  --kernel-out "$(KERNEL_ROOT)" --model "$(MODEL)" --profile "$(PROFILE)" \
	  --cross-compile "$(CROSS_COMPILE)" --jobs "$(JOBS)" \
	  $(foreach f,$(FRAGMENTS),--fragment "$(f)") \
	  $(if $(filter 1,$(SKIP_KERNEL)),--skip-kernel) \
	  $(if $(filter 1,$(SKIP_KERNEL_CHECK)),--skip-kernel-check)
endif
help:
	@printf '%s\n' \
	  'W202DS per-module build (local)' \
	  'make -C 6.18.35/Devices list' \
	  'make -C 6.18.35/Devices KDIR=/matched/kernel/output OUT=/new/work/modules w202ds-sensorhub-driver' \
	  'Missing default kernel output is built first; changed Git/config/output files are refused.' \
	  'Reuse old output: KERNEL_OUT=/old/output SKIP_KERNEL=1 SKIP_KERNEL_CHECK=1' \
	  'Use -n to inspect commands. Each target has its own output, symbols, log and receipt.' \
	  'all builds current Server modules; diagnostics and reference targets are explicit.'
list:
	@$(foreach m,$(sort $(MODULES)),printf '%-25s %-28s %s\n' '$(m)' '$($(m).ko)' 'providers: $($(m).deps)';)
all: $(SERVER_MODULES)
