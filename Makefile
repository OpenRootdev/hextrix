# Hextrix — terminal fluid color toy
#
#   make          build ./hextrix
#   make install  copy the binary to $(PREFIX)/bin (default ~/.local/bin)
#   make clean    remove build artifacts
#
# Audio support is compiled in automatically when the PipeWire
# development files are available (pkg-config: libpipewire-0.3).

CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wshadow -Wpedantic
OPT     ?= -O2
CFLAGS  ?= $(OPT) $(CSTD) $(WARN)
CFLAGS  += -D_DEFAULT_SOURCE
LDFLAGS ?=

NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
NCURSES_LIBS   := $(shell pkg-config --libs ncursesw 2>/dev/null)
LDLIBS := $(NCURSES_LIBS) -lm

HAVE_PIPEWIRE := $(shell pkg-config --exists libpipewire-0.3 && echo 1)
ifeq ($(HAVE_PIPEWIRE),1)
# -isystem keeps the PipeWire/SPA headers (which use GNU extensions) from
# tripping -Wpedantic on our own translation units.
PIPEWIRE_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3 | sed 's/-I/-isystem /g')
CFLAGS += -DHEXTRIX_HAVE_PIPEWIRE=1 $(PIPEWIRE_CFLAGS)
LDLIBS += $(shell pkg-config --libs libpipewire-0.3) -lpthread
endif

SRC := src/main.c src/grid.c src/fluid.c src/renderer.c src/input.c src/audio.c
OBJ := $(SRC:src/%.c=build/%.o)
DEP := $(OBJ:.o=.d)

all: hextrix

hextrix: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build hextrix

PREFIX  ?= $(HOME)/.local
BINDIR  := $(DESTDIR)$(PREFIX)/bin

install: hextrix
	install -Dm755 hextrix $(BINDIR)/hextrix

uninstall:
	rm -f $(BINDIR)/hextrix

-include $(DEP)

.PHONY: all clean install uninstall
