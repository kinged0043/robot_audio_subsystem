// PDM capture and decimation for 4 mics (one per data line).
//
// Pipeline, per 1 ms block, all on core 1:
//   PIO: 3.072 MHz PDM clock, 8 bits per clock (4 lines x 2 clock halves)
//   DMA: 768 words per block, ping-pong between two buffers
//   8x8 bit transpose, keeping the 4 streams from the selected clock half
//   4th-order sinc (CIC) filter, decimating by 64 with byte lookup tables,
//   then DC removal and conversion to int16.
// This takes a bit over half of core 1's time.

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "pdm_mics.h"
#include "pdm_mics.pio.h"

#define PDM_DECIMATION      64u                            // 3.072 MHz / 48 kHz
#define PDM_BIT_RATE        (AUDIO_SAMPLE_RATE * PDM_DECIMATION)
#define SAMPLES_PER_BLOCK   (AUDIO_SAMPLE_RATE / 1000u)    // 48, so 1 ms
#define BYTES_PER_SAMPLE    (PDM_DECIMATION / 8u)          // 8 bytes of one mic's bitstream
#define BYTES_PER_BLOCK     (SAMPLES_PER_BLOCK * BYTES_PER_SAMPLE)              // 384 per mic
#define WORDS_PER_BLOCK     (SAMPLES_PER_BLOCK * PDM_DECIMATION * 8u / 32u)     // 768

// sinc^4 kernel: 4 x 64 taps convolved = 253 taps, padded to 256 = 32 bytes.
#define SINC_ORDER          4u
#define FIR_TAPS            256u
#define FIR_BYTES           (FIR_TAPS / 8u)
#define HISTORY_BYTES       (FIR_BYTES - BYTES_PER_SAMPLE)                      // 24

// The filter output spans +/- 2^24 for a full-scale PDM stream. A shift of
// 9 maps that onto int16 exactly. Each step lower adds 6 dB of gain.
#define MIC_GAIN_DB_6DB_STEPS   2u
#define PCM_SHIFT               (9u - MIC_GAIN_DB_6DB_STEPS)

// DC blocker pole, about 1.9 Hz at 48 kHz.
#define DC_SHIFT            12u

#define RING_FRAMES         512u    // about 10 ms; power of two

#define PDM_PIO             pio0
#define PDM_DMA_IRQ         DMA_IRQ_1

static uint pdm_sm;
static int dma_ch[2];
static uint32_t raw_buf[2][WORDS_PER_BLOCK];
static volatile uint32_t blocks_ready;     // bit i set: raw_buf[i] is full
static volatile uint32_t overruns;

// Per mic bitstream: 24 bytes of history followed by the new block.
static uint8_t mic_bits[MIC_CHANNELS][HISTORY_BYTES + BYTES_PER_BLOCK];
// Indexed [byte value][byte position] so each lookup is one load with a
// small immediate offset on the Cortex-M0+.
static int32_t lut[256][FIR_BYTES];
static int32_t dc_state[MIC_CHANNELS];

static mic_frame_t ring[RING_FRAMES];
static volatile uint32_t ring_head;   // written by core 1
static volatile uint32_t ring_tail;   // written by core 0

// ---------------------------------------------------------------------------
// Filter setup

static void build_lut(void) {
    // Integer sinc^4 kernel: a 64-tap box filter convolved with itself.
    static int32_t h[FIR_TAPS], tmp[FIR_TAPS];
    memset(h, 0, sizeof(h));
    for (uint i = 0; i < PDM_DECIMATION; i++) h[i] = 1;
    uint len = PDM_DECIMATION;
    for (uint order = 1; order < SINC_ORDER; order++) {
        memset(tmp, 0, sizeof(tmp));
        for (uint i = 0; i < len; i++) {
            for (uint j = 0; j < PDM_DECIMATION; j++) tmp[i + j] += h[i];
        }
        len += PDM_DECIMATION - 1;
        memcpy(h, tmp, sizeof(h));
    }
    // Taps sum to 64^4 = 2^24. Each bit counts as +1 or -1.
    for (uint k = 0; k < FIR_BYTES; k++) {
        for (uint v = 0; v < 256; v++) {
            int32_t acc = 0;
            for (uint b = 0; b < 8; b++) {
                int32_t tap = h[k * 8 + b];
                acc += (v & (0x80u >> b)) ? tap : -tap;
            }
            lut[v][k] = acc;
        }
    }
}

// ---------------------------------------------------------------------------
// Block processing (runs from RAM on core 1)

// 8x8 bit matrix transpose (Hacker's Delight, transpose8rS32).
// x and y hold 8 raw bytes in time order, earliest in the top byte of x.
// Row r of the result holds bit (7 - r) of every input byte, earliest bit as
// MSB. Rows 0..3 come back in x (row 0 in the top byte), rows 4..7 in y.
static inline __attribute__((always_inline)) void transpose8(uint32_t *px, uint32_t *py) {
    uint32_t x = *px, y = *py, t;
    t = (x ^ (x >> 7)) & 0x00AA00AAu;  x = x ^ t ^ (t << 7);
    t = (y ^ (y >> 7)) & 0x00AA00AAu;  y = y ^ t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCCu; x = x ^ t ^ (t << 14);
    t = (y ^ (y >> 14)) & 0x0000CCCCu; y = y ^ t ^ (t << 14);
    t = (x & 0xF0F0F0F0u) | ((y >> 4) & 0x0F0F0F0Fu);
    y = ((x << 4) & 0xF0F0F0F0u) | (y & 0x0F0F0F0Fu);
    *px = t;
    *py = y;
}

static inline __attribute__((always_inline)) int16_t sat16(int32_t v) {
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

// sinc^4 FIR over the 32 bytes at p: one table load per byte.
// On the M0+ each tap is ldrb, lsls, adds, ldr, adds: 7 cycles.
static inline __attribute__((always_inline)) int32_t fir32(const uint8_t *p) {
    const int32_t *t = &lut[0][0];
#if defined(__ARM_ARCH_6M__)
    int32_t acc, v;
#define TAP(k) "ldrb %[v], [%[p], #" #k "]\n\t"         \
               "lsls %[v], %[v], #7\n\t"                  \
               "adds %[v], %[v], %[t]\n\t"                \
               "ldr  %[v], [%[v], #(4 * " #k ")]\n\t"     \
               "adds %[acc], %[acc], %[v]\n\t"
    pico_default_asm("movs %[acc], #0\n\t"
            TAP(0)  TAP(1)  TAP(2)  TAP(3)  TAP(4)  TAP(5)  TAP(6)  TAP(7)
            TAP(8)  TAP(9)  TAP(10) TAP(11) TAP(12) TAP(13) TAP(14) TAP(15)
            TAP(16) TAP(17) TAP(18) TAP(19) TAP(20) TAP(21) TAP(22) TAP(23)
            TAP(24) TAP(25) TAP(26) TAP(27) TAP(28) TAP(29) TAP(30) TAP(31)
            : [acc] "=&l"(acc), [v] "=&l"(v)
            : [p] "l"(p), [t] "l"(t), "m"(*(const uint8_t(*)[FIR_BYTES])p)
            : "cc");
#undef TAP
    return acc;
#else
    int32_t acc = 0;
    for (uint k = 0; k < FIR_BYTES; k++) acc += t[p[k] * FIR_BYTES + k];
    return acc;
#endif
}

static void __no_inline_not_in_flash_func(process_block)(const uint32_t *raw) {
    // Each raw byte holds one PDM clock period:
    //   bit 4+j = data line j+1 at the end of the clock-low half
    //   bit j   = data line j+1 at the end of the clock-high half
    // After the transpose, x holds the clock-low streams and y the clock-high
    // streams, line 1 in the low byte. Keep only the selected half.
    for (uint i = 0; i < BYTES_PER_BLOCK; i++) {
        uint32_t x = raw[2 * i], y = raw[2 * i + 1];
        transpose8(&x, &y);
#if PDM_SELECT_GND_DRIVES_CLOCK_LOW == MIC_USE_SELECT_GND
        uint32_t lines = x;
#else
        uint32_t lines = y;
#endif
        uint8_t *dst = &mic_bits[0][HISTORY_BYTES + i];
        const uint stride = sizeof(mic_bits[0]);
        dst[0 * stride] = (uint8_t)lines;
        dst[1 * stride] = (uint8_t)(lines >> 8);
        dst[2 * stride] = (uint8_t)(lines >> 16);
        dst[3 * stride] = (uint8_t)(lines >> 24);
    }

    // Leave the frames in the ring only if core 0 has room for them.
    uint32_t head = ring_head;
    bool keep = RING_FRAMES - (head - ring_tail) >= SAMPLES_PER_BLOCK;

    for (uint m = 0; m < MIC_CHANNELS; m++) {
        const uint8_t *bits = mic_bits[m];
        int32_t dc = dc_state[m];
        for (uint s = 0; s < SAMPLES_PER_BLOCK; s++) {
            int32_t acc = fir32(&bits[s * BYTES_PER_SAMPLE]);
            dc += (acc - dc) >> DC_SHIFT;
            if (keep) {
                ring[(head + s) & (RING_FRAMES - 1)].ch[m] = sat16((acc - dc) >> PCM_SHIFT);
            }
        }
        dc_state[m] = dc;
        memmove(mic_bits[m], &mic_bits[m][BYTES_PER_BLOCK], HISTORY_BYTES);
    }

    if (keep) {
        __dmb();
        ring_head = head + SAMPLES_PER_BLOCK;
    }
}

static void __not_in_flash_func(pdm_dma_irq)(void) {
    for (uint i = 0; i < 2; i++) {
        uint32_t mask = 1u << dma_ch[i];
        if (dma_hw->ints1 & mask) {
            dma_hw->ints1 = mask;
            // The other channel is running now; re-arm this one for later.
            dma_channel_set_write_addr(dma_ch[i], raw_buf[i], false);
            if (blocks_ready & (1u << i)) overruns++;
            blocks_ready |= 1u << i;
        }
    }
}

// ---------------------------------------------------------------------------
// Setup

static void setup_dma(void) {
    dma_ch[0] = dma_claim_unused_channel(true);
    dma_ch[1] = dma_claim_unused_channel(true);

    for (uint i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(PDM_PIO, pdm_sm, false));
        channel_config_set_chain_to(&c, dma_ch[i ^ 1]);
        dma_channel_configure(dma_ch[i], &c, raw_buf[i], &PDM_PIO->rxf[pdm_sm],
                              WORDS_PER_BLOCK, false);
        dma_channel_set_irq1_enabled(dma_ch[i], true);
    }
    irq_set_exclusive_handler(PDM_DMA_IRQ, pdm_dma_irq);
    irq_set_enabled(PDM_DMA_IRQ, true);
}

static void pdm_mics_start(void) {
    build_lut();

    uint offset = pio_add_program(PDM_PIO, &pdm_quad_program);
    pdm_sm = pio_claim_unused_sm(PDM_PIO, true);
    // 10 PIO cycles per PDM clock.
    float div = (float)clock_get_hz(clk_sys) / (float)(PDM_BIT_RATE * 10u);
    pdm_quad_program_init(PDM_PIO, pdm_sm, offset, PIN_PDM_CLK, PIN_PDM_DATA_BASE, div);

    setup_dma();
    dma_channel_start(dma_ch[0]);
    pio_sm_set_enabled(PDM_PIO, pdm_sm, true);
}

void pdm_mics_core1_entry(void) {
    pdm_mics_start();

    uint next = 0;
    while (true) {
        while (!(blocks_ready & (1u << next))) {
            __wfi();
        }
        process_block(raw_buf[next]);
        uint32_t irq_state = save_and_disable_interrupts();
        blocks_ready &= ~(1u << next);
        restore_interrupts(irq_state);
        next ^= 1u;
    }
}

// ---------------------------------------------------------------------------
// Core 0 side

size_t pdm_mics_available(void) {
    return ring_head - ring_tail;
}

size_t pdm_mics_read(mic_frame_t *dst, size_t max_frames) {
    uint32_t tail = ring_tail;
    size_t n = ring_head - tail;
    __dmb();
    if (n > max_frames) n = max_frames;
    for (size_t i = 0; i < n; i++) {
        dst[i] = ring[(tail + i) & (RING_FRAMES - 1)];
    }
    __dmb();
    ring_tail = tail + n;
    return n;
}

void pdm_mics_flush(void) {
    ring_tail = ring_head;
}

uint32_t pdm_mics_overruns(void) {
    return overruns;
}
