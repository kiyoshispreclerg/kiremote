# Kiremote build configuration. Override any variable on the command line,
# e.g. `make CC=clang PREFIX=/usr`.

VERSION = 0.0.1

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
LOCALEDIR ?= $(DATADIR)/locale
DESTDIR ?=

PKG_CONFIG ?= pkg-config

# C11 without GNU extensions; XSI (implies POSIX.1-2008).
STDFLAGS  = -std=c11 -D_XOPEN_SOURCE=700
WARNFLAGS = -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla
# Vendored third-party code is built with the base warnings only.
VENDOR_WARNFLAGS = -Wall -Wextra -Werror

OPTFLAGS ?= -O2 -g

# Daemon libraries. Library headers go in as -isystem so -Wpedantic/-Werror
# only judge our code.
PKGS       = openssl dbus-1 x11 xfixes xtst xext
PKG_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(PKGS) | sed 's/-I/-isystem /g')
PKG_LIBS   = $(shell $(PKG_CONFIG) --libs $(PKGS))

CPPFLAGS += -Isrc -Ivendor/cjson -DKR_VERSION='"$(VERSION)"' -DLOCALEDIR='"$(LOCALEDIR)"'
LDLIBS   += $(PKG_LIBS) -lm

# `make asan` sets SANITIZE=1.
SANITIZE ?= 0
ifeq ($(SANITIZE),1)
OPTFLAGS = -O1 -g -fno-omit-frame-pointer
SANFLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all
endif
# `make fuzz` sets SANITIZE=fuzz (clang): coverage for libFuzzer everywhere,
# the fuzzer's main() only in the fuzz targets.
ifeq ($(SANITIZE),fuzz)
OPTFLAGS = -O1 -g -fno-omit-frame-pointer
SANFLAGS = -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
endif
