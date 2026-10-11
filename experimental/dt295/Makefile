CC ?= cc
CFLAGS ?= -O2 -g
ROOT = ../..
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
LIB = libdt295.dylib
SHARED = -dynamiclib
else
LIB = libdt295.so
SHARED = -shared
endif
.PHONY: all test clean
all: $(LIB)
$(LIB): crypto.c $(ROOT)/src/crypto.c $(ROOT)/src/platform.c $(ROOT)/include/crypto.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -std=c11 -Wall -Wextra -Wpedantic -fPIC $(SHARED) -I$(ROOT)/include -o $@ crypto.c $(ROOT)/src/crypto.c $(ROOT)/src/platform.c -pthread
test: all
	python3 -m unittest discover -v
clean:
	rm -f libdt295.dylib libdt295.so
