// Pin and clock map for the robot audio board.
// Every value here was read from audio_subsystems_schematics.kicad_sch.

#ifndef BOARD_PINS_H
#define BOARD_PINS_H

// System clock. 153.6 MHz divides evenly into both the 3.072 MHz PDM clock
// and the 3.072 MHz I2S bit clock, so the audio rate is exactly 48 kHz.
#define SYS_CLOCK_KHZ       153600u
#define AUDIO_SAMPLE_RATE   48000u

// PDM microphones (U1 GPIO10..14). 8 x SPH0141LM4H-1 in 4 pairs.
// pdmclk goes through R5 (22R) to every mic CLOCK pin.
#define PIN_PDM_CLK         10u
// pdmdata1..4. Must stay consecutive: the PIO program reads them as one group.
#define PIN_PDM_DATA_BASE   11u
#define PDM_DATA_LINES      4u
#define PDM_NUM_MICS        8u    // fitted on the board

// Only 4 mics are captured: one per data line, so one from each pair.
// 1 = MIC1,3,5,7 (SELECT tied to GND), 0 = MIC2,4,6,8 (SELECT tied to micbias).
#define MIC_CHANNELS        4u
#define MIC_USE_SELECT_GND  1

// Which half of the clock period each mic drives its data line.
// Odd mics (MIC1,3,5,7) have SELECT tied to GND, even mics (MIC2,4,6,8)
// have SELECT tied to micbias. Set to 0 if the channel pairs come out
// swapped on the host; nothing else changes.
#define PDM_SELECT_GND_DRIVES_CLOCK_LOW 1

// I2S to the TAS5805M (U2). BCLK and LRCLK must be consecutive (PIO side-set).
#define PIN_I2S_DATA        16u   // sdin  -> U2 SDIN
#define PIN_I2S_BCLK        17u   // sclk  -> U2 SCLK
#define PIN_I2S_LRCLK       18u   // lrclk -> U2 LRCLK

// I2C0 to the TAS5805M and header J3. R1/R2 are 4.7k pull-ups to +3V3.
#define AMP_I2C             i2c0
#define PIN_I2C_SDA         4u
#define PIN_I2C_SCL         5u
#define AMP_I2C_BAUD        400000u

// TAS5805M control lines.
// PDN: R10 (10k) pulls it low, so the amp stays off until firmware drives it high.
#define PIN_AMP_PDN         19u
// ADR/FAULT: R11 (15k) to DVDD sets the I2C address at power-up.
// Firmware must never drive this pin; it is read as an input only.
#define PIN_AMP_FAULT       20u

// Only amp channel A (OUT_A+/-) has an LC filter and speaker (MK1).
// Channel B has bootstrap caps but no load, so mix L+R into both channels.
#define SPEAKER_MONO_MIX    1

// PVDD comes from USB VBUS through FB2, so the amp runs at about 5 V.
#define AMP_PVDD_MV         5000u

#endif
