//===-- Verification/Sifa/Statistics/IStatsProvider.h ---------------------===//
//
// Stats provider interface (Ultimate-aligned).
//
// Ultimate's IStatsProvider returns IStatisticsDataProvider.getStats().
// In lotus we expose SifaStats.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_STATISTICS_ISTATSPROVIDER_H
#define LOTUS_VERIFICATION_SIFA_STATISTICS_ISTATSPROVIDER_H

#include "Verification/Sifa/Statistics/SifaStats.h"

namespace lotus {
namespace sifa {

class IStatsProvider {
public:
  virtual ~IStatsProvider() = default;
  virtual SifaStats &getStats() = 0;
  virtual const SifaStats &getStats() const = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_STATISTICS_ISTATSPROVIDER_H
