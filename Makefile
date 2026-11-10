# Host tools + calculator binary.
#   make host      flysim_cli, ui_preview (clang, with sanitizer variants)
#   make nlink     build/nlink/nlink, the USB link tool (needs vendor/libnspire, libusb)
#   make calc      build/calc/drosophila.tns via the Ndless SDK
#   make data      build/calc/fly*.tns from the connectome (python pipeline)
#   make test      pytest (unit, integration, whole connectome)
#   make coverage  the same with line coverage of pipeline/

NDLESS ?= $(CURDIR)/vendor/ndless-sdk/lib/ndless-sdk
PY ?= .venv/bin/python
NDPATH = PATH="$(NDLESS)/bin:$$PATH"

CORE = core/flysim.c
UI = ui/gfx.c ui/app.c ui/app_panel.c ui/app_fly.c ui/app_modal.c
INC = -Icore -Iui
HOSTCC = clang -std=c99 -Wall -Wextra -Wpedantic $(INC)
CALC_CFLAGS = -std=gnu99 -Wall -W -marm -O2 -fno-strict-aliasing $(INC)

HOST = build/host
CALC = build/calc

.PHONY: all host calc nlink data test coverage clean
all: host calc

host: $(HOST)/flysim_cli $(HOST)/flysim_cli_asan $(HOST)/ui_preview $(HOST)/ui_preview_asan

$(HOST)/flysim_cli: $(CORE) core/flysim.h host/flysim_cli.c
	@mkdir -p $(HOST)
	$(HOSTCC) -O2 $(CORE) host/flysim_cli.c -o $@

$(HOST)/flysim_cli_asan: $(CORE) core/flysim.h host/flysim_cli.c
	@mkdir -p $(HOST)
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(CORE) host/flysim_cli.c -o $@

$(HOST)/ui_preview: $(CORE) $(UI) core/flysim.h ui/*.h host/ui_preview.c
	@mkdir -p $(HOST)
	$(HOSTCC) -O2 $(CORE) $(UI) host/ui_preview.c -o $@

$(HOST)/ui_preview_asan: $(CORE) $(UI) core/flysim.h ui/*.h host/ui_preview.c
	@mkdir -p $(HOST)
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(CORE) $(UI) host/ui_preview.c -o $@

# The link tool: tools/nlink/nlink.c on top of libnspire, compiled from its
# sources (see README, "What is not in the repository").
LIBNSPIRE = $(CURDIR)/vendor/libnspire/src
NLINK_DIR ?= build/nlink
NLINK_SRC = $(wildcard $(LIBNSPIRE)/*.c $(LIBNSPIRE)/services/*.c) $(CURDIR)/tools/nlink/nlink.c
NLINK_INC = -I. -I$(LIBNSPIRE) -I$(LIBNSPIRE)/api $(shell pkg-config --cflags libusb-1.0)

nlink: $(NLINK_DIR)/nlink

$(NLINK_DIR)/nlink: tools/nlink/nlink.c
	@mkdir -p $(NLINK_DIR)
	echo '#define ENDIAN_LITTLE 1' > $(NLINK_DIR)/config.h
	cd $(NLINK_DIR) && cc -c -O2 $(NLINK_INC) $(NLINK_SRC)
	cd $(NLINK_DIR) && c++ -c -O2 -std=c++17 $(NLINK_INC) $(LIBNSPIRE)/cx2.cpp
	c++ $(NLINK_DIR)/*.o $(shell pkg-config --libs libusb-1.0) -o $@

calc: $(CALC)/drosophila.tns

$(CALC)/drosophila.tns: $(CORE) $(UI) core/flysim.h ui/*.h calc/main.c calc/plat_nspire.c calc/bench.c
	@mkdir -p $(CALC) build/calc-obj
	$(NDPATH) nspire-gcc $(CALC_CFLAGS) $(CORE) $(UI) calc/main.c calc/plat_nspire.c calc/bench.c -o build/calc-obj/drosophila.elf
	$(NDPATH) genzehn --input build/calc-obj/drosophila.elf --output build/calc-obj/drosophila.zehn --name "drosophila" --author "fruitfly-calc" --240x320-support true --uses-lcd-blit true
	$(NDPATH) make-prg build/calc-obj/drosophila.zehn $@

data:
	$(PY) -m pipeline.build

test: host
	$(PY) -m pytest -q

# Compiled (numba) kernels are invisible to coverage, so the unit tests run
# once more with the JIT off; the whole-connectome tests need it on.
coverage: host
	NUMBA_DISABLE_JIT=1 $(PY) -m pytest -q -m unit --cov=pipeline --cov-report=
	$(PY) -m pytest -q -m "integration or connectome" --cov=pipeline --cov-append --cov-report=term

clean:
	rm -rf build/host build/calc-obj $(CALC)/drosophila.tns
