# NOTE: Build output is piped through grep -v 'alsa-lib' to suppress
# warnings from the static ALSA library during LTO. -isystem only
# silences header warnings, not LTO warnings from .o files in the .a archive.

# MAKEFLAGS += -j$(shell nproc)

# Compiler and flags
CC = musl-gcc
SOURCE = qua_player.c
BINDIR = bin

# --------------------------------------------------------------------------
# 🚀 Presets Configuration 🚀
# --------------------------------------------------------------------------
# Space-separated list of sample rates to build optimized binaries for.
SAMPLE_RATES = 44100 48000 96000 # 192000 88200 # 176400 352800 384000
BITDEPTHS = 16 32

# Generate a list of all output binaries (e.g., bin/qua-player-16-44100, bin/qua-player-32-44100)
RATE_TARGETS = $(foreach bd,$(BITDEPTHS),$(foreach sr,$(SAMPLE_RATES),$(BINDIR)/qua-player-$(bd)-$(sr)))

# Base CFLAGS for extreme optimization
# Includes some private methods from the include
# Base CFLAGS for extreme optimization
# Includes some private methods from the include
CFLAGS = -std=gnu23 -D_POSIX_C_SOURCE=202405L \
-Wall \
-Wextra \
-static \
-O2 \
-march=native \
-mtune=native \
-flto=auto \
-flto-partition=one \
-fno-fat-lto-objects \
-fipa-pta \
-fivopts \
-frename-registers \
-fallow-store-data-races \
-fomit-frame-pointer \
-fno-stack-protector \
-fweb \
-fira-loop-pressure \
-fsched-pressure \
-no-pie \
-fno-PIE \
-fno-plt \
-fno-semantic-interposition \
-fdata-sections \
-ffunction-sections \
-isystem ./tmp/alsa-lib-1.2.14/src/pcm \
-isystem ./tmp/alsa-lib-1.2.14/include

# -freg-struct-return \
# -fmerge-all-constants \

# --------------------------------------------------------------------------
# STABLE LDFLAGS (Compatible with GNU ld, retains LTO, adds GNU sorting)
# --------------------------------------------------------------------------
LDFLAGS = -static \
-no-pie \
-L./lib \
-flto=auto \
-flto-partition=one \
-Wl,-O2 \
-Wl,--gc-sections \
-Wl,--strip-all
LIBS = -lasound
# LIBS = -Wl,-Bstatic -lasound -Wl,-Bdynamic -luring
# Installation directories
PREFIX ?= /usr/local
BINDIR_INSTALL = $(PREFIX)/bin
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
$(BINDIR)/qua-player-%: $(SOURCE)
	@mkdir -p $(BINDIR)
	$(eval BD = $(shell echo $* | cut -d- -f1))
	$(eval RATE_ID = $(shell echo $* | cut -d- -f2))

	@echo "Compiling and optimizing for TARGET_BITDEPTH=$(BD) TARGET_SAMPLE_RATE=$(RATE_ID)"
	$(CC) $(CFLAGS) \
	      -DTARGET_BITDEPTH=$(BD) \
	      -DTARGET_SAMPLE_RATE=$(RATE_ID) \
	      -o $@ $(SOURCE) $(LDFLAGS) $(LIBS) 2>&1 | grep -vE 'alsa-lib|In function .(snd_|list_|_to_|_snd_)'; exit $${PIPESTATUS[0]}

# Create bin directory (Fixed indentation)
bin:
	mkdir -p $(BINDIR)
    
# Install target (Ensures correct binary names are installed)
install: $(RATE_TARGETS)
	@echo "Installing Qua Audio Player binaries and scripts..."
	# Create necessary directories
	install -d $(BINDIR_INSTALL)
	# Install all sample-rate specific binaries
	install -m 755 $(RATE_TARGETS) $(BINDIR_INSTALL)/

	@echo "Installation complete. Installed binaries: $(RATE_TARGETS)"
    
# Uninstall target (Ensures correct binary names are uninstalled)
uninstall:
	@echo "Uninstalling Qua Audio Player binaries..."
	# Remove all sample-rate specific binaries
	$(foreach bd, $(BITDEPTHS), $(foreach sr, $(SAMPLE_RATES), rm -f $(BINDIR_INSTALL)/qua-player-$(bd)-$(sr);))
	
	# Remove scripts
	rm -f $(addprefix $(BINDIR_INSTALL)/, $(notdir $(SCRIPTS)))
	@echo "Uninstallation complete."

# Clean target
clean:
	rm -rf $(BINDIR)

.PHONY: all debug install uninstall clean
