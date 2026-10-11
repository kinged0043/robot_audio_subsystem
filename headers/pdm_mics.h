// PDM microphone capture: 4 of the 8 mics, one per data line.
//
// PIO drives the PDM clock and samples the 4 shared data lines. DMA moves the
// raw bits into RAM, and core 1 turns them into 48 kHz, 16-bit PCM.

#ifndef PDM_MICS_H
#define PDM_MICS_H

#include <stddef.h>
#include <stdint.h>

#include "board_pins.h"

// One frame is one sample from each captured mic, in data line order:
// MIC1, MIC3, MIC5, MIC7 (or MIC2, 4, 6, 8 when MIC_USE_SELECT_GND is 0).
typedef struct {
    int16_t ch[MIC_CHANNELS];
} mic_frame_t;

// Core 1 entry point. Starts capture and never returns. Core 1 owns the
// PDM DMA interrupt and all the filtering.
void pdm_mics_core1_entry(void);

// Called from core 0. Copies up to max_frames frames out of the ring buffer.
size_t pdm_mics_read(mic_frame_t *dst, size_t max_frames);

// Called from core 0. Number of frames ready to read.
size_t pdm_mics_available(void);

// Called from core 0. Drops every buffered frame.
void pdm_mics_flush(void);

// Number of 1 ms raw blocks core 1 failed to process in time.
uint32_t pdm_mics_overruns(void);

#endif
