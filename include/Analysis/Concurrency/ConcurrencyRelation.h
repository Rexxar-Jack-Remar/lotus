#pragma once

#include <string>

namespace concurrency {

enum class ProofStrength {
  Must,
  May,
  Unknown
};

enum class RelationKind {
  MustHappenBefore,
  MayHappenBefore,
  MutuallyExclusive,
  UnknownDueToModelGap
};

struct Relation {
  RelationKind kind = RelationKind::UnknownDueToModelGap;
  ProofStrength proof = ProofStrength::Unknown;
  std::string reason;
};

inline int relationPriority(RelationKind kind) {
  switch (kind) {
  case RelationKind::MustHappenBefore:
    return 4;
  case RelationKind::MutuallyExclusive:
    return 3;
  case RelationKind::MayHappenBefore:
    return 2;
  case RelationKind::UnknownDueToModelGap:
    return 1;
  }
  return 0;
}

} // namespace concurrency
