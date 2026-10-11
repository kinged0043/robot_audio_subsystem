// I2S transmitter fed by two ping-pong DMA channels.
//
// USB delivers speaker audio on the host's clock, and I2S runs on the board
// crystal. The ring buffer absorbs the difference: when it runs too full one
// frame is dropped, and when it runs too empty one frame is repeated.

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "board_pins.h"
#include "i2s_out.h"
#include "i2s_out.pio.h"

#define I2S_PIO             pio1
#define I2S_DMA_IRQ         DMA_IRQ_0
#define FRAMES_PER_BLOCK    (AUDIO_SAMPLE_RATE / 1000u)   // 1 ms
#define WORDS_PER_BLOCK     (FRAMES_PER_BLOCK * 2u)

#define RING_FRAMES         512u        // power of two, about 10.7 ms
#define RING_LOW            (RING_FRAMES / 4u)
#define RING_TARGET         (RING_FRAMES / 2u)
#define RING_HIGH           (RING_FRAMES * 3u / 4u)

static uint i2s_sm;
static int dma_ch[2];
static uint32_t dma_buf[2][WORDS_PER_BLOCK];

// Each entry packs one frame: left in bits 15..0, right in bits 31..16.
static uint32_t ring[RING_FRAMES];
static volatile uint32_t ring_head;     // written by i2s_out_write
static volatile uint32_t ring_tail;     // written by the DMA interrupt
static volatile bool reset_request;
static bool prebuffering = true;
static uint32_t last_frame;

static void __not_in_flash_func(fill_block)(uint32_t *dst) {
    if (reset_request) {
        reset_request = false;
        ring_tail = ring_head;
        prebuffering = true;
        last_frame = 0;
    }

    uint32_t tail = ring_tail;
    uint32_t avail = ring_head - tail;

    if (prebuffering && avail >= RING_TARGET) prebuffering = false;

    if (prebuffering) {
        memset(dst, 0, WORDS_PER_BLOCK * sizeof(uint32_t));
        return;
    }

    // Slip one frame to stay near the target fill level.
    bool repeat_one = avail < RING_LOW;
    if (avail > RING_HIGH) {
        tail++;
        avail--;
    }

    for (uint i = 0; i < FRAMES_PER_BLOCK; i++) {
        if (repeat_one && i == FRAMES_PER_BLOCK / 2) {
            repeat_one = false;     // send last_frame again without consuming
        } else if (avail) {
            last_frame = ring[tail & (RING_FRAMES - 1)];
            tail++;
            avail--;
        } else {
            prebuffering = true;    // ran dry: hold the last value, then refill
        }
        dst[2 * i] = last_frame << 16;                  // left, MSB-aligned
        dst[2 * i + 1] = last_frame & 0xFFFF0000u;      // right, MSB-aligned
    }
    ring_tail = tail;
}

static void __not_in_flash_func(i2s_dma_irq)(void) {
    for (uint i = 0; i < 2; i++) {
        uint32_t mask = 1u << dma_ch[i];
        if (dma_hw->ints0 & mask) {
            dma_hw->ints0 = mask;
            // The other channel is playing now; refill and re-arm this one.
            fill_block(dma_buf[i]);
            dma_channel_set_read_addr(dma_ch[i], dma_buf[i], false);
        }
    }
}

void i2s_out_init(void) {
    memset(dma_buf, 0, sizeof(dma_buf));

    uint offset = pio_add_program(I2S_PIO, &i2s_out_program);
    i2s_sm = pio_claim_unused_sm(I2S_PIO, true);
    // 2 PIO cycles per bit, 64 bits per frame.
    float div = (float)clock_get_hz(clk_sys) / (float)(AUDIO_SAMPLE_RATE * 64u * 2u);
    i2s_out_program_init(I2S_PIO, i2s_sm, offset, PIN_I2S_DATA, PIN_I2S_BCLK, div);

    dma_ch[0] = dma_claim_unused_channel(true);
    dma_ch[1] = dma_claim_unused_channel(true);
    for (uint i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(I2S_PIO, i2s_sm, true));
        channel_config_set_chain_to(&c, dma_ch[i ^ 1]);
        dma_channel_configure(dma_ch[i], &c, &I2S_PIO->txf[i2s_sm], dma_buf[i],
                              WORDS_PER_BLOCK, false);
        dma_channel_set_irq0_enabled(dma_ch[i], true);
    }
    irq_set_exclusive_handler(I2S_DMA_IRQ, i2s_dma_irq);
    irq_set_enabled(I2S_DMA_IRQ, true);

    dma_channel_start(dma_ch[0]);
    pio_sm_set_enabled(I2S_PIO, i2s_sm, true);
}

size_t i2s_out_write(const int16_t *lr, size_t frames) {
    uint32_t head = ring_head;
    uint32_t space = RING_FRAMES - (head - ring_tail);
    if (frames > space) frames = space;

    for (size_t i = 0; i < frames; i++) {
        int32_t l = lr[2 * i];
        int32_t r = lr[2 * i + 1];
#if SPEAKER_MONO_MIX
        l = r = (l + r) / 2;
#endif
        ring[(head + i) & (RING_FRAMES - 1)] = ((uint32_t)(uint16_t)r << 16) | (uint16_t)l;
    }
    __dmb();
    ring_head = head + frames;
    return frames;
}

void i2s_out_reset(void) {
    reset_request = true;
}
