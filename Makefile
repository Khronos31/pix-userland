CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
OS_LIBS ?= -pthread
WARN = -std=c11 -Wall -Wextra -Wpedantic
USB_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libusb-1.0)
USB_LIBS ?= $(shell $(PKG_CONFIG) --libs libusb-1.0)
CORE = src/crypto.o src/platform.o src/protocol.o src/transport.o
OBJ = $(CORE) src/main.o src/usb_libusb.o
.PHONY: all clean test
all: pix-ts
pix-ts: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(USB_LIBS) $(OS_LIBS)
%.o: %.c
	$(CC) $(CPPFLAGS) $(USB_CFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c -o $@ $<
tests/test-crypto: tests/test_crypto.o src/crypto.o src/platform.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-transport: tests/test_transport.o src/crypto.o src/transport.o src/platform.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(OS_LIBS)

test: pix-ts tests/test-crypto tests/test-transport
	./tests/test-crypto
	./tests/test-transport
	./tests/test_cli.sh
clean:
	rm -f pix-ts pix-ts.exe tests/*.exe src/*.o src/*.d tests/*.o tests/*.d tests/test-crypto tests/test-transport
-include $(OBJ:.o=.d) tests/test_crypto.d tests/test_transport.d
