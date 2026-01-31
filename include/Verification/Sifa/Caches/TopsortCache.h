//===-- Verification/Sifa/Caches/TopsortCache.h ---------------------------===//
//
// Topological sort cache for RegexDags (ported from Ultimate Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_H
#define LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_H

#include "Verification/Sifa/RegexDag/RegexDag.h"

#include <unordered_map>
#include <vector>

namespace lotus {
namespace sifa {

template <typename L>
class TopsortCache final {
public:
  using Dag = RegexDag<L>;
  using Node = RegexDagNode<L>;

  std::vector<Node *> topsort(const Dag &dag);

private:
  std::vector<Node *> compute(const Dag &dag);

  std::unordered_map<const Dag *, std::vector<Node *>> cache_;
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/Caches/TopsortCache.tpp"

#endif // LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_H
