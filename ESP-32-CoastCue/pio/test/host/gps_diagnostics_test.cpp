#include <cassert>
#include <cstdint>
#include <cstring>

#include "gps_diagnostics.h"

namespace {

void feed(coastcue::GpsStreamDiagnostics &diagnostics, const char *bytes,
          uint32_t &now) {
  while (*bytes != '\0') {
    diagnostics.feed(static_cast<uint8_t>(*bytes++), now++);
  }
}

void feedUbx(coastcue::GpsStreamDiagnostics &diagnostics,
             const uint8_t *bytes, size_t size, uint32_t &now) {
  for (size_t i = 0; i < size; ++i) diagnostics.feed(bytes[i], now++);
}

}  // namespace

int main() {
  coastcue::GpsStreamDiagnostics diagnostics;
  uint32_t now = 1;

  // Valid GSV: eight satellites are visible and the strongest C/N0 is 42 dB-Hz.
  feed(diagnostics,
       "$GPGSV,2,1,08,01,40,083,42,02,17,273,35,03,09,123,18,04,05,312,\n",
       now);
  // The deliberately missing checksum must not be accepted.
  assert(diagnostics.snapshot(now).nmeaPassed == 0);

  diagnostics.reset();
  feed(diagnostics,
       "$GPGSV,2,1,08,01,40,083,42,02,17,273,35,03,09,123,18,04,05,312,20*7E\r\n",
       now);
  feed(diagnostics,
       "$GPGGA,123519,4807.038,N,01131.000,E,0,00,99.99,545.4,M,46.9,M,,*47\r\n",
       now);
  auto snapshot = diagnostics.snapshot(now);
  assert(snapshot.nmeaPassed == 2);
  assert(snapshot.nmeaFailed == 0);
  assert(snapshot.gsvSentences == 1);
  assert(snapshot.visibleSatellites == 8);
  assert(snapshot.minVisibleSatellites == 8);
  assert(snapshot.maxCn0DbHz == 42);
  assert(snapshot.minGsvMaxCn0DbHz == 42);
  assert(snapshot.usedSatellites == 0);
  assert(snapshot.fixQuality == 0);
  assert(snapshot.protocol == coastcue::GpsProtocol::kNmea);

  // A checksum error is counted separately instead of being called GPS data.
  feed(diagnostics,
       "$GPGGA,123519,4807.038,N,01131.000,E,1,05,1.20,545.4,M,46.9,M,,*00\r\n",
       now);
  snapshot = diagnostics.snapshot(now);
  assert(snapshot.nmeaPassed == 2);
  assert(snapshot.nmeaFailed == 1);

  // A false UBX prefix immediately before '$' must not hide the NMEA frame.
  diagnostics.reset();
  diagnostics.feed(0xB5, now++);
  feed(diagnostics,
       "$GPGSV,1,1,02,01,40,083,15,02,17,273,12*70\r\n", now);
  snapshot = diagnostics.snapshot(now);
  assert(snapshot.nmeaPassed == 1);
  assert(snapshot.unknownBytes == 1);
  assert(snapshot.minVisibleSatellites == 2);
  assert(snapshot.minGsvMaxCn0DbHz == 15);

  // Minimal valid UBX frame: class 0x01, id 0x02, zero-length payload.
  diagnostics.reset();
  const uint8_t ubx[] = {0xB5, 0x62, 0x01, 0x02, 0x00, 0x00, 0x03, 0x0A};
  feedUbx(diagnostics, ubx, sizeof(ubx), now);
  snapshot = diagnostics.snapshot(now);
  assert(snapshot.ubxPassed == 1);
  assert(snapshot.protocol == coastcue::GpsProtocol::kUbx);

  using coastcue::GpsDiagnosticPhase;
  assert(coastcue::gpsDiagnosticPhaseForElapsed(0) ==
         GpsDiagnosticPhase::kQuiet);
  assert(coastcue::gpsDiagnosticPhaseForElapsed(179999) ==
         GpsDiagnosticPhase::kQuiet);
  assert(coastcue::gpsDiagnosticPhaseForElapsed(180000) ==
         GpsDiagnosticPhase::kActive);
  assert(coastcue::gpsDiagnosticPhaseForElapsed(359999) ==
         GpsDiagnosticPhase::kActive);
  assert(coastcue::gpsDiagnosticPhaseForElapsed(360000) ==
         GpsDiagnosticPhase::kConfirm);
  assert(coastcue::gpsDiagnosticPhaseForElapsed(599999) ==
         GpsDiagnosticPhase::kConfirm);
  assert(coastcue::gpsDiagnosticPhaseForElapsed(600000) ==
         GpsDiagnosticPhase::kDone);

  coastcue::GpsDiagnosticSnapshot quiet{};
  quiet.nmeaPassed = 100;
  quiet.gsvSentences = 20;
  quiet.visibleSatellites = 7;
  quiet.minVisibleSatellites = 7;
  quiet.maxCn0DbHz = 38;
  quiet.minGsvMaxCn0DbHz = 35;
  quiet.fixQuality = 1;
  coastcue::GpsDiagnosticSnapshot active = quiet;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(quiet, active, active,
                                                     true, 3300),
                     "FIX_OK") == 0);

  active.visibleSatellites = 0;
  active.minVisibleSatellites = 0;
  active.maxCn0DbHz = 0;
  active.minGsvMaxCn0DbHz = 0;
  active.fixQuality = 0;
  active.nmeaFailed = 30;
  coastcue::GpsDiagnosticSnapshot confirm = active;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(quiet, active, confirm,
                                                     true, 3300),
                     "LORA_POWER_OR_EMI") == 0);

  coastcue::GpsDiagnosticSnapshot noGsv{};
  noGsv.nmeaPassed = 50;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(noGsv, noGsv, noGsv,
                                                     true, 3300),
                     "GSV_DISABLED_OR_GPS_CONFIG") == 0);

  coastcue::GpsDiagnosticSnapshot noSignal = noGsv;
  noSignal.gsvSentences = 10;
  noSignal.visibleSatellites = 0;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(noSignal, noSignal,
                                                     noSignal, true, 3300),
                     "GPS_ANTENNA_OR_RF_PATH") == 0);

  // A mid-phase collapse must remain visible even if the final sample recovers.
  coastcue::GpsDiagnosticSnapshot recovered = quiet;
  recovered.fixQuality = 0;
  recovered.minVisibleSatellites = 0;
  recovered.minGsvMaxCn0DbHz = 5;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(
                         quiet, recovered, quiet, true, 3300),
                     "LORA_POWER_OR_EMI") == 0);

  // Sparse reception in every phase is not evidence that LoRa caused it.
  coastcue::GpsDiagnosticSnapshot sparseQuiet{};
  sparseQuiet.nmeaPassed = 400;
  sparseQuiet.gsvSentences = 80;
  sparseQuiet.visibleSatellites = 2;
  sparseQuiet.minVisibleSatellites = 0;
  sparseQuiet.maxCn0DbHz = 29;
  sparseQuiet.minGsvMaxCn0DbHz = 18;
  coastcue::GpsDiagnosticSnapshot sparseActive = sparseQuiet;
  sparseActive.nmeaPassed = 500;
  sparseActive.visibleSatellites = 1;
  coastcue::GpsDiagnosticSnapshot sparseConfirm = sparseQuiet;
  sparseConfirm.nmeaPassed = 540;
  sparseConfirm.visibleSatellites = 0;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(
                         sparseQuiet, sparseActive, sparseConfirm, true, 3300),
                     "SATELLITES_VISIBLE_NO_FIX") == 0);

  const char fixCode = coastcue::gpsDiagnosticClassificationCode("FIX_OK");
  assert(fixCode == 'F');
  assert(std::strcmp(coastcue::gpsDiagnosticClassificationName(fixCode),
                     "FIX_OK") == 0);
  const char antennaCode = coastcue::gpsDiagnosticClassificationCode(
      "GPS_ANTENNA_OR_RF_PATH");
  assert(antennaCode == 'A');
  assert(std::strcmp(coastcue::gpsDiagnosticClassificationName(antennaCode),
                     "GPS_ANTENNA_OR_RF_PATH") == 0);

  coastcue::GpsDiagnosticSnapshot noData{};
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(noData, noData, noData,
                                                     true, 3300),
                     "NO_GPS_UART_DATA") == 0);

  coastcue::GpsDiagnosticSnapshot wrongProtocol{};
  wrongProtocol.bytes = 100;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(
                         wrongProtocol, wrongProtocol, wrongProtocol, true, 3300),
                     "GPS_PROTOCOL_OR_BAUD") == 0);

  coastcue::GpsDiagnosticSnapshot ubxOnly{};
  ubxOnly.bytes = 8;
  ubxOnly.ubxPassed = 1;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(ubxOnly, ubxOnly, ubxOnly,
                                                     true, 3300),
                     "UBX_ONLY_OR_NMEA_DISABLED") == 0);

  coastcue::GpsDiagnosticSnapshot corrupt{};
  corrupt.bytes = 1000;
  corrupt.nmeaPassed = 2;
  corrupt.nmeaFailed = 20;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(corrupt, corrupt, corrupt,
                                                     true, 3300),
                     "NMEA_CORRUPT_OR_UART_NOISE") == 0);

  coastcue::GpsDiagnosticSnapshot weak = noSignal;
  weak.visibleSatellites = 3;
  weak.maxCn0DbHz = 15;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(weak, weak, weak, true,
                                                     3300),
                     "WEAK_GPS_SIGNAL") == 0);

  coastcue::GpsDiagnosticSnapshot visible = weak;
  visible.visibleSatellites = 7;
  visible.maxCn0DbHz = 35;
  assert(std::strcmp(coastcue::classifyGpsDiagnostic(visible, visible, visible,
                                                     true, 3300),
                     "SATELLITES_VISIBLE_NO_FIX") == 0);

  assert(std::strcmp(coastcue::classifyGpsDiagnostic(visible, visible, visible,
                                                     false, 0),
                     "GPS_POWER_RAIL_CONFIG") == 0);
}
