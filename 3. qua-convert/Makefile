CC = musl-gcc
CFLAGS = -O2 -march=native -mtune=native -flto -Wall
LDFLAGS = -static -pthread -flto
# Target name
TARGET = qua-convert
# Source files (cache logic moved to qua-socket daemon)
SRCS = qua-convert.c qua-decode.c qua-post-processing.c qua-config.c

# Install prefix
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete. Run with: ./$(TARGET)"

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/
	@echo "Installed to $(PREFIX)/bin/$(TARGET)"

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	@echo "Uninstalled $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
