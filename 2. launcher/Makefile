# Variables
CC = gcc
CFLAGS = -O3 -Wall -Wextra
TARGET = qua-bare-launcher
PREFIX = /usr/local/bin
SRC = qua-bare-launcher.c

# Default target
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

# Install with detailed feedback
install: $(TARGET)
	@echo "--------------------------------------------------------"
	@echo "🚀 Installing $(TARGET) to $(PREFIX)..."
	sudo cp $(TARGET) $(PREFIX)/$(TARGET)
	sudo chmod 755 $(PREFIX)/$(TARGET)
	
	@echo "--------------------------------------------------------"
	@echo "🛡️  Applying Kernel Capabilities..."
	@echo "   -> cap_sys_nice     : For Real-Time Priority (FIFO 99) & CPU Affinity"
	@echo "   -> cap_sys_resource : For OOM Score Adjustment (-1000)"
	@echo "   -> cap_ipc_lock     : For Memory Locking (No Swapping/Page Faults)"
	sudo setcap 'cap_sys_nice,cap_sys_resource,cap_ipc_lock+ep' $(PREFIX)/$(TARGET)
	
	@echo "--------------------------------------------------------"
	@echo "🔍 Verifying Permissions:"
	@getcap $(PREFIX)/$(TARGET)
	
	@echo "--------------------------------------------------------"
	@echo "✅ Done! Use: $(TARGET) <core> <player_path> [args...]"
	@echo "Example: $(TARGET) 4 /usr/bin/player /tmp/test.wav hw:0,0"

# Clean build artifacts
clean:
	rm -f $(TARGET)

# Uninstall from system
uninstall:
	@echo "🗑️  Removing $(TARGET) from $(PREFIX)..."
	sudo rm -f $(PREFIX)/$(TARGET)

.PHONY: all install clean uninstall
