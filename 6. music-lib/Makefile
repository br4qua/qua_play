CC = gcc
CFLAGS = -O2 -g -march=native -mtune=native -flto -Wall -std=c23
LDFLAGS = -flto
LIBS = -lsqlite3 -lpng -lz -lm -lpthread
TARGET = qua-music-lib
SRCS = poc.c
PREFIX = /usr/local

TUI = qua-music-tui
TUI_SRCS = tui.c
TUI_CFLAGS = $(CFLAGS)
TUI_LDFLAGS = $(LDFLAGS)
TUI_LIBS = -lsqlite3 -lncursesw -lm -lpthread

all: $(TARGET) $(TUI)

setup: no-art.png
	mkdir -p $$HOME/.config/qua-player
	cp no-art.png $$HOME/.config/qua-player/

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS) $(LIBS)
	@echo "Build complete: ./$(TARGET)"

$(TUI): $(TUI_SRCS)
	$(CC) $(TUI_CFLAGS) $(TUI_SRCS) -o $(TUI) $(TUI_LDFLAGS) $(TUI_LIBS)
	@echo "Build complete: ./$(TUI)"

install: $(TARGET) $(TUI)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/
	install -m 755 $(TUI) $(PREFIX)/bin/
	@echo "Installed to $(PREFIX)/bin/"

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET) $(PREFIX)/bin/$(TUI)
	@echo "Uninstalled"

clean:
	rm -f $(TARGET) $(TUI)

.PHONY: all setup install uninstall clean
