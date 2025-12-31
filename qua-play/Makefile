CC = gcc
CFLAGS = -O3 -march=native -mtune=native -flto -Wall
LDFLAGS = -pthread -flto -lFLAC -lwavpack

# Target name
TARGET = qua-play

# Source files
SRCS = src/qua-play.c src/qua-decode.c src/qua-cache.c src/qua-next.c src/qua-post-processing.c src/qua-config.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete. Run with: ./$(TARGET)"

clean:
	rm -f $(TARGET)