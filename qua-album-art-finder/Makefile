CC = gcc

CFLAGS = -O3 -march=native -mtune=native -flto -fomit-frame-pointer -pipe -Wall -Wextra

# Only the tools you are actually using
TARGETS = qua-album-art-finder qua-album-art-finder-biggest

all: $(TARGETS)

# Pattern rule: compiles the .c file into a binary of the same name
%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

install: $(TARGETS)
	install -m 755 $(TARGETS) /usr/local/bin/

.PHONY: all clean install
