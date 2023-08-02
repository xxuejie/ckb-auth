# TARGET := riscv64-unknown-linux-gnu
# CC := $(TARGET)-gcc
# LD := $(TARGET)-gcc
# OBJCOPY := $(TARGET)-objcopy
# AR := $(TARGET)-ar

CC := clang-16
LD := ld.lld-16
OBJCOPY := llvm-objcopy-16
AR := llvm-ar-16

CFLAGS := --target=riscv64 -march=rv64imc_zba_zbb_zbc_zbs -fPIC -O3 -fno-builtin-printf -fno-builtin-memcmp -nostdinc -nostdlib -fvisibility=hidden -fdata-sections -ffunction-sections -I deps/secp256k1-20210801/src -I deps/secp256k1-20210801 -I deps/ckb-c-stdlib-2023 -I deps/ckb-c-stdlib-2023/libc -I deps/ckb-c-stdlib-2023/molecule -I c -I build -Wall -Werror -Wno-nonnull -Wno-unused-function -Wno-bitwise-instead-of-logical -g
LDFLAGS := -Wl,--gc-sections
SECP256K1_SRC_20210801 := deps/secp256k1-20210801/src/ecmult_static_pre_context.h
AUTH_CFLAGS := $(CFLAGS) -I deps/mbedtls/include -I deps/ed25519/src -I c/cardano/nanocbor -Wno-array-bounds

# RSA/mbedtls
PASSED_MBEDTLS_CFLAGS := --target=riscv64 -march=rv64imc_zba_zbb_zbc_zbs -O3 -fPIC -nostdinc -nostdlib -DCKB_DECLARATION_ONLY -I ../../ckb-c-stdlib-2023/libc -fdata-sections -ffunction-sections -fvisibility=hidden

# docker pull nervos/ckb-riscv-gnu-toolchain:gnu-jammy-20230214
BUILDER_DOCKER := nervos/ckb-riscv-gnu-toolchain@sha256:d3f649ef8079395eb25a21ceaeb15674f47eaa2d8cc23adc8bcdae3d5abce6ec

all:  build/secp256k1_data_info_20210801.h $(SECP256K1_SRC_20210801) deps/mbedtls/library/libmbedcrypto.a build/auth build/always_success

all-via-docker: ${PROTOCOL_HEADER}
	mkdir -p build
	docker run --rm -v `pwd`:/code ${BUILDER_DOCKER} bash -c "cd /code && make"

build/always_success: c/always_success.c
	$(CC) $(AUTH_CFLAGS) $(LDFLAGS) -o $@ $<
	$(OBJCOPY) --only-keep-debug $@ $@.debug
	$(OBJCOPY) --strip-debug --strip-all $@


build/secp256k1_data_info_20210801.h: build/dump_secp256k1_data_20210801
	$<

build/dump_secp256k1_data_20210801: c/dump_secp256k1_data_20210801.c $(SECP256K1_SRC_20210801)
	mkdir -p build
	gcc -I deps/ckb-c-stdlib-2023 -I deps/secp256k1-20210801/src -I deps/secp256k1-20210801 -o $@ $<

$(SECP256K1_SRC_20210801):
	cd deps/secp256k1-20210801 && \
		./autogen.sh && \
		./configure --with-bignum=no --enable-ecmult-static-precomputation --enable-endomorphism --enable-module-recovery --with-asm=no && \
		make src/ecmult_static_pre_context.h src/ecmult_static_context.h

deps/mbedtls/library/libmbedcrypto.a:
	cp deps/mbedtls-config-template.h deps/mbedtls/include/mbedtls/config.h
	make -C deps/mbedtls/library APPLE_BUILD=0 AR=$(AR) CC=${CC} LD=${LD} CFLAGS="${PASSED_MBEDTLS_CFLAGS}" libmbedcrypto.a

build/nanocbor/%.o: c/cardano/nanocbor/%.c
	mkdir -p build/nanocbor
	$(CC) -c -DCKB_DECLARATION_ONLY -I c/cardano -I c/cardano/nanocbor $(AUTH_CFLAGS) -o $@ $^
build/libnanocbor.a: build/nanocbor/encoder.o build/nanocbor/decoder.o
	$(AR) cr $@ $^
build/ed25519/%.o: deps/ed25519/src/%.c
	mkdir -p build/ed25519
	$(CC) -c -DCKB_DECLARATION_ONLY $(AUTH_CFLAGS) -o $@ $^
build/libed25519.a: build/ed25519/sign.o build/ed25519/verify.o build/ed25519/sha512.o build/ed25519/sc.o build/ed25519/keypair.o \
					build/ed25519/key_exchange.o build/ed25519/ge.o build/ed25519/fe.o build/ed25519/add_scalar.o
	$(AR) cr $@ $^

build/auth: c/auth.c deps/mbedtls/library/libmbedcrypto.a build/libed25519.a build/libnanocbor.a
	$(CC) $(AUTH_CFLAGS) -DCKB_NO_ENTRY_GP -fPIC -fPIE -c -o build/auth.o c/auth.c
	$(LD) --shared --gc-sections --dynamic-list c/auth.syms -o $@ build/auth.o deps/mbedtls/library/libmbedcrypto.a build/libed25519.a build/libnanocbor.a
	cp $@ $@.debug
	$(OBJCOPY) --strip-debug --strip-all $@

fmt:
	clang-format -i -style="{BasedOnStyle: Google, IndentWidth: 4}" c/*.c c/*.h

clean:
	rm -rf build/*.debug
	rm -f build/auth build/auth_demo
	rm -rf build/secp256k1_data_info_20210801.h build/dump_secp256k1_data_20210801
	rm -rf build/ed25519 build/libed25519.a build/nanocbor build/libnanocbor.a
	cd deps/secp256k1-20210801 && [ -f "Makefile" ] && make clean
	make -C deps/mbedtls/library clean

.PHONY: all all-via-docker

