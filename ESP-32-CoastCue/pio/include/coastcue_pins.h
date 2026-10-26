#pragma once

// CoastCue board profiles. Keep the boat build on T-Beam and the shore build on
// a simple ESP32 + RA-02 gateway unless you intentionally add a new profile.
#define COASTCUE_BOARD_TBEAM_SX127X 1
#define COASTCUE_BOARD_DEVKIT_RA02 2

#define COASTCUE_TBEAM_PMU_NONE 0
#define COASTCUE_TBEAM_PMU_AXP192 1
#define COASTCUE_TBEAM_PMU_AXP2101 2

#ifndef COASTCUE_BOARD_PROFILE
#if defined(COASTCUE_ROLE_SHORE)
#define COASTCUE_BOARD_PROFILE COASTCUE_BOARD_DEVKIT_RA02
#else
#define COASTCUE_BOARD_PROFILE COASTCUE_BOARD_TBEAM_SX127X
#endif
#endif

#if COASTCUE_BOARD_PROFILE == COASTCUE_BOARD_TBEAM_SX127X

#define COASTCUE_BOARD_LABEL "T-Beam SX127x"

// LILYGO T-Beam SX127x GPS and LoRa wiring. Buy the frequency variant that
// matches the selected region profile; SX1262 boards need a separate profile.
#define COASTCUE_PIN_GPS_RX 34
#define COASTCUE_PIN_GPS_TX 12

// GPIO32/33 are wired to LoRa DIO2/DIO1 on T-Beam SX127x. Keep external LEDs
// off those pins so they cannot load the radio interrupt lines.
#define COASTCUE_PIN_LED_BLUE 13
#define COASTCUE_PIN_LED_RED 2

#define COASTCUE_PIN_BUTTON 14
#define COASTCUE_PIN_BUZZER 25

#define COASTCUE_PIN_I2C_SDA 21
#define COASTCUE_PIN_I2C_SCL 22
#define COASTCUE_PIN_PMU_IRQ 35

#define COASTCUE_PIN_LORA_SCK 5
#define COASTCUE_PIN_LORA_MISO 19
#define COASTCUE_PIN_LORA_MOSI 27
#define COASTCUE_PIN_LORA_NSS 18
#define COASTCUE_PIN_LORA_RST 23
#define COASTCUE_PIN_LORA_DIO0 26
#define COASTCUE_PIN_LORA_DIO1 -1

#ifndef COASTCUE_TBEAM_PMU
#define COASTCUE_TBEAM_PMU COASTCUE_TBEAM_PMU_AXP2101
#endif

#ifndef COASTCUE_OLED_ENABLED
#define COASTCUE_OLED_ENABLED 0
#endif

#ifndef COASTCUE_BATTERY_ADC_ENABLED
#define COASTCUE_BATTERY_ADC_ENABLED 0
#endif

#elif COASTCUE_BOARD_PROFILE == COASTCUE_BOARD_DEVKIT_RA02

#define COASTCUE_BOARD_LABEL "ESP32 DevKit + RA-02"

// Shore gateway defaults for a regular ESP32 DevKit wired to an RA-02/SX1278.
#define COASTCUE_PIN_GPS_RX 16
#define COASTCUE_PIN_GPS_TX 17

#define COASTCUE_PIN_LED_BLUE 26
#define COASTCUE_PIN_LED_RED 27

#define COASTCUE_PIN_BUTTON 14
#define COASTCUE_PIN_BUZZER 25

#define COASTCUE_PIN_I2C_SDA 21
#define COASTCUE_PIN_I2C_SCL 22
#define COASTCUE_PIN_PMU_IRQ -1

#define COASTCUE_PIN_LORA_SCK 18
#define COASTCUE_PIN_LORA_MISO 19
#define COASTCUE_PIN_LORA_MOSI 23
#define COASTCUE_PIN_LORA_NSS 5
#define COASTCUE_PIN_LORA_RST 32
#define COASTCUE_PIN_LORA_DIO0 33
#define COASTCUE_PIN_LORA_DIO1 -1

#ifndef COASTCUE_TBEAM_PMU
#define COASTCUE_TBEAM_PMU COASTCUE_TBEAM_PMU_NONE
#endif

#ifndef COASTCUE_OLED_ENABLED
#define COASTCUE_OLED_ENABLED 1
#endif

#ifndef COASTCUE_BATTERY_ADC_ENABLED
#define COASTCUE_BATTERY_ADC_ENABLED 0
#endif

#else
#error "Unsupported COASTCUE_BOARD_PROFILE"
#endif

#define COASTCUE_OLED_ADDR 0x3C
#define COASTCUE_OLED_WIDTH 128
#ifndef COASTCUE_OLED_HEIGHT
#if COASTCUE_BOARD_PROFILE == COASTCUE_BOARD_DEVKIT_RA02
#define COASTCUE_OLED_HEIGHT 64
#else
#define COASTCUE_OLED_HEIGHT 32
#endif
#endif
#if COASTCUE_OLED_ENABLED && \
    COASTCUE_OLED_HEIGHT != 32 && COASTCUE_OLED_HEIGHT != 64
#error "Enabled CoastCue OLED height must be exactly 32 or 64 pixels"
#endif
#define COASTCUE_OLED_RESET -1

#ifndef COASTCUE_PIN_BATTERY_ADC
#define COASTCUE_PIN_BATTERY_ADC 34
#endif

// On the T-Beam SX127x, GPIO34 is the GPS UART RX input.  The optional ADC
// fallback must use a separately wired input; sampling this default pin would
// interfere with GPS reception.  The normal PMU path leaves this disabled.
#if COASTCUE_BOARD_PROFILE == COASTCUE_BOARD_TBEAM_SX127X && \
    COASTCUE_BATTERY_ADC_ENABLED && \
    COASTCUE_PIN_BATTERY_ADC == COASTCUE_PIN_GPS_RX
#error "T-Beam battery ADC pin conflicts with GPS RX; choose a separate ADC pin"
#endif

// LiPo 1S empty/full millivolts at the cell, used only by the optional ADC
// fallback when a PMU reading is unavailable.
#define COASTCUE_BATTERY_MIN_MV 3300
#define COASTCUE_BATTERY_MAX_MV 4200
#define COASTCUE_BATTERY_DIVIDER_RATIO 2.0f
