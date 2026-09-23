# tests/Makefile
#
# Host-simulated unit tests for the firmware's pure-logic modules.
# Built with the host toolchain (gcc) — no ARM toolchain, no hardware.
#
#   make run     # build and run all tests
#   make clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99
INC     = -I../Core/Inc -I../Drivers/CMSIS/DSP/Include -I../Drivers/CMSIS/Include

DSP_SRC = ../Drivers/CMSIS/DSP/Source
# Pure-C CMSIS-DSP objects needed for arm_rfft_fast_f32 + arm_cmplx_mag_f32.
# -ffunction-sections/--gc-sections drops unused MFCC/Statistics refs.
CMSIS_SRCS = \
	$(DSP_SRC)/TransformFunctions/TransformFunctions.c \
	$(DSP_SRC)/ComplexMathFunctions/ComplexMathFunctions.c \
	$(DSP_SRC)/CommonTables/CommonTables.c \
	$(DSP_SRC)/FastMathFunctions/FastMathFunctions.c \
	$(DSP_SRC)/BasicMathFunctions/BasicMathFunctions.c
CMSIS_LDFLAGS = -ffunction-sections -fdata-sections -Wl,--gc-sections

BIN = test_ring_buffer test_audio_fft

all: $(BIN)

test_ring_buffer: test_ring_buffer.c ../Core/Src/ring_buffer.c
	$(CC) $(CFLAGS) -I../Core/Inc $^ -o $@

test_audio_fft: test_audio_fft.c ../Core/Src/audio_fft.c $(CMSIS_SRCS)
	$(CC) $(CFLAGS) $(INC) test_audio_fft.c ../Core/Src/audio_fft.c \
		$(CMSIS_SRCS) $(CMSIS_LDFLAGS) -o $@ -lm

run: $(BIN)
	./test_ring_buffer
	./test_audio_fft

clean:
	-rm -f $(BIN)

.PHONY: all run clean
