CC ?= cc
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -DPIX_CAPTURE_BRIDGE_EXTERNAL
OS_LIBS ?= -pthread
WARN = -std=c11 -Wall -Wextra -Wpedantic
USB_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libusb-1.0)
USB_LIBS ?= $(shell $(PKG_CONFIG) --libs libusb-1.0)
CORE = src/pid_filter.o src/control.o src/channel.o src/crypto.o src/platform.o src/protocol.o src/transport.o
OBJ = src/usb_selector.o src/usb_listing.o $(CORE) src/diagnostic.o src/main.o src/dt181_capture_bridge.o src/dt181_session.o src/usb_libusb.o src/usb_log.o src/dt295.o src/dt295_crypto.o src/dt295_routes.o src/dt295_stream.o src/ts_output.o src/dt295_capture.o
.PHONY: all clean test test-pix-session test-ts-output-fd
all: pix-ts
pix-ts: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(USB_LIBS) $(OS_LIBS)
%.o: %.c
	$(CC) $(CPPFLAGS) $(USB_CFLAGS) $(CFLAGS) $(WARN) -MMD -MP -c -o $@ $<
tests/test-crypto: tests/test_crypto.o src/crypto.o src/platform.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-transport: tests/test_transport.o src/crypto.o src/transport.o src/platform.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(OS_LIBS)

tests/test-dt181-capture-bridge: tests/test_dt181_capture_bridge.c src/dt181_capture_bridge.c src/diagnostic.c src/transport.c src/crypto.c src/platform.c
	$(CC) $(CPPFLAGS) -Isrc $(USB_CFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)

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
tests/test-dt295: tests/test_dt295.c src/dt295.c src/dt295_crypto.c src/crypto.c src/dt295_routes.c src/transport.c
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-dt295-routes: tests/test_dt295_routes.c src/dt295_routes.c src/transport.c src/crypto.c src/platform.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ $^ $(OS_LIBS)
tests/test-dt295-stream: tests/test_dt295_stream.c src/dt295_stream.c
	$(CC) $(CPPFLAGS) $(USB_CFLAGS) $(CFLAGS) $(WARN) -DDT295_STREAM_TESTING $(LDFLAGS) -o $@ $^ $(USB_LIBS) $(OS_LIBS)
tests/test-ts-output: tests/test_ts_output.c src/ts_output.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DTS_OUTPUT_TESTING $(LDFLAGS) -o $@ $^ $(OS_LIBS)

test: pix-ts tests/test-dt181-capture-bridge tests/test-dt295-protocol tests/test-dt295-capture tests/test-dt295 tests/test-dt295-routes tests/test-dt295-stream tests/test-ts-output test-ts-output-fd tests/test-crypto tests/test-transport tests/test-diagnostic tests/test-usb-log tests/test-usb-selector tests/test-usb-listing tests/test-compat tests/test-usb-restart test-pix-session
	$(PYTHON) tests/run_dt181_capture_bridge.py --timeout 10 -- ./tests/test-dt181-capture-bridge
	$(PYTHON) tests/run_dt181_capture_bridge.py --timeout 10 --expect-exit 23 -- ./tests/test-dt181-capture-bridge --no-sink
	$(PYTHON) tests/run_dt181_capture_bridge.py --timeout 10 --expect-exit 25 -- ./tests/test-dt181-capture-bridge --queue-full
	./tests/test-dt295-protocol
	./tests/test-dt295-capture
	./tests/test-dt295
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
	$(PYTHON) tests/test_dt295_assets.py
ifeq ($(shell uname -s),Linux)
test-pix-session:
	$(PYTHON) tests/test_pix_session.py

else
# This harness wraps POSIX pthread symbols with GNU ld --wrap and uses /dev/null.
# Keep it Linux-only; the product has separate Windows/Darwin build paths.
test-pix-session:
	@echo 'SKIP test-pix-session: fake actual-main harness is supported on Linux only (GNU ld --wrap, POSIX pthreads, /dev/null).'
endif
ifeq ($(shell uname -s),Linux)
test-ts-output-fd: tests/test_ts_output_fd.c src/ts_output.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DTS_OUTPUT_TESTING_FD_CLEANUP $(LDFLAGS) -o tests/test-ts-output-fd $^ $(OS_LIBS)
	$(PYTHON) tests/run_ts_output_fd.py --timeout 20 -- ./tests/test-ts-output-fd
else
test-ts-output-fd:
	@echo 'SKIP test-ts-output-fd: Linux FD cleanup tests are supported on Linux only.'
endif
clean:
	rm -f pix-ts pix-ts.exe tests/*.exe src/*.o src/*.d tests/*.o tests/*.d tests/test-dt181-capture-bridge tests/test-crypto tests/test-transport tests/test-diagnostic tests/test-usb-log tests/test-usb-selector tests/test-usb-listing tests/test-compat tests/test-usb-restart tests/test-dt295 tests/test-dt295-routes tests/test-dt295-stream tests/test-ts-output tests/test-ts-output-fd tests/test-dt295-capture tests/test-dt295-protocol
-include $(OBJ:.o=.d) tests/test_crypto.d tests/test_transport.d tests/test_usb_log.d
