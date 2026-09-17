# tests/Makefile
#
# Host-simulated unit tests for the firmware's pure-logic modules.
# Built with the host toolchain (gcc) — no ARM toolchain, no hardware.
#
#   make run     # build and run the tests
#   make clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99
INC     = -I../Core/Inc

SRCS = test_ring_buffer.c ../Core/Src/ring_buffer.c
BIN  = test_ring_buffer

all: $(BIN)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(INC) $(SRCS) -o $@

run: $(BIN)
	./$(BIN)

clean:
	-rm -f $(BIN)

.PHONY: all run clean