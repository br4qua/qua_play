CC = gcc
CFLAGS = -O3 -march=native -mtune=native -flto -Wall
LDFLAGS = -pthread -flto -lFLAC -lwavpack
# Target name
TARGET = qua-play
# Source files
SRCS = src/qua-play.c src/qua-decode.c src/qua-cache.c src/qua-next.c src/qua-post-processing.c src/qua-config.c

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