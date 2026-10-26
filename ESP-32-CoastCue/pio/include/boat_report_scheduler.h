#pragma once

#include <cstdint>

class BoatReportScheduler {
 public:
  bool normalReportDue(uint32_t now, uint32_t reportPeriodMs,
                       bool gpsFresh) const {
    return !normalReportInFlight_ &&
           ((gpsFresh && !hasSentFreshGpsReport_) ||
            now - lastNormalReportAt_ >= reportPeriodMs);
  }

  void normalReportStarted(bool gpsFresh) {
    normalReportInFlight_ = true;
    pendingFreshGpsReport_ = gpsFresh;
  }

  void scheduleFirstPeriodicReport(uint32_t now, uint32_t reportPeriodMs) {
    lastNormalReportAt_ = now - reportPeriodMs;
  }

  void normalReportFinished(bool success, uint32_t now) {
    normalReportInFlight_ = false;
    if (success) {
      lastNormalReportAt_ = now;
      hasSentFreshGpsReport_ = hasSentFreshGpsReport_ || pendingFreshGpsReport_;
    }
    pendingFreshGpsReport_ = false;
  }

  bool hasSentFreshGpsReport() const { return hasSentFreshGpsReport_; }
  bool normalReportInFlight() const { return normalReportInFlight_; }

 private:
  bool normalReportInFlight_ = false;
  bool pendingFreshGpsReport_ = false;
  bool hasSentFreshGpsReport_ = false;
  uint32_t lastNormalReportAt_ = 0;
};
