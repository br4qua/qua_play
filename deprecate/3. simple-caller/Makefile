CC = musl-gcc
CFLAGS = -Os -march=native -mtune=native -flto -fno-pie -Wall
LDFLAGS = -flto -no-pie -static -Wl,-O1 -Wl,--as-needed -Wl,--strip-all
TARGET = qua-caller
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): qua-caller.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) qua-caller.c

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
