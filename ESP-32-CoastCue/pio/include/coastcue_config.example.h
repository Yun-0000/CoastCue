#pragma once

// Replace with a stable UUID that identifies this boat in the local CoastCue app.
#define COASTCUE_BOAT_ID "00000000-0000-0000-0000-000000000000"
// Normal position/status cadence.
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
