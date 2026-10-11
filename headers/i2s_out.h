// I2S output to the TAS5805M, 48 kHz, 32-bit slots, 64 x fs bit clock.
// The clocks run all the time (silence when no audio is queued), because the
// amp uses SCLK as its clock reference.

#ifndef I2S_OUT_H
#define I2S_OUT_H

#include <stddef.h>
#include <stdint.h>

// Starts BCLK/LRCLK/DATA. Call on core 0; the DMA interrupt runs there.
void i2s_out_init(void);

// Queues interleaved 16-bit stereo frames (L, R). Returns frames accepted.
size_t i2s_out_write(const int16_t *lr, size_t frames);

// Drops queued audio and waits for a fresh pre-buffer before playing again.
void i2s_out_reset(void);

#endif
