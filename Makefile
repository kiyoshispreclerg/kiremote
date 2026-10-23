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
      ui/krclient.c \
      mirror/stream.c \
      mirror/adb.c \
      mirror/server.c \
      mirror/ctlmsg.c \
      mirror/keys.c
VENDOR_SRC = vendor/cjson/cJSON.c
LIBKR = $(BUILD)/libkr.a

UNIT_TESTS = $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(wildcard tests/unit/test_*.c))
# Fuzz targets (tests/fuzz/fuzz_<name>.c, corpus in tests/fuzz/corpus/<name>/).
FUZZ_TARGETS = $(patsubst tests/fuzz/fuzz_%.c,%,$(wildcard tests/fuzz/fuzz_*.c))
FUZZ_RUNNERS = $(patsubst %,$(BUILD)/tests/fuzz-run-%,$(FUZZ_TARGETS))

ALL_CFLAGS  = $(STDFLAGS) $(OPTFLAGS) $(SANFLAGS) $(PKG_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(SANFLAGS) $(LDFLAGS)

.PHONY: install-daemon install-gtk install-kidesktop test-ui all test test-net run asan asan-run fuzz fuzz-targets install uninstall lint clean locale pot update-po

# The GTK2 interface is built when GTK2 is there: the daemon and the CLI don't
# need it (and a machine with only the daemon shouldn't).
HAVE_GTK := $(shell $(PKG_CONFIG) --exists gtk+-2.0 && echo 1)
ifeq ($(HAVE_GTK),1)
GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags gtk+-2.0 | sed 's/-I/-isystem /g')
GTK_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-2.0)
KRUI_SRC = $(filter-out ui/gtk/main.c,$(wildcard ui/gtk/*.c))
LIBKRUI = $(BUILD)/libkrui.a
GTK_TARGETS = $(BUILD)/kiremote
GTK_TESTS = $(BUILD)/tests/test_ui
endif

# The KiDesktop modules: a kiconf tab and an xisserve page (GTK2) and an
# xispanel widget (cairo only). The page and the widget need the headers
# KiDesktop installs (under $(PREFIX)/include: /usr/local from source, /usr from
# the kidesktop-core package); without them they are skipped (nothing else
# notices).
# They are loaded into other programs, so everything they embed is PIC.
KD_INCDIR ?= $(PREFIX)/include
LIBDIR ?= $(PREFIX)/lib
KD_PAGE_H = $(wildcard $(KD_INCDIR)/xisserve/xisserve_page.h)
KD_WIDGET_H = $(wildcard $(KD_INCDIR)/xispanel/xispanel_widget.h)
HAVE_CAIRO := $(shell $(PKG_CONFIG) --exists cairo && echo 1)
KD_OUT = $(BUILD)/kidesktop
PIC_SRC = src/util.c src/log.c src/i18n.c src/control.c ui/krmodel.c ui/krclient.c
KD_TARGETS =
KD_TESTS =
ifeq ($(HAVE_GTK),1)
KD_TARGETS += $(KD_OUT)/tabs/kiremote-tab.so $(KD_OUT)/tabs/kiremote.desktop
ifneq ($(KD_PAGE_H),)
KD_TARGETS += $(KD_OUT)/pages/kiremote.so
endif
endif
ifneq ($(and $(KD_WIDGET_H),$(filter 1,$(HAVE_CAIRO))),)
KD_TARGETS += $(KD_OUT)/widgets/kiremote.so $(KD_OUT)/widgets/kiremote.widget
KD_TESTS += $(BUILD)/tests/test_widget
endif

# kiremote-mirror (the phone's screen in a window): needs libavcodec,
# libswscale and Xext; without them it is skipped. Its protocol parts
# (stream, adb, server) are in libkr, so the tests do not need libav.
MIRROR_PKGS = libavcodec libavutil libswscale x11 xext
HAVE_MIRROR := $(shell $(PKG_CONFIG) --exists $(MIRROR_PKGS) && echo 1)
ifeq ($(HAVE_MIRROR),1)
MIRROR_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(MIRROR_PKGS) | sed 's/-I/-isystem /g')
MIRROR_LIBS = $(shell $(PKG_CONFIG) --libs $(MIRROR_PKGS))
MIRROR_SRC = mirror/main.c mirror/decode.c mirror/screen.c
MIRROR_TARGETS = $(BUILD)/kiremote-mirror
endif

# Translations (po/LINGUAS lists the languages), built into $(BUILD)/locale,
# which the programs find next to their own executable.
LINGUAS = $(shell sed 's/\#.*//' po/LINGUAS)
HAVE_MSGFMT := $(shell command -v msgfmt >/dev/null 2>&1 && echo 1)
MO_FILES = $(if $(HAVE_MSGFMT),$(foreach l,$(LINGUAS),$(BUILD)/locale/$(l)/LC_MESSAGES/kiremote.mo))

all: $(BUILD)/kiremoted $(BUILD)/kiremote-cli $(MO_FILES) $(GTK_TARGETS) $(KD_TARGETS) $(MIRROR_TARGETS)
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

# What the modules embed from the daemon's side (PIC, and its own cJSON).
$(BUILD)/pic/vendor/%.o: vendor/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(VENDOR_WARNFLAGS) -fPIC -MMD -MP -c -o $@ $<

$(BUILD)/pic/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -fPIC -MMD -MP -c -o $@ $<

LIBKRPIC = $(BUILD)/libkrpic.a
$(LIBKRPIC): $(patsubst %.c,$(BUILD)/pic/%.o,$(PIC_SRC) $(VENDOR_SRC))
	$(AR) rcs $@ $^

# The widget module under a host that gives it the xpw_*() it expects.
$(BUILD)/tests/test_widget: tests/ui/test_widget.c tests/harness.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -I$(KD_INCDIR)/xispanel $(ALL_CFLAGS) $(shell $(PKG_CONFIG) --cflags cairo | sed 's/-I/-isystem /g') $(WARNFLAGS) -MMD -MP \
		$(ALL_LDFLAGS) -rdynamic -o $@ $< $(shell $(PKG_CONFIG) --libs cairo) -ldl

# The modules. -Bsymbolic: what they embed (cJSON, kr_*) binds to their own
# copy, whatever the program that loads them exports.
KD_SO_LDFLAGS = -shared -Wl,-Bsymbolic $(ALL_LDFLAGS)

$(KD_OUT)/%.o: kidesktop/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -I$(KD_INCDIR)/xisserve -I$(KD_INCDIR)/xispanel $(ALL_CFLAGS) $(GTK_CFLAGS) $(shell $(PKG_CONFIG) --cflags cairo 2>/dev/null | sed 's/-I/-isystem /g') \
		-fPIC $(WARNFLAGS) -MMD -MP -c -o $@ $<

$(KD_OUT)/tabs/kiremote-tab.so: $(KD_OUT)/tab.o $(LIBKRUI) $(LIBKRPIC)
	@mkdir -p $(@D)
	$(CC) $(KD_SO_LDFLAGS) -o $@ $^ -lm

$(KD_OUT)/pages/kiremote.so: $(KD_OUT)/page.o $(LIBKRUI) $(LIBKRPIC)
	@mkdir -p $(@D)
	$(CC) $(KD_SO_LDFLAGS) -o $@ $^ -lm

$(KD_OUT)/widgets/kiremote.so: $(KD_OUT)/widget.o $(LIBKRPIC)
	@mkdir -p $(@D)
	$(CC) $(KD_SO_LDFLAGS) -o $@ $^ -lm

$(KD_OUT)/widgets/kiremote.widget: kidesktop/kiremote.widget
	@mkdir -p $(@D)
	cp $< $@

# In the build tree the tab's module sits beside its .desktop; installed, it
# is found by its absolute path.
$(KD_OUT)/tabs/kiremote.desktop: kidesktop/kiremote.desktop.in
	@mkdir -p $(@D)
	sed 's|@MODULE@|kiremote-tab.so|' $< > $@

# libkrui: the interface as widgets (PIC: the KiDesktop modules embed it).
$(BUILD)/ui/gtk/%.o: ui/gtk/%.c $(wildcard ui/gtk/*.h) ui/krmodel.h ui/krclient.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(GTK_CFLAGS) -fPIC $(WARNFLAGS) -MMD -MP -c -o $@ $<

$(LIBKRUI): $(patsubst %.c,$(BUILD)/%.o,$(KRUI_SRC))
	$(AR) rcs $@ $^

# The interface tests need GTK and a private X server (tests/ui/run.sh).
$(BUILD)/tests/test_ui: tests/ui/test_ui.c tests/harness.h $(LIBKRUI) $(LIBKR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(GTK_CFLAGS) $(WARNFLAGS) -MMD -MP $(ALL_LDFLAGS) -o $@ $< $(LIBKRUI) $(LIBKR) $(GTK_LIBS) $(LDLIBS)

$(BUILD)/kiremote: $(BUILD)/ui/gtk/main.o $(LIBKRUI) $(LIBKR)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(GTK_LIBS) $(LDLIBS)

$(BUILD)/mirror/%.o: mirror/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(MIRROR_CFLAGS) $(WARNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/kiremote-mirror: $(patsubst %.c,$(BUILD)/%.o,$(MIRROR_SRC)) $(LIBKR)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(MIRROR_LIBS) $(LDLIBS)

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

test: all $(UNIT_TESTS) $(GTK_TESTS) $(KD_TESTS) $(FUZZ_RUNNERS) $(BUILD)/tests/fakephone $(BUILD)/tests/xtool
	@fail=0; for t in $(UNIT_TESTS); do \
		$$t || fail=1; \
	done; \
	for f in $(FUZZ_TARGETS); do \
		$(BUILD)/tests/fuzz-run-$$f tests/fuzz/corpus/$$f || fail=1; \
	done; \
	tests/net/pair.sh $(BUILD) || fail=1; \
	tests/net/plugins.sh $(BUILD) || fail=1; \
	tests/net/desktop.sh $(BUILD) || fail=1; \
	$(if $(GTK_TESTS),tests/ui/run.sh $(BUILD) || fail=1;) \
	$(if $(KD_TESTS),$(BUILD)/tests/test_widget $(KD_OUT)/widgets/kiremote.so || fail=1;) \
	$(if $(and $(KD_TESTS),$(filter 0,$(SANITIZE))),tests/ui/kidesktop.sh $(BUILD) || fail=1;) \
	if [ $$fail -ne 0 ]; then echo "*** tests FAILED"; exit 1; fi; \
	echo "*** all tests passed"

# Only the interface tests (GTK2 and Xvfb). After GTK_TESTS is set: a
# rule's prerequisites are expanded when make reads it.
test-ui: all $(GTK_TESTS)
	tests/ui/run.sh $(BUILD)

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

# One target per package (debian/rules uses them): the daemon and CLI with
# the translations, the GTK window, the KiDesktop modules. `make install`
# does what can be built here.
install: install-daemon $(if $(GTK_TARGETS),install-gtk) $(if $(KD_TARGETS),install-kidesktop)

install-daemon: $(BUILD)/kiremoted $(BUILD)/kiremote-cli $(MO_FILES)
	install -Dm755 $(BUILD)/kiremoted $(DESTDIR)$(BINDIR)/kiremoted
	install -Dm755 $(BUILD)/kiremote-cli $(DESTDIR)$(BINDIR)/kiremote-cli
	install -Dm644 data/kiremote.ini $(DESTDIR)$(DATADIR)/doc/kiremote/kiremote.ini
	install -Dm644 data/kiremoted.desktop $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/kiremoted.desktop
	$(foreach l,$(if $(MO_FILES),$(LINGUAS)),install -Dm644 $(BUILD)/locale/$(l)/LC_MESSAGES/kiremote.mo \
		$(DESTDIR)$(LOCALEDIR)/$(l)/LC_MESSAGES/kiremote.mo;)

install-gtk: $(BUILD)/kiremote
	install -Dm755 $(BUILD)/kiremote $(DESTDIR)$(BINDIR)/kiremote
	install -Dm644 data/kiremote.desktop $(DESTDIR)$(DATADIR)/applications/kiremote.desktop
	install -Dm644 data/kiremote-autostart.desktop $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/kiremote.desktop

# The tab's module is found by its absolute path (the .desktop is generated
# for LIBDIR); xisserve and xispanel look in their own directories.
install-kidesktop: $(KD_TARGETS)
	install -Dm755 $(KD_OUT)/tabs/kiremote-tab.so $(DESTDIR)$(LIBDIR)/kiremote/kiremote-tab.so
	install -d $(DESTDIR)$(DATADIR)/kiconf/tabs
	sed 's|@MODULE@|$(LIBDIR)/kiremote/kiremote-tab.so|' kidesktop/kiremote.desktop.in > $(DESTDIR)$(DATADIR)/kiconf/tabs/kiremote.desktop
	install -Dm755 $(KD_OUT)/pages/kiremote.so $(DESTDIR)$(LIBDIR)/xisserve/pages/kiremote.so
	install -Dm755 $(KD_OUT)/widgets/kiremote.so $(DESTDIR)$(LIBDIR)/xispanel/widgets/kiremote.so
	install -Dm644 kidesktop/kiremote.widget $(DESTDIR)$(LIBDIR)/xispanel/widgets/kiremote.widget

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiremoted $(DESTDIR)$(BINDIR)/kiremote-cli $(DESTDIR)$(BINDIR)/kiremote
	rm -f $(DESTDIR)$(DATADIR)/applications/kiremote.desktop $(DESTDIR)$(DATADIR)/kiconf/tabs/kiremote.desktop
	rm -f $(DESTDIR)$(LIBDIR)/kiremote/kiremote-tab.so $(DESTDIR)$(LIBDIR)/xisserve/pages/kiremote.so
	rm -f $(DESTDIR)$(LIBDIR)/xispanel/widgets/kiremote.so $(DESTDIR)$(LIBDIR)/xispanel/widgets/kiremote.widget
	rm -f $(DESTDIR)$(DATADIR)/doc/kiremote/kiremote.ini
	rm -f $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/kiremoted.desktop $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/kiremote.desktop
	for l in $(LINGUAS); do rm -f $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/kiremote.mo; done

lint:
	@if grep -nE '\b(strcpy|strcat|sprintf|vsprintf|gets)[[:space:]]*\(' src/*.c src/*.h ui/*.c ui/*.h mirror/*.c mirror/*.h tests/unit/*.c tests/fuzz/*.c; then \
		echo "*** banned function used"; exit 1; fi
	@command -v cppcheck >/dev/null || { echo "*** cppcheck not installed"; exit 1; }
	cppcheck --std=c11 --enable=warning,performance,portability \
		--error-exitcode=1 --inline-suppr --quiet \
		--suppress=normalCheckLevelMaxBranches -Isrc -Imirror -Ivendor/cjson \
		-D_XOPEN_SOURCE=700 -DKR_VERSION='"lint"' -DKR_FIXTURES_DIR='"."' src ui/krmodel.c ui/krclient.c mirror tests/unit tests/fuzz

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
