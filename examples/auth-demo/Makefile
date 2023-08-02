# TARGET := riscv64-unknown-linux-gnu
# CC := $(TARGET)-gcc
# LD := $(TARGET)-gcc
# OBJCOPY := $(TARGET)-objcopy

CC := clang-16
OBJCOPY := llvm-objcopy-16

CFLAGS := --target=riscv64 -march=rv64imc_zba_zbb_zbc_zbs -fPIC -O3 -fno-builtin-printf -fno-builtin-memcmp -nostdinc -nostdlib -fvisibility=hidden -fdata-sections -ffunction-sections -I deps/secp256k1/src -I deps/secp256k1 -I deps/ckb-c-std-lib -I deps/ckb-c-std-lib/libc -I deps/ckb-c-std-lib/molecule -I c -I build -Wall -Werror -Wno-nonnull -Wno-unused-function -g
LDFLAGS := -Wl,-static -fdata-sections -ffunction-sections -Wl,--gc-sections
AUTH_CFLAGS=$(subst ckb-c-std-lib,ckb-c-stdlib-2023,$(CFLAGS)) -Wno-array-bounds

# docker pull nervos/ckb-riscv-gnu-toolchain:gnu-jammy-20230214
BUILDER_DOCKER := nervos/ckb-riscv-gnu-toolchain@sha256:d3f649ef8079395eb25a21ceaeb15674f47eaa2d8cc23adc8bcdae3d5abce6ec

all: build/auth_demo

all-via-docker:
	docker run --rm -v `pwd`:/code ${BUILDER_DOCKER} bash -c "cd /code && make -f examples/auth-demo/Makefile all"

build/auth_demo: examples/auth-demo/auth_demo.c c/ckb_auth.h
	$(CC) $(AUTH_CFLAGS) $(LDFLAGS) -o $@ $<
	$(OBJCOPY) --only-keep-debug $@ $@.debug
	$(OBJCOPY) --strip-debug --strip-all $@

clean:
	rm -rf build/auth_demo
