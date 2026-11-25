# Host tests of components/kfind. `make` builds and runs them.
KF      := ../../components/kfind
PY      ?= python3
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -O2 -I$(KF)/include -I$(KF)/third_party/micro-ecc \
           -DuECC_SUPPORTS_secp192r1=0 -DuECC_SUPPORTS_secp224r1=0 \
           -DuECC_SUPPORTS_secp256r1=0 -DuECC_SUPPORTS_secp256k1=0 \
           -DuECC_SUPPORT_COMPRESSED_POINT=0 -DuECC_PLATFORM=uECC_arch_other
SRC     := test_kfind.c $(KF)/src/kfind.c $(KF)/src/kfind_crypto.c $(KF)/third_party/micro-ecc/uECC.c

all: test_kfind
	./test_kfind

vectors.h: ../../tools/reference/fhn_ref.py
	$(PY) $< vectors > $@

test_kfind: $(SRC) vectors.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -o $@ $(SRC)

clean:
	rm -f test_kfind

.PHONY: all clean
