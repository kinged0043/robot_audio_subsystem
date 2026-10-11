// TinyUSB configuration: one UAC2 function, full speed, no other classes.

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#include "usb_descriptors.h"

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_AUDIO               1
#define CFG_TUD_VENDOR              0

#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN       TUD_AUDIO_ROBOT_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT       2
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ    64
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP   0

// Mic in: 4 ch x 16 bit, up to 49 frames per 1 ms packet = 392 bytes.
#define CFG_TUD_AUDIO_ENABLE_EP_IN          1
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX   TUD_AUDIO_EP_SIZE(48000, AUDIO_BYTES_PER_SAMPLE, AUDIO_N_CHANNELS_MIC)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX * 4)

// Speaker out: 2 ch x 16 bit, up to 49 frames per packet = 196 bytes.
#define CFG_TUD_AUDIO_ENABLE_EP_OUT         1
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX  TUD_AUDIO_EP_SIZE(48000, AUDIO_BYTES_PER_SAMPLE, AUDIO_N_CHANNELS_SPK)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX * 4)

#endif
