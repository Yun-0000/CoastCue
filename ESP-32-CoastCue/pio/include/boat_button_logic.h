#pragma once

#include <stdint.h>

namespace coastcue {

constexpr uint32_t kBoatSosHoldMs = 3000;
constexpr uint32_t kBoatHelpMinHoldMs = 250;

enum class BoatButtonEvent : uint8_t { kNone, kHelp, kSos };

constexpr bool shouldFireBoatSosWhileHeld(bool pressed, bool alreadyFired,
                                          uint32_t heldMs) {
  return pressed && !alreadyFired && heldMs >= kBoatSosHoldMs;
}

constexpr BoatButtonEvent classifyBoatButtonRelease(uint32_t heldMs) {
  return heldMs >= kBoatSosHoldMs ? BoatButtonEvent::kSos
         : heldMs >= kBoatHelpMinHoldMs ? BoatButtonEvent::kHelp
                                        : BoatButtonEvent::kNone;
}

}  // namespace coastcue
