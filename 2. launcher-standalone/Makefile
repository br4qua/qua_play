CC = gcc
CFLAGS = -O2 -march=native -flto -Wall -Wextra
LDFLAGS = -static -flto
TARGET = qua-bare-launcher
PREFIX = /usr/local/bin
SRC = qua-bare-launcher.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

install: $(TARGET)
	sudo cp $(TARGET) $(PREFIX)/$(TARGET)
	sudo chmod 755 $(PREFIX)/$(TARGET)
	sudo setcap 'cap_sys_nice,cap_sys_resource,cap_ipc_lock+ep' $(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET)

uninstall:
	sudo rm -f $(PREFIX)/$(TARGET)

.PHONY: all install clean uninstall
