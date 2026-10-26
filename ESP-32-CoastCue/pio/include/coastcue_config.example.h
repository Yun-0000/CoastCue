#pragma once

// Replace with a stable UUID that identifies this boat in the local CoastCue app.
#define COASTCUE_BOAT_ID "00000000-0000-0000-0000-000000000000"
// Normal position/status cadence for the field MVP.
#define COASTCUE_REPORT_PERIOD_MS 10000UL

// Generate a unique deployment key with: openssl rand -hex 32
// This encodes 32 HMAC key bytes; the deploy-config check requires 64 lowercase hex chars.
#define COASTCUE_LORA_AUTH_KEY "replace-with-a-unique-64-lowercase-hex-character-deployment-key"

// Normal reports include coordinates only when this fresh. Help/SOS reports
// may include the last known fix with its age in seconds.
#define COASTCUE_GPS_FRESHNESS_MS 30000UL

// Keep production serial output event-oriented. Set to 1 only for a bounded
// bench capture that needs raw authenticated packets and periodic GPS chatter.
#ifndef COASTCUE_VERBOSE_SERIAL
#define COASTCUE_VERBOSE_SERIAL 0
#endif

// Bench-only deterministic fault injection. Product profiles leave all three
// at zero; use the explicit *-fault-* PlatformIO environments instead of
// enabling these in a field deployment config.
#ifndef COASTCUE_BENCH_BOAT_PERSIST_FAIL_AFTER_SUCCESSES
#define COASTCUE_BENCH_BOAT_PERSIST_FAIL_AFTER_SUCCESSES 0
#endif
#ifndef COASTCUE_BENCH_BOAT_PERSIST_FAILURE_COUNT
#define COASTCUE_BENCH_BOAT_PERSIST_FAILURE_COUNT 0
#endif
#ifndef COASTCUE_BENCH_SHORE_FAIL_FIRST_OPERATOR_TRANSITION
#define COASTCUE_BENCH_SHORE_FAIL_FIRST_OPERATOR_TRANSITION 0
#endif

// Default ready path: China 433 MHz T-Beam SX1278 boat node + ESP32 DevKit +
// RA-02 shore gateway. PlatformIO envs override these for US 915 MHz builds.
// China envs also override the generic SF/CR fallback below to SF11 and CR 4/8.
// Confirm local radio legality and attach antennas before transmitting outside the lab.
#ifndef COASTCUE_TRANSPORT_LORA
#define COASTCUE_TRANSPORT_LORA 1
#endif

// 1 = SX1278-class 433 MHz module, 0 = SX1276-class module with matching frequency.
#ifndef COASTCUE_LORA_RADIO_SX1278
#define COASTCUE_LORA_RADIO_SX1278 1
#endif

#ifndef COASTCUE_LORA_FREQUENCY_MHZ
#define COASTCUE_LORA_FREQUENCY_MHZ 433.0
#endif
#ifndef COASTCUE_LORA_BANDWIDTH_KHZ
#define COASTCUE_LORA_BANDWIDTH_KHZ 125.0
#endif
#ifndef COASTCUE_LORA_SPREADING_FACTOR
#define COASTCUE_LORA_SPREADING_FACTOR 9
#endif
#ifndef COASTCUE_LORA_CODING_RATE
#define COASTCUE_LORA_CODING_RATE 7
#endif
#ifndef COASTCUE_LORA_SYNC_WORD
#define COASTCUE_LORA_SYNC_WORD 0x12
#endif
#ifndef COASTCUE_LORA_TX_POWER_DBM
#define COASTCUE_LORA_TX_POWER_DBM 10
#endif
#ifndef COASTCUE_LORA_PREAMBLE_LENGTH
#define COASTCUE_LORA_PREAMBLE_LENGTH 8
#endif
// The locked field `boat` profile overrides this to 1 for its wired SSD1306.
// Headless engineering/compatibility profiles leave it at zero.
#ifndef COASTCUE_OLED_ENABLED
#define COASTCUE_OLED_ENABLED 0
#endif

// OLED panel height: 32 for 128x32, 64 for 128x64 SSD1306 modules.
#ifndef COASTCUE_OLED_HEIGHT
#define COASTCUE_OLED_HEIGHT 32
#endif
