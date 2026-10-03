# xgpuprofile
# See LICENSE file for copyright and license details.

VERSION ?= 0.1.0
CC      ?= cc

CFLAGS ?= -Os
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wpointer-arith -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition \
	-Wformat=2 -Wconversion -Wsign-conversion

CPPFLAGS += -DVERSION=\"${VERSION}\"

LDLIBS ?=

PREFIX      ?= /usr/local
MANPREFIX   ?= ${PREFIX}/share/man
SYSCONFDIR  ?= /etc
SYSTEMDDIR  ?= ${PREFIX}/lib/systemd/system
BASHCOMPDIR ?= ${PREFIX}/share/bash-completion/completions
ZSHCOMPDIR  ?= ${PREFIX}/share/zsh/site-functions

BIN    := xgpuprofile
SRC    := xgpuprofile.c
BUILD  := build
TARGET := $(BUILD)/$(BIN)

all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	[ -f $(DESTDIR)$(SYSCONFDIR)/$(BIN).conf ] \
		|| install -Dm644 $(BIN).conf $(DESTDIR)$(SYSCONFDIR)/$(BIN).conf
	install -d $(DESTDIR)$(SYSCONFDIR)/X11/xorg.conf.d
	sed "s|@BINDIR@|$(PREFIX)/bin|g" < $(BIN).service > $(BUILD)/$(BIN).service
	install -Dm644 $(BUILD)/$(BIN).service $(DESTDIR)$(SYSTEMDDIR)/$(BIN).service
	install -d $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < $(BIN).1 > $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1
	install -Dm644 $(BIN).bash $(DESTDIR)$(BASHCOMPDIR)/$(BIN)
	install -Dm644 $(BIN).zsh $(DESTDIR)$(ZSHCOMPDIR)/_$(BIN)
	@echo
	@echo "Installed. Next:"
	@echo "    sudo systemctl daemon-reload"
	@echo "    sudo systemctl enable $(BIN).service"
	@echo "    sudo $(BIN) --mode auto"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(SYSTEMDDIR)/$(BIN).service
	rm -f $(DESTDIR)$(SYSCONFDIR)/X11/xorg.conf.d/10-$(BIN).conf
	rm -f $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1
	rm -f $(DESTDIR)$(BASHCOMPDIR)/$(BIN)
	rm -f $(DESTDIR)$(ZSHCOMPDIR)/_$(BIN)
	@echo "Removed. Config left at $(SYSCONFDIR)/$(BIN).conf"

.PHONY: all clean install uninstall
