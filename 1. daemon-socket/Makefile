CC = gcc
CFLAGS = -std=c23 -D_GNU_SOURCE -O3 -march=native -mtune=native -flto -fno-pie -Wall
LDFLAGS = -flto -no-pie -Wl,-O1 -Wl,--as-needed -Wl,--strip-all
TARGET = qua-socket
SRCS = qua-socket.c qua-cache.c qua-launcher.c qua-player-selector.c
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRCS)

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/bin/
	setcap 'cap_sys_nice,cap_sys_resource,cap_ipc_lock+ep' $(PREFIX)/bin/$(TARGET)
	install -m 755 qua-send $(PREFIX)/bin/
	install -Dm 644 qua-play.desktop $(PREFIX)/share/applications/qua-play.desktop

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f $(PREFIX)/bin/qua-send
	rm -f $(PREFIX)/share/applications/qua-play.desktop

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
