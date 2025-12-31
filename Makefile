# Compiler and flags
CC = gcc
SOURCE = src/qua_player.c
BINDIR = bin

# --------------------------------------------------------------------------
# 🚀 Presets Configuration 🚀
# --------------------------------------------------------------------------
# Space-separated list of sample rates to build optimized binaries for.
SAMPLE_RATES = 44100 48000 96000 88200 192000 #176400 352800 384000
BITDEPTHS = 16 32

# Generate a list of all output binaries (e.g., bin/qua-player-16-44100, bin/qua-player-32-44100)
RATE_TARGETS = $(foreach bd,$(BITDEPTHS),$(foreach sr,$(SAMPLE_RATES),$(BINDIR)/qua-player-$(bd)-$(sr)))

# Base CFLAGS for extreme optimization
# Includes some private methods from the include
# Base CFLAGS for extreme optimization
# Includes some private methods from the include
CFLAGS = -std=gnu17 -D_POSIX_C_SOURCE=202405L \
-Wall \
-Wextra \
-Wno-maybe-uninitialized \
-Wno-unused-but-set-variable \
-O3 \
-march=native \
-mtune=native \
-flto=auto \
-flto-partition=one \
-fno-fat-lto-objects \
-fipa-pta \
-fivopts \
-fdevirtualize-at-ltrans \
-ftree-loop-distribution \
-frename-registers \
-fallow-store-data-races \
-fomit-frame-pointer \
-fno-stack-protector \
-no-pie \
-fno-PIE \
-fno-plt \
-ffunction-sections \
-fdata-sections \
-I./tmp/alsa-lib-1.2.14/src/pcm \
-I./tmp/alsa-lib-1.2.14/include

# -freg-struct-return \
# -fmerge-all-constants \

# --------------------------------------------------------------------------
# STABLE LDFLAGS (Compatible with GNU ld, retains LTO, adds GNU sorting)
# --------------------------------------------------------------------------
LDFLAGS = -no-pie \
-L./lib \
-Wl,-rpath=./lib \
-flto=auto \
-flto-partition=one \
-Wl,-O3 \
-Wl,--emit-relocs \
-Wl,--as-needed \
-Wl,--gc-sections \
-Wl,--as-needed \
-Wl,--hash-style=gnu
       
LIBS =  -Wl,-Bstatic -lasound -Wl,-Bdynamic

# Installation directories
PREFIX ?= /usr/local
BINDIR_INSTALL = $(PREFIX)/bin
SCRIPTS = scripts/qua-xdg-wrapper scripts/qua-play scripts/qua-stop
APPLICATIONSDIR = /usr/share/applications
DESKTOP_FILE = qua-audio-player.desktop

# Default target: build all rate-specific binaries
all: $(RATE_TARGETS)

# Debug flags
DEBUG_CFLAGS = -DDEBUG_BUILD -g -O0 -D_GNU_SOURCE

# Debug build (Builds 48000Hz version with debug flags and corrected name)
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: TARGET = $(BINDIR)/qua-player-32-48000-debug
debug: $(TARGET)
	@echo "Debug build complete: $(TARGET)"

# Build rule for the single debug target (Corrected name and indentation)
$(BINDIR)/qua-player-32-48000-debug: $(SOURCE) | bin
	# Using TARGET_SAMPLE_RATE for consistency with the C code's fixed buffer calculation
	$(CC) $(CFLAGS) \
	      -D TARGET_SAMPLE_RATE=48000 \
	      -o $@ $(SOURCE) $(LDFLAGS) $(LIBS)

# --------------------------------------------------------------------------
# Pattern Rule: Build for a specific bitdepth and sample rate
# --------------------------------------------------------------------------
# This rule matches targets like bin/qua_player_16_44100, bin/qua_player_32_48000, etc.
$(BINDIR)/qua-player-%: $(SOURCE) | bin
	$(eval BD = $(shell echo $* | cut -d- -f1))
	$(eval RATE_ID = $(shell echo $* | cut -d- -f2))
	
	@echo "Compiling and optimizing for TARGET_BITDEPTH=$(BD) TARGET_SAMPLE_RATE=$(RATE_ID)"
	$(CC) $(CFLAGS) \
	      -DTARGET_BITDEPTH=$(BD) \
	      -DTARGET_SAMPLE_RATE=$(RATE_ID) \
	      -o $@ $(SOURCE) $(LDFLAGS) $(LIBS)

# Create bin directory (Fixed indentation)
bin:
	mkdir -p $(BINDIR)
    
# Install target (Ensures correct binary names are installed)
install: $(RATE_TARGETS)
	@echo "Installing Qua Audio Player binaries and scripts..."
	# Create necessary directories
	install -d $(BINDIR_INSTALL)
	install -d $(APPLICATIONSDIR)
	
	# Install all sample-rate specific binaries
	install -m 755 $(RATE_TARGETS) $(BINDIR_INSTALL)/
	
	# Install support scripts
	install -m 755 $(SCRIPTS) $(BINDIR_INSTALL)/
	
	# Install desktop file (to be referenced by an upstream handler)
	install -m 644 $(DESKTOP_FILE) $(APPLICATIONSDIR)/
	
	@echo "Updating desktop database..."
	-update-desktop-database $(APPLICATIONSDIR) 2>/dev/null || true
	@echo "Installation complete. Installed binaries: $(RATE_TARGETS)"
    
# Uninstall target (Ensures correct binary names are uninstalled)
uninstall:
	@echo "Uninstalling Qua Audio Player binaries..."
	# Remove all sample-rate specific binaries
	$(foreach bd, $(BITDEPTHS), $(foreach sr, $(SAMPLE_RATES), rm -f $(BINDIR_INSTALL)/qua-player-$(bd)-$(sr);))
	
	# Remove scripts and desktop file
	rm -f $(addprefix $(BINDIR_INSTALL)/, $(notdir $(SCRIPTS)))
	rm -f $(APPLICATIONSDIR)/$(DESKTOP_FILE)
	@echo "Uninstallation complete."

# Clean target
clean:
	rm -rf $(BINDIR)

.PHONY: all debug install uninstall clean
