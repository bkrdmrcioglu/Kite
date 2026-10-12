CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
CPPFLAGS += -Icore/include -Icore/src
LDLIBS += -lpthread

CORE_SRC := $(wildcard core/src/*.c)
CORE_OBJ := $(CORE_SRC:%.c=build/%.o)

all: build/kite

build/%.o: %.c core/include/kite.h core/src/internal.h core/src/net.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/libkite.a: $(CORE_OBJ)
	ar rcs $@ $^

build/kite: build/cli/main.o build/libkite.a
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

build/test_tls: build/core/tests/test_tls.o build/libkite.a
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

test: build/test_tls
	./build/test_tls

clean:
	rm -rf build

.PHONY: all test clean

# Windows (cross-compiled with mingw-w64: brew install mingw-w64)
WIN_CC ?= x86_64-w64-mingw32-gcc
WIN_RC ?= x86_64-w64-mingw32-windres
WIN_CFLAGS := -O2 -Wall -Wextra -std=c11 -Icore/include -Icore/src
WIN_LIBS := -static -lws2_32 -lwininet -lpthread
WIN_GUI_LIBS := -lgdiplus -ldwmapi -lcomctl32

windows: build/win/Kite.exe build/win/kite-cli.exe

build/win/kite_res.o: windows/kite.rc windows/kite.manifest windows/icons/kite.ico windows/icons/kite-off.ico
	@mkdir -p build/win
	cd windows && $(WIN_RC) kite.rc -O coff -o ../$@

build/win/Kite.exe: $(CORE_SRC) windows/kite_win.c build/win/kite_res.o
	$(WIN_CC) $(WIN_CFLAGS) -municode -mwindows $(CORE_SRC) windows/kite_win.c build/win/kite_res.o $(WIN_GUI_LIBS) $(WIN_LIBS) -o $@

build/win/kite-cli.exe: $(CORE_SRC) cli/main.c
	$(WIN_CC) $(WIN_CFLAGS) $(CORE_SRC) cli/main.c $(WIN_LIBS) -o $@

.PHONY: windows
