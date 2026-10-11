// Robot audio subsystem firmware (RP2040).
//
// Shows up on the host as a USB Audio Class 2 device with
//   - a 4-channel, 48 kHz, 16-bit microphone array (MIC1, MIC3, MIC5, MIC7;
//     the other 4 mics on the board are not captured)
//   - a 48 kHz, 16-bit stereo speaker, played through the TAS5805M
//
// Core 0: USB, I2S output, amp control.
// Core 1: PDM capture and decimation.

#include <string.h>

#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "board_pins.h"
#include "i2s_out.h"
#include "pdm_mics.h"
#include "tas5805m.h"
#include "usb_descriptors.h"

#define FAULT_POLL_MS       250u

// USB volume range offered to the host, in 1/256 dB.
#define VOLUME_MIN          (-60 * 256)
#define VOLUME_MAX          0
#define VOLUME_RES          (256 / 2)       // 0.5 dB, the amp's step

static volatile bool mic_streaming;
static volatile bool spk_streaming;

// Master channel controls from the host. Applied to the amp from the main loop,
// because I2C must not run inside USB callbacks.
static bool spk_mute;
static int16_t spk_volume = VOLUME_MAX;
static volatile bool amp_settings_dirty = true;

static bool amp_ok;
static volatile uint32_t amp_last_faults;

// ---------------------------------------------------------------------------
// Audio data movement

static void mic_task(void) {
    static mic_frame_t frames[16];

    if (!mic_streaming) {
        pdm_mics_flush();
        return;
    }

    // Write whole frames only, so channels never shift in the stream.
    tu_fifo_t *ff = tud_audio_get_ep_in_ff();
    if (!ff) return;
    size_t room = tu_fifo_remaining(ff) / sizeof(mic_frame_t);
    while (room) {
        size_t want = room < count_of(frames) ? room : count_of(frames);
        size_t got = pdm_mics_read(frames, want);
        if (!got) break;
        tud_audio_write(frames, (uint16_t)(got * sizeof(mic_frame_t)));
        room -= got;
    }
}

static void speaker_task(void) {
    static int16_t buf[2 * 64];

    if (!spk_streaming) return;

    // Read whole stereo frames (4 bytes) only.
    uint16_t avail = (uint16_t)(tud_audio_available() & ~3u);
    while (avail) {
        uint16_t n = avail < sizeof(buf) ? avail : sizeof(buf);
        n = tud_audio_read(buf, n);
        if (!n) break;
        i2s_out_write(buf, n / 4u);
        avail -= n;
    }
}

static void amp_task(void) {
    if (!amp_ok) return;

    if (amp_settings_dirty) {
        amp_settings_dirty = false;
        tas5805m_set_volume(spk_volume, spk_mute);
    }

    static absolute_time_t next_poll;
    if (time_reached(next_poll)) {
        next_poll = make_timeout_time_ms(FAULT_POLL_MS);
        uint32_t faults = tas5805m_check_faults();
        if (faults) amp_last_faults = faults;
    }
}

int main(void) {
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

    // I2S clocks must be running before the amp leaves Hi-Z.
    i2s_out_init();
    amp_ok = tas5805m_init();

    multicore_launch_core1(pdm_mics_core1_entry);

    tud_init(0);

    while (true) {
        tud_task();
        mic_task();
        speaker_task();
        amp_task();
    }
}

// ---------------------------------------------------------------------------
// USB audio class requests

static bool clock_get_request(uint8_t rhport, audio_control_request_t const *req) {
    if (req->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t cur = {.bCur = (int32_t)tu_htole32(AUDIO_SAMPLE_RATE)};
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, (tusb_control_request_t const *)req, &cur, sizeof(cur));
        }
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(1) range = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = {.bMin = (int32_t)AUDIO_SAMPLE_RATE,
                                .bMax = (int32_t)AUDIO_SAMPLE_RATE,
                                .bRes = 0},
            };
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, (tusb_control_request_t const *)req, &range, sizeof(range));
        }
    } else if (req->bControlSelector == AUDIO_CS_CTRL_CLK_VALID &&
               req->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(
            rhport, (tusb_control_request_t const *)req, &valid, sizeof(valid));
    }
    return false;
}

static bool clock_set_request(audio_control_request_t const *req, uint8_t const *buf) {
    if (req->bControlSelector != AUDIO_CS_CTRL_SAM_FREQ || req->bRequest != AUDIO_CS_REQ_CUR) {
        return false;
    }
    TU_VERIFY(req->wLength == sizeof(audio_control_cur_4_t));
    // The PDM and I2S clocks are fixed, so 48 kHz is the only rate.
    return (uint32_t)((audio_control_cur_4_t const *)buf)->bCur == AUDIO_SAMPLE_RATE;
}

static bool feature_unit_get_request(uint8_t rhport, audio_control_request_t const *req) {
    if (req->bChannelNumber != 0) return false;   // master channel only

    if (req->bControlSelector == AUDIO_FU_CTRL_MUTE && req->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute = {.bCur = spk_mute};
        return tud_audio_buffer_and_schedule_control_xfer(
            rhport, (tusb_control_request_t const *)req, &mute, sizeof(mute));
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = {.bMin = tu_htole16(VOLUME_MIN),
                                .bMax = tu_htole16(VOLUME_MAX),
                                .bRes = tu_htole16(VOLUME_RES)},
            };
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, (tusb_control_request_t const *)req, &range, sizeof(range));
        }
        if (req->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t vol = {.bCur = tu_htole16(spk_volume)};
            return tud_audio_buffer_and_schedule_control_xfer(
                rhport, (tusb_control_request_t const *)req, &vol, sizeof(vol));
        }
    }
    return false;
}

static bool feature_unit_set_request(audio_control_request_t const *req, uint8_t const *buf) {
    if (req->bChannelNumber != 0 || req->bRequest != AUDIO_CS_REQ_CUR) return false;

    if (req->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(req->wLength == sizeof(audio_control_cur_1_t));
        spk_mute = ((audio_control_cur_1_t const *)buf)->bCur != 0;
        amp_settings_dirty = true;
        return true;
    }
    if (req->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(req->wLength == sizeof(audio_control_cur_2_t));
        int16_t v = (int16_t)tu_le16toh(((audio_control_cur_2_t const *)buf)->bCur);
        if (v < VOLUME_MIN) v = VOLUME_MIN;
        if (v > VOLUME_MAX) v = VOLUME_MAX;
        spk_volume = v;
        amp_settings_dirty = true;
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *req = (audio_control_request_t const *)p_request;
    if (req->bEntityID == UAC2_ENTITY_CLOCK) return clock_get_request(rhport, req);
    if (req->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) return feature_unit_get_request(rhport, req);
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request,
                                 uint8_t *buf) {
    (void)rhport;
    audio_control_request_t const *req = (audio_control_request_t const *)p_request;
    if (req->bEntityID == UAC2_ENTITY_CLOCK) return clock_set_request(req, buf);
    if (req->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) return feature_unit_set_request(req, buf);
    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
        spk_streaming = alt != 0;
        i2s_out_reset();
    } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
        pdm_mics_flush();
        mic_streaming = alt != 0;
    }
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));

    if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
        spk_streaming = false;
        i2s_out_reset();
    } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
        mic_streaming = false;
    }
    return true;
}

void tud_umount_cb(void) {
    spk_streaming = false;
    mic_streaming = false;
    i2s_out_reset();
}
