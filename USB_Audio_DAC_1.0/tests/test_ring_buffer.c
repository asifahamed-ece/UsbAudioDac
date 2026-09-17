/* tests/test_ring_buffer.c
 *
 * Host-simulated unit tests for the SPSC ring buffer.
 *
 * ring_buffer.h pulls in only <stdint.h>/<stddef.h>, so ring_buffer.c
 * compiles unchanged with the host gcc — no HAL, no hardware mocks.
 * Build with `make -C tests run`.
 *
 * The producer stream is globally-unique: pattern(i) for stream index i.
 * Any re-ordering, duplication, loss, or re-read of a sample breaks the
 * pattern, so a mismatch here is a real ring bug.
 */

#include <stdio.h>
#include <string.h>

#include "../Core/Inc/ring_buffer.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond)                                                 \
    do {                                                            \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);\
        }                                                           \
    } while (0)

/* Globally-unique int16 pattern; gcd(37, 2^16) = 1 so the value space
 * does not repeat until index 65536, well past any test's range. */
static int16_t pattern(uint32_t i)
{
    return (int16_t)(i * 37 + 11);
}

/* Fill buf[n] with a contiguous slice of the global stream starting at
 * stream position 'start'. */
static void fill_seq(int16_t *buf, uint16_t n, uint32_t start)
{
    uint16_t i;
    for (i = 0; i < n; i++) buf[i] = pattern(start + i);
}

static void test_initially_empty(void)
{
    RingBuffer_Reset();
    CHECK(RingBuffer_Available() == 0);
    CHECK(RingBuffer_Space() == RING_BUFFER_SIZE - 1);
}

static void test_write_read_roundtrip(void)
{
    int16_t in[256], out[256];
    RingBuffer_Reset();
    fill_seq(in, 256, 0);
    CHECK(RingBuffer_Write(in, 256) == 256);
    CHECK(RingBuffer_Available() == 256);
    CHECK(RingBuffer_Read(out, 256) == 256);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
    CHECK(RingBuffer_Available() == 0);
}

static void test_full_state(void)
{
    int16_t in[RING_BUFFER_SIZE], spill;
    RingBuffer_Reset();
    fill_seq(in, RING_BUFFER_SIZE - 1, 0);
    /* Writer may never be more than SIZE-1 ahead of the reader. */
    CHECK(RingBuffer_Write(in, RING_BUFFER_SIZE - 1) == RING_BUFFER_SIZE - 1);
    CHECK(RingBuffer_Available() == RING_BUFFER_SIZE - 1);
    CHECK(RingBuffer_Space() == 0);
    /* Blocked write must report 0 and enqueue nothing. */
    spill = pattern(99999);
    CHECK(RingBuffer_Write(&spill, 1) == 0);
    CHECK(RingBuffer_Available() == RING_BUFFER_SIZE - 1);
}

static void test_wraparound(void)
{
    /* Fill the ring to the brim and fully drain it twice. Both head and
     * tail cross index 1024 each pass, exercising the power-of-2 wrap
     * with the ring at its maximum occupancy. */
    int16_t in[RING_BUFFER_SIZE], out[RING_BUFFER_SIZE];
    RingBuffer_Reset();
    fill_seq(in, RING_BUFFER_SIZE - 1, 0);

    CHECK(RingBuffer_Write(in, RING_BUFFER_SIZE - 1) == RING_BUFFER_SIZE - 1);
    CHECK(RingBuffer_Read(out, RING_BUFFER_SIZE - 1) == RING_BUFFER_SIZE - 1);

    CHECK(RingBuffer_Write(in, RING_BUFFER_SIZE - 1) == RING_BUFFER_SIZE - 1);
    CHECK(RingBuffer_Read(out, RING_BUFFER_SIZE - 1) == RING_BUFFER_SIZE - 1);

    CHECK(memcmp(in, out, (RING_BUFFER_SIZE - 1) * sizeof(int16_t)) == 0);
    CHECK(RingBuffer_Available() == 0);
}

static void test_partial_ops(void)
{
    int16_t in[32], out[128];
    RingBuffer_Reset();
    fill_seq(in, 32, 0);
    CHECK(RingBuffer_Write(in, 32) == 32);
    /* Ask for more than is available: read returns 32, rest untouched. */
    memset(out, 0xAB, sizeof(out));
    CHECK(RingBuffer_Read(out, 128) == 32);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
    CHECK(out[32] == (int16_t)0xABAB);
    CHECK(RingBuffer_Available() == 0);
}

static void test_reset_clears(void)
{
    int16_t in[16];
    fill_seq(in, 16, 0);
    RingBuffer_Write(in, 16);
    RingBuffer_Reset();
    CHECK(RingBuffer_Available() == 0);
    CHECK(RingBuffer_Space() == RING_BUFFER_SIZE - 1);
}

static void test_interleaved_churn(void)
{
    /* Producer/consumer churn with independently-tracked counters.
     * Reads over-request on purpose; actual consumed count comes from
     * the return value so the final Available() oracle stays exact. */
    uint16_t step, oracle_head = 0, oracle_tail = 0;
    int16_t  in[128], out[256];
    RingBuffer_Reset();

    for (step = 0; step < 5000; step++) {
        uint16_t w  = (step * 7 + 1) % 64;      /* 1..63 */
        uint16_t rq = (step * 9 + 32) % 128;    /* 32..159, over-provisioned */
        uint16_t got;
        fill_seq(in, w, oracle_head);
        CHECK(RingBuffer_Write(in, w) == w);
        oracle_head += w;
        got = RingBuffer_Read(out, rq);
        oracle_tail += got;
    }
    CHECK(RingBuffer_Available() ==
          (uint16_t)((oracle_head - oracle_tail) & (RING_BUFFER_SIZE - 1)));
}

static void test_stream_integrity_across_wrap(void)
{
    /* Full stream-ordering check: the continuous sample stream produced
     * by write/read round-trips must equal pattern(0..N) with no gaps,
     * duplicates, or reorders — even across many ring wraps. */
    int16_t emitted[RING_BUFFER_SIZE];
    uint32_t produced_total = 0;
    uint16_t emitted_idx = 0, iter;
    int16_t in[256], out[256];
    RingBuffer_Reset();

    for (iter = 0; iter < 200; iter++) {
        uint16_t w = (iter * 13) % 180 + 16;   /* 16..195, contiguous */
        uint16_t r = (iter * 171) % 200 + 40;  /* 40..239, keeps up */
        uint16_t got, i;
        fill_seq(in, w, produced_total);
        /* If the ring ever truncates a write, the stream has a gap and
         * the final check below flags it — fail loudly here for clarity. */
        CHECK(RingBuffer_Write(in, w) == w);
        produced_total += w;
        got = RingBuffer_Read(out, r);
        for (i = 0; i < got; i++) {
            if (emitted_idx < RING_BUFFER_SIZE) emitted[emitted_idx++] = out[i];
        }
    }
    /* first emitted_idx samples of the global stream, in order */
    {
        uint16_t i;
        for (i = 0; i < emitted_idx; i++) {
            if (emitted[i] != pattern(i)) {
                failures++;
                printf("  FAIL stream mismatch at index %u: got %d want %d\n",
                       i, emitted[i], pattern(i));
                break;
            }
        }
        checks++;
    }
}

#define RUN(name)                                                    \
    do {                                                             \
        printf("[TEST] %s\n", #name);                                \
        test_##name();                                               \
    } while (0)

int main(void)
{
    RUN(initially_empty);
    RUN(write_read_roundtrip);
    RUN(full_state);
    RUN(wraparound);
    RUN(partial_ops);
    RUN(reset_clears);
    RUN(interleaved_churn);
    RUN(stream_integrity_across_wrap);

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}