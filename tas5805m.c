// TAS5805M driver. Register map: TAS5805M datasheet (SLOS958), book 0, page 0.

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "board_pins.h"
#include "tas5805m.h"

#define REG_PAGE            0x00
#define REG_DEVICE_CTRL_2   0x03
#define REG_DIG_VOL         0x4C
#define REG_AGAIN           0x54
#define REG_CHAN_FAULT      0x70
#define REG_GLOBAL_FAULT1   0x71
#define REG_GLOBAL_FAULT2   0x72
#define REG_FAULT_CLEAR     0x78
#define REG_BOOK            0x7F

#define CTRL_STATE_HIZ      0x02
#define CTRL_STATE_PLAY     0x03
#define CTRL_MUTE           (1u << 3)

#define FAULT_CLEAR_ANALOG  0x80

// DIG_VOL: 0x30 is 0 dB, each step is -0.5 dB, 0xFF is mute.
#define DIG_VOL_0DB         0x30
#define DIG_VOL_MIN         0xFE

// AGAIN: 0 is 0 dB (29.5 V peak at full scale), each step is -0.5 dB, 31 max.
// PVDD is about 5 V, so 29.5 V -> 5 V needs about -15.4 dB. Use -15.5 dB so
// full-scale digital audio just reaches the rail instead of clipping hard.
#if AMP_PVDD_MV <= 5500
#define AGAIN_STEPS         31u
#else
#error "Set AGAIN_STEPS for the new PVDD: 2 * 20*log10(29.5 V / PVDD)"
#endif

// The ADR/FAULT strap picks one of these. R11 is 15k to DVDD, which the
// datasheet maps to 0x2F; the rest are probed in case the strap changes.
static const uint8_t candidate_addrs[] = {0x2F, 0x2E, 0x2D, 0x2C};

static uint8_t amp_addr;
static uint8_t ctrl2_play = CTRL_STATE_PLAY;   // play state plus the current mute bit

static bool reg_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_write_timeout_us(AMP_I2C, amp_addr, buf, 2, false, 2000) == 2;
}

static bool reg_read(uint8_t reg, uint8_t *value) {
    if (i2c_write_timeout_us(AMP_I2C, amp_addr, &reg, 1, true, 2000) != 1) return false;
    return i2c_read_timeout_us(AMP_I2C, amp_addr, value, 1, false, 2000) == 1;
}

static bool select_book0_page0(void) {
    return reg_write(REG_PAGE, 0x00) && reg_write(REG_BOOK, 0x00) && reg_write(REG_PAGE, 0x00);
}

static bool probe(void) {
    for (uint i = 0; i < count_of(candidate_addrs); i++) {
        uint8_t dummy;
        if (i2c_read_timeout_us(AMP_I2C, candidate_addrs[i], &dummy, 1, false, 2000) == 1) {
            amp_addr = candidate_addrs[i];
            return true;
        }
    }
    amp_addr = 0;
    return false;
}

bool tas5805m_init(void) {
    // ADR/FAULT is an address strap: never drive it, and remove the RP2040's
    // default pull-down so it does not load R11 before the amp samples it.
    gpio_init(PIN_AMP_FAULT);
    gpio_set_dir(PIN_AMP_FAULT, GPIO_IN);
    gpio_disable_pulls(PIN_AMP_FAULT);

    i2c_init(AMP_I2C, AMP_I2C_BAUD);
    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C_SDA);
    gpio_pull_up(PIN_I2C_SCL);

    // Datasheet start-up: supplies stable, PDN high, wait 5 ms, then I2C.
    gpio_init(PIN_AMP_PDN);
    gpio_put(PIN_AMP_PDN, 0);
    gpio_set_dir(PIN_AMP_PDN, GPIO_OUT);
    sleep_ms(10);
    gpio_put(PIN_AMP_PDN, 1);
    sleep_ms(10);

    if (!probe()) return false;
    if (!select_book0_page0()) return false;

    // Hi-Z with the DSP enabled, wait, configure, then play.
    if (!reg_write(REG_DEVICE_CTRL_2, CTRL_STATE_HIZ)) return false;
    sleep_ms(5);
    reg_write(REG_AGAIN, AGAIN_STEPS);
    reg_write(REG_DIG_VOL, DIG_VOL_0DB);
    reg_write(REG_FAULT_CLEAR, FAULT_CLEAR_ANALOG);
    return reg_write(REG_DEVICE_CTRL_2, CTRL_STATE_PLAY);
}

void tas5805m_set_volume(int16_t volume_q8_db, bool mute) {
    if (!amp_addr) return;

    // 1/256 dB units to 0.5 dB steps below 0 dB.
    int32_t steps = volume_q8_db >= 0 ? 0 : (-(int32_t)volume_q8_db + 64) / 128;
    int32_t reg = DIG_VOL_0DB + steps;
    if (reg > DIG_VOL_MIN) reg = DIG_VOL_MIN;

    reg_write(REG_DIG_VOL, (uint8_t)reg);
    ctrl2_play = CTRL_STATE_PLAY | (mute ? CTRL_MUTE : 0);
    reg_write(REG_DEVICE_CTRL_2, ctrl2_play);
}

uint32_t tas5805m_check_faults(void) {
    if (!amp_addr) return 0;

    uint8_t chan = 0, g1 = 0, g2 = 0;
    if (!reg_read(REG_CHAN_FAULT, &chan) || !reg_read(REG_GLOBAL_FAULT1, &g1) ||
        !reg_read(REG_GLOBAL_FAULT2, &g2)) {
        return 0;
    }
    uint32_t faults = chan | ((uint32_t)g1 << 8) | ((uint32_t)g2 << 16);
    if (faults) {
        // Clear latched faults and put the amp back into play.
        reg_write(REG_FAULT_CLEAR, FAULT_CLEAR_ANALOG);
        reg_write(REG_DEVICE_CTRL_2, ctrl2_play);
    }
    return faults;
}

uint8_t tas5805m_address(void) {
    return amp_addr;
}
