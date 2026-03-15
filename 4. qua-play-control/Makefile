CC      = musl-gcc
CFLAGS  = -std=c23 -O2 -static -flto -fno-pie -march=native
SRC     = qua-ctl.c

TARGETS = qua-next qua-prev qua-stop qua-play qua-play-this

all: $(TARGETS)

qua-next: $(SRC)
	$(CC) $(CFLAGS) -DMSG='"play-next"' -o $@ $<

qua-prev: $(SRC)
	$(CC) $(CFLAGS) -DMSG='"play-prev"' -o $@ $<

qua-stop: $(SRC)
	$(CC) $(CFLAGS) -DMSG='"stop"' -o $@ $<

qua-play: $(SRC)
	$(CC) $(CFLAGS) -DMSG='"play"' -o $@ $<

qua-play-this: qua-ctl-file.c
	$(CC) $(CFLAGS) -o $@ $<

PREFIX  = /usr/local/bin

install: all
	install -m 755 $(TARGETS) $(PREFIX)/

uninstall:
	cd $(PREFIX) && rm -f $(TARGETS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean install uninstall
