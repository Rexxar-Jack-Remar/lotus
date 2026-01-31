//===-- Verification/Sifa/Caches/StarDagCache.tpp ---------------------------===//
//
// Template implementation for StarDagCache.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CACHES_STARDAGCACHE_TPP
#define LOTUS_VERIFICATION_SIFA_CACHES_STARDAGCACHE_TPP

#include "Verification/Sifa/Caches/StarDagCache.h"
#include "Verification/Sifa/RegexDag/RegexDagCompressor.h"
#include "Verification/Sifa/RegexDag/RegexDagUtils.h"
#include "Verification/Sifa/RegexDag/RegexToDag.h"

namespace lotus {
namespace sifa {

template <typename L>
const typename StarDagCache<L>::Dag &StarDagCache<L>::dagOf(const RegexRef &regex) {
  const auto it = cache_.find(regex);
  if (it != cache_.end()) {
    return it->second;
  }
  return cache_.emplace(regex, computeDagOf(regex)).first->second;
}

template <typename L>
typename StarDagCache<L>::Dag StarDagCache<L>::computeDagOf(const RegexRef &regex) {
  RegexToDag<L> r2d;
  const auto marked = markRegex(regex, /*finalLocationAsMark=*/nullptr, nextMarkerId_++);
  r2d.add(marked);
  Dag dag = r2d.getDagAndReset();
  RegexDagCompressor<L> comp;
  comp.compress(dag);
  (void)stats_;
  return dag;
}

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_CACHES_STARDAGCACHE_TPP
