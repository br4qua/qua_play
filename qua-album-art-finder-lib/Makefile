CC = gcc
CFLAGS = -O3 -fPIC -march=native -mtune=native -pipe -Wall -Wextra
INCLUDES = -I./include
LIBNAME = libqua.so

# Default target
all: $(LIBNAME)

$(LIBNAME): src/libqua.c
	$(CC) $(CFLAGS) $(INCLUDES) -shared -o $@ $^

clean:
	rm -f $(LIBNAME)

.PHONY: all clean
