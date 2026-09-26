#pragma once

#include <algorithm>

namespace elimination::order {

inline double normalize(double Value, double Cap) {
  return std::min(Value / Cap, 1.0);
}

} // namespace elimination::order
