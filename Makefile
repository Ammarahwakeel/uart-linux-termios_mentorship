# ─────────────────────────────────────────────────────────────────────────────
# Makefile — uart_test
# LFX Mentorship: RISC-V ACT Framework / M-Mode Firmware Validation
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
TARGET  = uart_test
SRC     = uart_test.c

# Strict warnings + optimization. -g keeps debug symbols for gdb.
CFLAGS  = -Wall -Wextra -Wpedantic -O2 -g

# Default: build with select()
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)
	@echo "Built $(TARGET) using select()"

# Alternative: build with poll() instead of select()
poll: $(SRC)
	$(CC) $(CFLAGS) -DUSE_POLL -o $(TARGET) $(SRC)
	@echo "Built $(TARGET) using poll()"

clean:
	rm -f $(TARGET)

.PHONY: all poll clean
