BIN      := easytether-bridge
PREFIX   ?= /usr/local
SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:.c=.o)

CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
            -Wno-unused-parameter -D_DARWIN_C_SOURCE
LDFLAGS  += -framework SystemConfiguration -framework CoreFoundation

# Build for whichever Mac this is; add both for a universal binary.
ARCHS    ?= $(shell uname -m)
CFLAGS   += $(foreach a,$(ARCHS),-arch $(a))
LDFLAGS  += $(foreach a,$(ARCHS),-arch $(a))

.PHONY: all clean install uninstall universal

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

universal:
	$(MAKE) clean
	$(MAKE) ARCHS="arm64 x86_64"

clean:
	rm -rf $(OBJ) $(BIN) test/unit test/protocol test/mockphone *.dSYM test/*.dSYM

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

src/main.o:   src/util.h src/proto.h src/bridge.h src/adb.h
src/bridge.o: src/bridge.h src/util.h src/proto.h src/utun.h src/netcfg.h src/adb.h
src/proto.o:  src/proto.h src/util.h
src/utun.o:   src/utun.h src/util.h
src/netcfg.o: src/netcfg.h src/proto.h src/util.h
src/adb.o:    src/adb.h src/util.h
src/util.o:   src/util.h

# --- Windows port, work in progress -------------------------------------
#
# The protocol core is platform-independent and must stay that way: it is the
# part a port reuses verbatim, so a stray POSIX include here would be the most
# expensive kind of regression.  This target proves it still cross-compiles,
# for 64- and 32-bit Windows, under the same warnings as the native build.
#
# Needs a cross toolchain:  brew install mingw-w64
# Everything else (utun.c, netcfg.c, the event loop) is not ported yet --
# see docs/PORTING.md.

WIN_CC64 ?= x86_64-w64-mingw32-gcc
WIN_CC32 ?= i686-w64-mingw32-gcc
WIN_PORTABLE := src/proto.c

.PHONY: windows-check
windows-check:
	@command -v $(WIN_CC64) >/dev/null 2>&1 || \
		{ echo "no $(WIN_CC64); brew install mingw-w64" >&2; exit 1; }
	@for cc in $(WIN_CC64) $(WIN_CC32); do \
		echo "  $$cc"; \
		$$cc -std=c11 -Wall -Wextra -Wshadow -Wpointer-arith \
		     -Wstrict-prototypes -Werror -c $(WIN_PORTABLE) -o /dev/null || exit 1; \
	done
	@echo "the protocol core still builds for Windows"

# --- test harness -------------------------------------------------------
TEST_SRC := test/testutil.c src/proto.c src/util.c

test/mockphone: test/mockphone.c $(TEST_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

.PHONY: mock
mock: test/mockphone

test/unit: test/unit.c $(TEST_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

.PHONY: check
check: test/unit
	@./test/unit

test/protocol: test/protocol.c $(TEST_SRC) src/adb.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

.PHONY: protocol-test
protocol-test: test/protocol test/mockphone
	@bash test/run-protocol-test.sh
