CHIP_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all clean

all: chip.wasm

chip.wasm: chip.c wokwi-api.h
	docker run --rm -v "$(CHIP_DIR)":/work -w /work wokwi/builder-clang-wasm \
	  clang --target=wasm32-unknown-wasi --sysroot /opt/wasi-libc \
	    -nostartfiles -Wl,--export-table -Wl,--no-entry -Werror \
	    -o chip.wasm chip.c

clean:
	rm -f chip.wasm
