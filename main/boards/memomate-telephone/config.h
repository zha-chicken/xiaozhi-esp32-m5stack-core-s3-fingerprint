#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// MemoMate Telephone — headless board based on the Waveshare ESP32-C6-LCD-1.69
// dev module. The 1.69" ST7789 panel has been physically removed (broken), so
// there are NO display pins here: state feedback is voice cues + an RGB LED.

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_19
#define AUDIO_I2S_GPIO_WS GPIO_NUM_22
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_20
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_21
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_23

#define AUDIO_CODEC_PA_PIN      GPIO_NUM_NC
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_8
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_7
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_PA_VOLTAGE  3.7f

// WS2812 state LED (not yet wired — planned). GPIO4 was the LCD RST line and
// is free since the panel was removed; adjust to the actual pad used when the
// LED is physically installed.
#define BUILTIN_LED_GPIO        GPIO_NUM_4

// External handset buttons are wired to the dev board's BOOT and side (PWR)
// buttons. BOOT = talk key (start/pause conversation), PWR = long-press
// power on/off.
#define BOOT_BUTTON_GPIO        GPIO_NUM_9
#define PWR_BUTTON_GPIO         GPIO_NUM_18
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#define BATTERY_EN_PIN          GPIO_NUM_15
#define BATTERY_ADC_PIN         GPIO_NUM_0
#define BATTERY_CHARGING_PIN    GPIO_NUM_NC

// Future telephone hardware (rotary dial pulse contact, hook switch,
// MAX9814 handset mic) — pins TBD once the vintage phone internals are
// wired to this module. Freed LCD pins available: 1, 2, 3, 5, 6.

#endif // _BOARD_CONFIG_H_
