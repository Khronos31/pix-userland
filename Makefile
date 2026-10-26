CC ?= cc
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
OS_LIBS ?= -pthread
WARN = -std=c11 -Wall -Wextra -Wpedantic
USB_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libusb-1.0)
USB_LIBS ?= $(shell $(PKG_CONFIG) --libs libusb-1.0)
CORE = src/pid_filter.o src/control.o src/channel.o src/crypto.o src/platform.o src/protocol.o src/transport.o
OBJ = src/usb_selector.o src/usb_listing.o $(CORE) src/diagnostic.o src/main.o src/usb_libusb.o src/usb_log.o src/dt295.o src/dt295_crypto.o src/dt295_routes.o src/dt295_stream.o src/ts_output.o src/dt295_capture.o
.PHONY: all clean test
# Legacy standalone C CLI, kept for internal diagnostics and tests only. It is not a release
# program: pixd, pixctl and pix-ts are built by core-integration/CMakeLists.txt.
all: pix-ts-legacy
pix-ts-legacy: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(USB_LIBS) $(OS_LIBS)
%.o: %.c
	$(CC) $(CPPFLAGS) $(USB_CFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c -o $@ $<
tests/test-crypto: tests/test_crypto.o src/crypto.o src/platform.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-transport: tests/test_transport.o src/crypto.o src/transport.o src/platform.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(OS_LIBS)

tests/test-diagnostic: tests/test_diagnostic.c src/diagnostic.c src/crypto.c src/transport.c src/platform.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DDIAG_SEGMENT=32768 -DDIAG_POST=65536 -DDIAG_CC_POST=65536 $(LDFLAGS) -o $@ $^ $(OS_LIBS)

tests/test-usb-log: tests/test_usb_log.o src/usb_log.o src/platform.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(OS_LIBS)

tests/test-usb-selector: tests/test_usb_selector.c src/usb_selector.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-usb-listing: tests/test_usb_listing.c src/usb_listing.c src/usb_selector.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-compat: tests/test_compat.c src/pid_filter.c src/channel.c src/control.c src/protocol.c src/crypto.c src/platform.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)

tests/test-usb-restart: tests/test_usb_restart.c src/usb_log.o src/usb_listing.o src/usb_selector.o src/platform.o
	$(CC) $(CPPFLAGS) $(USB_CFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(USB_LIBS) $(OS_LIBS)

tests/test-dt295-capture: tests/test_dt295_capture.c src/dt295_capture.c src/dt295_routes.c src/transport.c src/crypto.c src/platform.c
	$(CC) $(CPPFLAGS) $(USB_CFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $(filter-out src/dt295_capture.c,$^) $(OS_LIBS)
tests/test-dt295-protocol: tests/test_dt295_protocol.c src/dt295.c src/dt295_crypto.c src/crypto.c src/dt295_routes.c src/transport.c
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-dt295-satellite: tests/test_dt295_satellite.c src/dt295.c src/dt295_crypto.c src/crypto.c src/dt295_routes.c src/transport.c
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-dt295: tests/test_dt295.c src/dt295.c src/dt295_crypto.c src/crypto.c src/dt295_routes.c src/transport.c
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-dt295-assets-c: tests/test_dt295_assets.c src/dt295.c src/dt295_crypto.c src/crypto.c src/dt295_routes.c src/transport.c
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-dt295-routes: tests/test_dt295_routes.c src/dt295_routes.c src/transport.c src/crypto.c src/platform.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-dt295-stream: tests/test_dt295_stream.c src/dt295_stream.c
	$(CC) $(CPPFLAGS) $(USB_CFLAGS) $(CFLAGS) $(WARN) -DDT295_STREAM_TESTING $(LDFLAGS) -o $@ $^ $(USB_LIBS) $(OS_LIBS)
tests/test-ts-output: tests/test_ts_output.c src/ts_output.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DTS_OUTPUT_TESTING $(LDFLAGS) -o $@ $^ $(OS_LIBS)

test: pix-ts-legacy tests/test-dt295-protocol tests/test-dt295-satellite tests/test-dt295-capture tests/test-dt295 tests/test-dt295-routes tests/test-dt295-stream tests/test-ts-output tests/test-crypto tests/test-transport tests/test-diagnostic tests/test-usb-log tests/test-usb-selector tests/test-usb-listing tests/test-compat tests/test-usb-restart tests/test-dt295-assets-c
	./tests/test-dt295-protocol
	./tests/test-dt295-satellite
	./tests/test-dt295-capture
	./tests/test-dt295
	./tests/test-dt295-assets-c
	./tests/test-dt295-routes
	./tests/test-dt295-stream
	./tests/test-ts-output
	./tests/test-crypto
	./tests/test-transport
	./tests/test-diagnostic
	./tests/test-usb-log
	./tests/test-usb-selector
	./tests/test-usb-listing
	./tests/test-usb-restart
	./tests/test-compat
	./tests/test_cli.sh
	PYTHON="$(PYTHON)" ./tests/test_cli_compat.sh
	$(PYTHON) tests/test_dt295_independent.py
	$(PYTHON) tests/test_dt295_packaging.py
	$(PYTHON) tests/test_legacy_source_copy.py
	$(PYTHON) tests/test_dt295_assets.py
clean:
	rm -f pix-ts-legacy pix-ts-legacy.exe tests/*.exe src/*.o src/*.d tests/*.o tests/*.d tests/test-crypto tests/test-transport tests/test-diagnostic tests/test-usb-log tests/test-usb-selector tests/test-usb-listing tests/test-compat tests/test-usb-restart tests/test-dt295 tests/test-dt295-routes tests/test-dt295-stream tests/test-ts-output tests/test-dt295-capture tests/test-dt295-protocol tests/test-dt295-satellite tests/test-dt295-assets-c
-include $(OBJ:.o=.d) tests/test_crypto.d tests/test_transport.d tests/test_usb_log.d
