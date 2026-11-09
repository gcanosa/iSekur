# iSekur -- plain GNU make (use `gmake` on the BSDs).
#   make          build ./isekur
#   make test     build + run tests
#   make debug    ASan+UBSan build
#   make notwitch build without Twitch chat support (same as make TWITCH=0)
#   make install  PREFIX=/usr/local (default) DESTDIR= for staging; use the
#                 same PREFIX for build and install: the themes path is compiled in
#   make clean

CXX      ?= c++
CC       ?= cc

# OpenSSL: pkg-config first, then Homebrew prefixes (macOS ships no headers),
# then plain -lssl -lcrypto.
PKG_OPENSSL := $(shell pkg-config --exists openssl 2>/dev/null && echo yes)
ifeq ($(PKG_OPENSSL),yes)
  OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl)
  OPENSSL_LIBS   := $(shell pkg-config --libs openssl)
else
  BREW_OPENSSL := $(firstword $(wildcard /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 /opt/homebrew/opt/openssl /usr/local/opt/openssl))
  ifneq ($(BREW_OPENSSL),)
    OPENSSL_CFLAGS := -I$(BREW_OPENSSL)/include
    OPENSSL_LIBS   := -L$(BREW_OPENSSL)/lib -lssl -lcrypto
  else
    OPENSSL_LIBS   := -lssl -lcrypto
  endif
endif

# Cygwin's newlib hides strptime() unless asked; it has no PAM, so no /shell login there
ifneq ($(findstring CYGWIN,$(shell uname -s)),)
  OPENSSL_CFLAGS += -D_GNU_SOURCE -DISEKUR_NO_PAM
else
  ifneq ($(shell uname -s),Darwin)
    PAM_LIBS := -lutil  # forkpty
    LDFLAGS  += -rdynamic  # function names in isekur.crash backtraces
    ifneq ($(filter %BSD DragonFly,$(shell uname -s)),)
      PAM_LIBS += -lexecinfo  # backtrace() isn't in the BSDs' libc
    endif
  else
    PAM_LIBS := -framework IOKit -framework CoreFoundation  # /statusbar bat
  endif
  # \043 is "#": make 4.x keeps a backslash in '\#', so cpp got "\#include", saw no include and always said yes
  ifeq ($(shell printf '\043include <security/pam_appl.h>\n' | $(CXX) -E -x c++ - >/dev/null 2>&1 && echo yes),yes)
    PAM_LIBS += -lpam
  else
    $(warning no PAM headers: building without the /shell login. For it, install them and run make again:)
    $(warning   Debian/Ubuntu: sudo apt install libpam0g-dev   Fedora/RHEL: sudo dnf install pam-devel   Arch: sudo pacman -S pam)
    OPENSSL_CFLAGS += -DISEKUR_NO_PAM
  endif
endif

PREFIX   ?= /usr/local
DATADIR  ?= $(PREFIX)/share/isekur
MANDIR   ?= $(PREFIX)/share/man
OPT      ?= -O2
COMMON   := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -fstack-protector-strong \
            -DTB_OPT_ATTR_W=32 -DISEKUR_DATADIR='"$(DATADIR)"' -Ivendor -Ivendor/libvterm/include -Isrc -Ibuild $(OPENSSL_CFLAGS)
CXXFLAGS += -std=c++17 -pthread $(COMMON)
CFLAGS   += -std=c99 $(COMMON) -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-result
LDLIBS   += $(OPENSSL_LIBS) $(PAM_LIBS)

SRC  := src/main.cpp src/core.cpp src/irc.cpp src/net.cpp src/config.cpp src/tui.cpp src/web.cpp src/bnc.cpp src/help.cpp src/chanops.cpp src/session.cpp src/e2e.cpp src/exec.cpp src/dcc.cpp src/sysinfo.cpp src/twitch.cpp src/plugin.cpp src/logs.cpp src/fifo.cpp
VTERM := $(wildcard vendor/libvterm/src/*.c)
LUA  := $(wildcard vendor/lua/*.c)
OBJ  := $(SRC:src/%.cpp=build/%.o) build/termbox2.o $(VTERM:vendor/libvterm/src/%.c=build/vterm_%.o) $(LUA:vendor/lua/%.c=build/lua_%.o)

all: isekur

isekur: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

build/termbox2.o: vendor/termbox2.c vendor/termbox2.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

# libvterm 0.3.3 (MIT): the screen of /shell's terminal
build/vterm_%.o: vendor/libvterm/src/%.c | build
	$(CC) $(CFLAGS) -Ivendor/libvterm/include -w -c -o $@ $<

# Lua 5.4.7 (MIT): plugins. LUA_USE_POSIX: mkstemp etc. in the parts the sandbox leaves out anyway
build/lua_%.o: vendor/lua/%.c | build
	$(CC) $(CFLAGS) -DLUA_USE_POSIX -w -c -o $@ $<

# TWITCH=0 (make notwitch): twitch.cpp with ISEKUR_NO_TWITCH. The choice is kept in
# build/twitch.flag, rewritten only when it changes, so switching rebuilds twitch.o
TWITCH ?= 1
build/twitch.o: CXXFLAGS += $(if $(filter 0,$(TWITCH)),-DISEKUR_NO_TWITCH)
build/twitch.o: build/twitch.flag
build/twitch.flag: FORCE | build
	@echo $(TWITCH) | cmp -s - $@ || echo $(TWITCH) > $@
notwitch:
	$(MAKE) TWITCH=0

# DATADIR (the themes path) is compiled into config.o: kept in build/datadir the same way,
# so `make` then `PREFIX=~/.local make install` rebuilds instead of installing a binary
# that looks in /usr/local
build/config.o: build/datadir
build/datadir: FORCE | build
	@echo '$(DATADIR)' | cmp -s - $@ || echo '$(DATADIR)' > $@

# the config template is compiled in too: /set lists its keys and defaults
build/core.o: build/conf_template.h
build/conf_template.h: iSekur.conf.example | build
	{ printf 'static const char conf_template[] = R"ISEKURCONF('; cat iSekur.conf.example; printf ')ISEKURCONF";\n'; } > $@

# the webchat page is compiled into the binary
build/web.o: build/web_assets.h
build/web_assets.h: web/index.html | build
	{ printf 'static const char index_html[] = R"ISEKURHTML('; cat web/index.html; printf ')ISEKURHTML";\n'; } > $@

# git build id (e.g. v0.2-4-g686b410-dirty) for --version and CTCP VERSION; the
# file is only rewritten when it changes, so it doesn't force rebuilds
build/core.o: build/version.h
build/version.h: FORCE | build
	@printf '#define ISEKUR_BUILD "%s"\n' "$$(git describe --tags --always --dirty 2>/dev/null)" > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
FORCE:

build:
	mkdir -p build

# gcc gives up tracking variables in the test's huge main() and says so; skip that tracking up front (clang has no such flag)
TEST_FLAGS := $(shell $(CXX) -fno-var-tracking-assignments -E -x c++ /dev/null >/dev/null 2>&1 && echo -fno-var-tracking-assignments)

test: build/test
	./build/test

build/test: tests/test.cpp build/config.o build/irc.o build/core.o build/help.o build/chanops.o build/session.o build/net.o build/web.o build/bnc.o build/e2e.o build/exec.o build/fifo.o build/dcc.o build/tui.o build/sysinfo.o build/twitch.o build/plugin.o build/logs.o build/termbox2.o $(VTERM:vendor/libvterm/src/%.c=build/vterm_%.o) $(LUA:vendor/lua/%.c=build/lua_%.o)
	$(CXX) $(CXXFLAGS) $(TEST_FLAGS) -Ibuild -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug:
	$(MAKE) clean
	$(MAKE) OPT="-O0 -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

install: isekur
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(DATADIR)/themes $(DESTDIR)$(MANDIR)/man1
	install -m 755 isekur $(DESTDIR)$(PREFIX)/bin/isekur
	install -m 644 isekur.1 $(DESTDIR)$(MANDIR)/man1/isekur.1
	install -m 644 themes/*.theme $(DESTDIR)$(DATADIR)/themes/
	install -m 644 iSekur.conf.example README.md LICENSE $(DESTDIR)$(DATADIR)/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/isekur $(DESTDIR)$(MANDIR)/man1/isekur.1
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	rm -rf build isekur

-include $(wildcard build/*.d)
.PHONY: all test debug notwitch install uninstall clean FORCE
