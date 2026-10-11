// TAS5805M class-D amplifier (U2), controlled over I2C0.

#ifndef TAS5805M_H
#define TAS5805M_H

#include <stdbool.h>
#include <stdint.h>

// Powers up the amp and puts it in play mode. I2S clocks must already be
// running. Returns false if the amp does not answer on I2C.
bool tas5805m_init(void);

// Volume in USB Audio units (1/256 dB), 0 dB maximum.
void tas5805m_set_volume(int16_t volume_q8_db, bool mute);

// Reads and clears the fault registers. Returns 0 when there is no fault,
// otherwise CHAN_FAULT | GLOBAL_FAULT1 << 8 | GLOBAL_FAULT2 << 16.
uint32_t tas5805m_check_faults(void);

// 7-bit I2C address found at init, or 0 if none.
uint8_t tas5805m_address(void);

#endif
