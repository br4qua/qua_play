CC = musl-gcc
CFLAGS = -O3 -march=native -mtune=native -flto -Wall -static
LDFLAGS = -static -pthread -flto

# Target name
TARGET = qua-play

# Source files
SRCS = qua-play.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete. Run with: ./$(TARGET)"

clean:
	rm -f $(TARGET)
