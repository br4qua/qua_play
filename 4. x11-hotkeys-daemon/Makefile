CC = gcc
CFLAGS = -Os -march=native -mtune=native -flto -Wall
LDFLAGS = -flto -Wl,-O1 -Wl,--as-needed -Wl,--strip-all
LIBS = -lX11
TARGET = qua-hotkeys
SRCS = qua-hotkeys.c
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
