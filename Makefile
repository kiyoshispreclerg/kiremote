include config.mk

BUILD ?= build

# Each program's main file; everything else goes into a static lib that the
# programs and the unit tests link against.
DAEMON_MAIN = src/kiremoted.c
CLI_MAIN = src/kiremote-cli.c
LIB_SRC = src/log.c \
      src/i18n.c \
      src/util.c \
      src/json.c \
      src/ini.c \
      src/config.c \
      src/packet.c \
      src/cert.c \
      src/conn.c \
      src/trust.c \
      src/pair.c \
      src/link.c \
      src/net.c \
      src/transfer.c \
      src/control.c \
      src/schema.c \
      src/daemon.c \
      src/plugins.c \
      src/plugin_ping.c \
      src/plugin_share.c \
      src/proc.c \
      src/bus.c \
      src/x11.c \
      src/notify.c \
      src/mpris.c \
      src/plugin_battery.c \
      src/plugin_connectivity.c \
      src/plugin_find.c \
      src/plugin_telephony.c \
      src/plugin_pausemusic.c \
      src/plugin_mpris.c \
      src/plugin_volume.c \
      src/plugin_clipboard.c \
      src/plugin_notifications.c \
      src/plugin_runcommand.c \
      src/plugin_mousepad.c \
      src/plugin_presenter.c \
      src/plugin_screensaver.c \
      src/plugin_lock.c \
      src/plugin_sftp.c \
      ui/krmodel.c \
      ui/krclient.c
VENDOR_SRC = vendor/cjson/cJSON.c
LIBKR = $(BUILD)/libkr.a

UNIT_TESTS = $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(wildcard tests/unit/test_*.c))
# Fuzz targets (tests/fuzz/fuzz_<name>.c, corpus in tests/fuzz/corpus/<name>/).
FUZZ_TARGETS = $(patsubst tests/fuzz/fuzz_%.c,%,$(wildcard tests/fuzz/fuzz_*.c))
FUZZ_RUNNERS = $(patsubst %,$(BUILD)/tests/fuzz-run-%,$(FUZZ_TARGETS))

ALL_CFLAGS  = $(STDFLAGS) $(OPTFLAGS) $(SANFLAGS) $(PKG_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(SANFLAGS) $(LDFLAGS)

.PHONY: all test test-net run asan asan-run fuzz fuzz-targets install uninstall lint clean locale pot update-po

# Translations (po/LINGUAS lists the languages), built into $(BUILD)/locale,
# which the programs find next to their own executable.
LINGUAS = $(shell sed 's/\#.*//' po/LINGUAS)
HAVE_MSGFMT := $(shell command -v msgfmt >/dev/null 2>&1 && echo 1)
MO_FILES = $(if $(HAVE_MSGFMT),$(foreach l,$(LINGUAS),$(BUILD)/locale/$(l)/LC_MESSAGES/kiremote.mo))

all: $(BUILD)/kiremoted $(BUILD)/kiremote-cli $(MO_FILES)
locale: $(MO_FILES)

$(BUILD)/locale/%/LC_MESSAGES/kiremote.mo: po/%.po
	@mkdir -p $(@D)
	msgfmt -c -o $@ $<

# Rebuilds the template from the sources and merges it into every .po. No
# --package-version: config.mk's VERSION is the only place the version
# lives, so bumping it doesn't touch the .po files too.
pot update-po:
	xgettext --from-code=UTF-8 -L C --keyword=_ --keyword=N_ --keyword=ngettext:1,2 \
		--add-comments=TRANSLATORS -f po/POTFILES.in -o po/kiremote.pot \
		--package-name=kiremote \
		--copyright-holder="Kiremote contributors" --msgid-bugs-address=""
	for l in $(LINGUAS); do msgmerge -q -U --backup=none po/$$l.po po/kiremote.pot; done

$(BUILD)/kiremoted: $(BUILD)/$(DAEMON_MAIN:.c=.o) $(LIBKR)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/kiremote-cli: $(BUILD)/$(CLI_MAIN:.c=.o) $(LIBKR)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

$(LIBKR): $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRC) $(VENDOR_SRC))
	$(AR) rcs $@ $^

$(BUILD)/vendor/%.o: vendor/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(VENDOR_WARNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP -c -o $@ $<

# ---- tests ----------------------------------------------------------------

TEST_CPPFLAGS = -DKR_FIXTURES_DIR='"$(CURDIR)/tests/fixtures"'

$(BUILD)/tests/%: tests/unit/%.c tests/harness.h $(LIBKR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP \
		$(ALL_LDFLAGS) -o $@ $< $(LIBKR) $(LDLIBS)

# A fuzz target without libFuzzer: runs over the files it is given.
$(BUILD)/tests/fuzz-run-%: tests/fuzz/fuzz_%.c tests/fuzz/driver.c $(LIBKR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) $(ALL_LDFLAGS) -o $@ \
		tests/fuzz/fuzz_$*.c tests/fuzz/driver.c $(LIBKR) $(LDLIBS)

# The scriptable phone that tests/net drives the daemon with.
$(BUILD)/tests/fakephone: tests/tools/fakephone.c $(LIBKR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) $(ALL_LDFLAGS) -o $@ $< $(LIBKR) $(LDLIBS)

$(BUILD)/tests/xtool: tests/tools/xtool.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) $(ALL_LDFLAGS) -o $@ $< $(LDLIBS)

test: all $(UNIT_TESTS) $(FUZZ_RUNNERS) $(BUILD)/tests/fakephone $(BUILD)/tests/xtool
	@fail=0; for t in $(UNIT_TESTS); do \
		$$t || fail=1; \
	done; \
	for f in $(FUZZ_TARGETS); do \
		$(BUILD)/tests/fuzz-run-$$f tests/fuzz/corpus/$$f || fail=1; \
	done; \
	tests/net/pair.sh $(BUILD) || fail=1; \
	tests/net/plugins.sh $(BUILD) || fail=1; \
	tests/net/desktop.sh $(BUILD) || fail=1; \
	if [ $$fail -ne 0 ]; then echo "*** tests FAILED"; exit 1; fi; \
	echo "*** all tests passed"

# Only the two daemons in network namespaces (tests/net/pair.sh).
test-net: all $(BUILD)/tests/fakephone $(BUILD)/tests/xtool
	tests/net/pair.sh $(BUILD)
	tests/net/plugins.sh $(BUILD)
	tests/net/desktop.sh $(BUILD)

run: $(BUILD)/kiremoted
	$(BUILD)/kiremoted $(ARGS)

asan:
	LSAN_OPTIONS=suppressions=$(CURDIR)/tools/lsan.supp \
		$(MAKE) BUILD=$(BUILD)/asan SANITIZE=1 test

# libFuzzer + ASan/UBSan with clang: `make fuzz`, then e.g.
#   build/fuzz/fuzz_packet -max_total_time=60 tests/fuzz/corpus/packet
fuzz:
	$(MAKE) CC=clang BUILD=$(BUILD)/fuzz SANITIZE=fuzz fuzz-targets

fuzz-targets: $(patsubst %,$(BUILD)/fuzz_%,$(FUZZ_TARGETS))

# libFuzzer is C++: clang may pick a GCC whose libstdc++.so isn't
# installed, so point it at the one the default gcc uses.
FUZZ_LDFLAGS ?= -L$(dir $(shell gcc -print-file-name=libstdc++.so 2>/dev/null))

$(BUILD)/fuzz_%: tests/fuzz/fuzz_%.c $(LIBKR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -fsanitize=fuzzer $(FUZZ_LDFLAGS) -o $@ $< \
		$(LIBKR) $(LDLIBS)

asan-run: asan
	LSAN_OPTIONS=suppressions=$(CURDIR)/tools/lsan.supp $(BUILD)/asan/kiremoted $(ARGS)

install: $(BUILD)/kiremoted $(BUILD)/kiremote-cli $(MO_FILES)
	install -Dm755 $(BUILD)/kiremoted $(DESTDIR)$(BINDIR)/kiremoted
	install -Dm644 data/kiremote.ini $(DESTDIR)$(DATADIR)/doc/kiremote/kiremote.ini
	install -Dm644 data/kiremoted.desktop $(DESTDIR)/etc/xdg/autostart/kiremoted.desktop
	$(foreach l,$(if $(MO_FILES),$(LINGUAS)),install -Dm644 $(BUILD)/locale/$(l)/LC_MESSAGES/kiremote.mo \
		$(DESTDIR)$(LOCALEDIR)/$(l)/LC_MESSAGES/kiremote.mo;)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiremoted $(DESTDIR)$(BINDIR)/kiremote-cli
	rm -f $(DESTDIR)$(DATADIR)/doc/kiremote/kiremote.ini
	rm -f $(DESTDIR)/etc/xdg/autostart/kiremoted.desktop
	for l in $(LINGUAS); do rm -f $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/kiremote.mo; done

lint:
	@if grep -nE '\b(strcpy|strcat|sprintf|vsprintf|gets)[[:space:]]*\(' src/*.c src/*.h ui/*.c ui/*.h tests/unit/*.c tests/fuzz/*.c; then \
		echo "*** banned function used"; exit 1; fi
	@command -v cppcheck >/dev/null || { echo "*** cppcheck not installed"; exit 1; }
	cppcheck --std=c11 --enable=warning,performance,portability \
		--error-exitcode=1 --inline-suppr --quiet -Isrc -Ivendor/cjson \
		-D_XOPEN_SOURCE=700 -DKR_VERSION='"lint"' -DKR_FIXTURES_DIR='"."' src ui tests/unit tests/fuzz

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
