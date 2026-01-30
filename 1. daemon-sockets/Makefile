CC = musl-gcc
CFLAGS = -Os -march=native -mtune=native -flto -Wall
LDFLAGS = -flto -Wl,-O1 -Wl,--as-needed -Wl,--strip-all
TARGET = qua-socket
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): qua-socket.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) qua-socket.c

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
