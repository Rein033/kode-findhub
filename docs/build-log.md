# Build log

## 2026-11-25: kodeOS 0.1.5 + kode-findhub

* Sources: KodeOS `7ebfdd8` (0.1.5), sdk `d775f3d` (v0.1.5), then `os/apply.sh`.
* Toolchain: ESP-IDF v6.1, riscv32-esp-elf gcc 15.2.0, target esp32p4.
* `idf.py build`: **passed**, with no warnings in the new or changed files.
  `-Werror` is on.
* `kodeos.bin`: 3 987 552 bytes (0x3cd860). The app partition is 0x400000,
  so 5 % is free.
* `CONFIG_KODEOS_FINDHUB=y` (the default in kodeOS).
* `os/apply.sh` was tested on a fresh clone: the result is byte-identical to
  the tree that was built.
* Host tests: `make -C tests/host` gives 59 passed, 0 failed.

Not done yet:

* Booting the image in kode-sim. The build machine ran out of disk. The
  simulator also does not model the BLE air, so it would only prove that
  kodeOS boots and that `findhub_info` answers.
* The SDK's own host suite (`sdk/tests/host`).
* `tests/tools/test_kodefind.py`, which needs a GoogleFindMyTools checkout.
* Anything on a real Dot.

Flash the app only, over an existing kodeOS 0.1.5:

```bash
python -m esptool --chip esp32p4 -b 460800 write-flash 0x20000 dist/kodeos-0.1.5-findhub.bin
```
