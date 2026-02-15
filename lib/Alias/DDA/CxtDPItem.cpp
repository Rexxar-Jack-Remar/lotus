//===- CxtDPItem.cpp -- Context-sensitive DP item impl --------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/CxtDPItem.h"

#include <sstream>

namespace lotus {
namespace analysis {

uint32_t ContextCond::maximumCxtLen = 3u;
uint32_t ContextCond::maximumPathLen = 0u;
uint32_t ContextCond::maxCxtLenSeen = 0u;

void ContextCond::updateMaxCxtLenSeen(size_t len) {
  if (len > maxCxtLenSeen)
    maxCxtLenSeen = static_cast<uint32_t>(len);
}

// Matches SVF DPItem.h pushContext: when at maximumCxtLen, set non-concrete,
// drop front (FIFO), push back, return false. Empty check guards maxCxtLen==0.
bool ContextCond::pushContext(uint32_t ctx) {
  if (context_.size() < maximumCxtLen) {
    context_.push_back(ctx);
    updateMaxCxtLenSeen(context_.size());
    return true;
  }
  if (!context_.empty()) {
    setNonConcreteCxt();
    context_.erase(context_.begin());
    context_.push_back(ctx);
    updateMaxCxtLenSeen(context_.size());
  }
  return false;
}

bool ContextCond::matchContext(uint32_t ctx) {
  if (context_.empty())
    return true;
  if (context_.back() == ctx) {
    context_.pop_back();
    return true;
  }
  return false;
}

void ContextCond::popBack() {
  if (!context_.empty())
    context_.pop_back();
}

std::string ContextCond::toString() const {
  std::ostringstream os;
  os << "[:";
  for (size_t i = 0; i < context_.size(); ++i)
    os << (i ? " " : "") << context_[i];
  os << " ]";
  return os.str();
}

} // namespace analysis
} // namespace lotus
