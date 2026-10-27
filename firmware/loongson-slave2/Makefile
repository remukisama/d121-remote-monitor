TOOLCHAIN ?= /opt/loongson-gnu-toolchain-8.3-x86_64-loongarch64-linux-gnu-rc1.3-1/bin/loongarch64-linux-gnu-
CC = $(TOOLCHAIN)gcc
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -Wformat=2 -Wshadow
SOURCES = main.c protocol.c metrics.c

all: build/loongson-slave2

build/loongson-slave2: $(SOURCES) protocol.h metrics.h modbus_rtu.h
	mkdir -p build
	$(CC) $(CFLAGS) $(SOURCES) -pthread -o $@

test:
	mkdir -p build
	gcc $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit.c protocol.c metrics.c -I. -o build/unit-test
	ASAN_OPTIONS=detect_leaks=1 ./build/unit-test
	gcc $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/runtime.c protocol.c metrics.c -I. -pthread -lutil -Wl,--wrap=open -Wl,--wrap=ioctl -o build/runtime-test
	ASAN_OPTIONS=detect_leaks=1 timeout 10s ./build/runtime-test
	gcc $(CFLAGS) -DLS_HOST_TEST $(SOURCES) -pthread -o build/host-slave
	python3 tests/integration.py

package: all
	mkdir -p build/package
	$(TOOLCHAIN)strip -o build/package/loongson-slave2 build/loongson-slave2
	cp loongson-slave2.service loongson-slave2-reboot install.sh build/package/
	chmod 755 build/package/loongson-slave2 build/package/loongson-slave2-reboot build/package/install.sh
	chmod 644 build/package/loongson-slave2.service
	cd build/package && sha256sum loongson-slave2 loongson-slave2.service loongson-slave2-reboot install.sh > SHA256SUMS
	cd build/package && tar -czf ../loongson-slave2.tar.gz loongson-slave2 loongson-slave2.service loongson-slave2-reboot install.sh SHA256SUMS

.PHONY: all test package
