//===-- Verification/Sifa/Cfg/Transition.cpp ------------------------------===//
//
// Non-inline definitions for Transition (ostream, etc.).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Cfg/Transition.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <cstddef>
#include <ostream>
#include <string>

namespace lotus {
namespace sifa {

std::string CallReturnSummary::calledProcedure() const {
  return callee ? callee->getName().str() : std::string();
}

Transition Transition::makeEdge(std::uint32_t id, const llvm::BasicBlock *src,
                               const llvm::BasicBlock *dst) {
  Transition t;
  t.kind = TransitionKind::Edge;
  t.id = id;
  t.source = const_cast<llvm::BasicBlock *>(src);
  t.target = const_cast<llvm::BasicBlock *>(dst);
  t.callee = nullptr;
  return t;
}

Transition Transition::makeMarker(std::uint32_t id, const llvm::BasicBlock *markedTarget) {
  Transition t;
  t.kind = TransitionKind::Marker;
  t.id = id;
  t.source = nullptr;
  t.target = const_cast<llvm::BasicBlock *>(markedTarget);
  t.callee = nullptr;
  return t;
}

Transition Transition::makeReturnSummary(std::uint32_t id, const llvm::BasicBlock *src,
                                         const llvm::BasicBlock *dst,
                                         const llvm::Function *calleeFn) {
  Transition t;
  t.kind = TransitionKind::ReturnSummary;
  t.id = id;
  t.source = const_cast<llvm::BasicBlock *>(src);
  t.target = const_cast<llvm::BasicBlock *>(dst);
  t.callee = const_cast<llvm::Function *>(calleeFn);
  return t;
}

Transition Transition::from(const LocationMarkerTransition &m) {
  return makeMarker(m.uniqueId, m.markedTarget);
}

Transition Transition::from(const CallReturnSummary &c) {
  return makeReturnSummary(c.id, c.source, c.target, c.callee);
}

llvm::Optional<LocationMarkerTransition> Transition::getLocationMarkerTransition() const {
  if (kind != TransitionKind::Marker)
    return llvm::None;
  LocationMarkerTransition m;
  m.markedTarget = target;
  m.uniqueId = id;
  return m;
}

llvm::Optional<CallReturnSummary> Transition::getCallReturnSummary() const {
  if (kind != TransitionKind::ReturnSummary)
    return llvm::None;
  CallReturnSummary c;
  c.source = source;
  c.target = target;
  c.callee = callee;
  c.id = id;
  return c;
}

bool Transition::operator==(const Transition &o) const {
  return kind == o.kind && id == o.id && callee == o.callee;
}

std::size_t hashValue(const Transition &t) {
  const std::size_t a = static_cast<std::size_t>(t.id);
  const std::size_t b = static_cast<std::size_t>(t.kind);
  const std::size_t c = std::hash<llvm::Function *>()(t.callee);
  return (a << 1) ^ b ^ (c << 2);
}

std::ostream &operator<<(std::ostream &os, const Transition &t) {
  switch (t.kind) {
  case TransitionKind::Edge:
    os << "t" << t.id;
    break;
  case TransitionKind::Marker:
    os << "※" << t.id;
    break;
  case TransitionKind::ReturnSummary:
    os << "ret@" << t.id;
    if (t.callee) {
      os << "(" << t.callee->getName().str() << ")";
    }
    break;
  }
  return os;
}

} // namespace sifa
} // namespace lotus
