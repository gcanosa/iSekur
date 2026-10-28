# iSekur -- plain GNU make (use `gmake` on the BSDs).
#   make          build ./isekur
#   make test     build + run tests
#   make debug    ASan+UBSan build
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

# Cygwin's newlib hides strptime() unless asked
ifneq ($(findstring CYGWIN,$(shell uname -s)),)
  OPENSSL_CFLAGS += -D_GNU_SOURCE
endif

PREFIX   ?= /usr/local
DATADIR  ?= $(PREFIX)/share/isekur
MANDIR   ?= $(PREFIX)/share/man
OPT      ?= -O2
COMMON   := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -fstack-protector-strong \
            -DTB_OPT_ATTR_W=32 -DISEKUR_DATADIR='"$(DATADIR)"' -Ivendor -Isrc -Ibuild $(OPENSSL_CFLAGS)
CXXFLAGS += -std=c++17 -pthread $(COMMON)
CFLAGS   += -std=c99 $(COMMON) -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-result
LDLIBS   += $(OPENSSL_LIBS)

SRC  := src/main.cpp src/core.cpp src/irc.cpp src/net.cpp src/config.cpp src/tui.cpp src/web.cpp src/help.cpp src/chanops.cpp src/session.cpp src/e2e.cpp
OBJ  := $(SRC:src/%.cpp=build/%.o) build/termbox2.o

all: isekur

isekur: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

build/termbox2.o: vendor/termbox2.c vendor/termbox2.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

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

test: build/test
	./build/test

build/test: tests/test.cpp build/config.o build/irc.o build/core.o build/help.o build/chanops.o build/session.o build/net.o build/web.o build/e2e.o build/termbox2.o
	$(CXX) $(CXXFLAGS) -Ibuild -o $@ $^ $(LDFLAGS) $(LDLIBS)

debug:
	$(MAKE) clean
	$(MAKE) OPT="-O0 -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

install: isekur
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(DATADIR)/themes $(DESTDIR)$(MANDIR)/man1
	install -m 755 isekur $(DESTDIR)$(PREFIX)/bin/isekur
	install -m 644 isekur.1 $(DESTDIR)$(MANDIR)/man1/isekur.1
	install -m 644 themes/*.theme $(DESTDIR)$(DATADIR)/themes/
	install -m 644 iSekur.conf.example README.md $(DESTDIR)$(DATADIR)/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/isekur $(DESTDIR)$(MANDIR)/man1/isekur.1
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	rm -rf build isekur

-include $(wildcard build/*.d)
.PHONY: all test debug install uninstall clean FORCE
