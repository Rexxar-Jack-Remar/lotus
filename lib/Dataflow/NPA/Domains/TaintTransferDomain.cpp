/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Domains/TaintTransferDomain.h"

namespace npa {

thread_local unsigned TaintTransferDomain::BitWidth = 1;

std::vector<llvm::APInt> TaintTransferDomain::identityRel() {
  std::vector<llvm::APInt> rel;
  rel.reserve(BitWidth);
  for (unsigned i = 0; i < BitWidth; ++i) {
    llvm::APInt row(BitWidth, 0);
    row.setBit(i);
    rel.push_back(row);
  }
  return rel;
}

TaintTransferDomain::value_type TaintTransferDomain::zero() {
  value_type out;
  out.rel.assign(BitWidth, llvm::APInt(BitWidth, 0));
  out.gen = llvm::APInt(BitWidth, 0);
  return out;
}

TaintTransferDomain::value_type TaintTransferDomain::one() {
  value_type out;
  out.rel = identityRel();
  out.gen = llvm::APInt(BitWidth, 0);
  return out;
}

bool TaintTransferDomain::equal(const value_type &a, const value_type &b) {
  if (a.gen != b.gen || a.rel.size() != b.rel.size())
    return false;
  for (unsigned i = 0; i < a.rel.size(); ++i) {
    if (a.rel[i] != b.rel[i])
      return false;
  }
  return true;
}

TaintTransferDomain::value_type
TaintTransferDomain::combine(const value_type &a, const value_type &b) {
  value_type out;
  out.rel.resize(BitWidth, llvm::APInt(BitWidth, 0));
  for (unsigned i = 0; i < BitWidth; ++i) {
    out.rel[i] = a.rel[i] | b.rel[i];
  }
  out.gen = a.gen | b.gen;
  return out;
}

TaintTransferDomain::value_type
TaintTransferDomain::ndetCombine(const value_type &a, const value_type &b) {
  return combine(a, b);
}

TaintTransferDomain::value_type
TaintTransferDomain::condCombine(bool phi, const value_type &t,
                                 const value_type &e) {
  return phi ? t : e;
}

llvm::APInt TaintTransferDomain::applyRel(const std::vector<llvm::APInt> &rel,
                                          const llvm::APInt &in) {
  llvm::APInt out(BitWidth, 0);
  for (unsigned i = 0; i < BitWidth; ++i) {
    if (in[i])
      out |= rel[i];
  }
  return out;
}

TaintTransferDomain::value_type
TaintTransferDomain::extend(const value_type &a, const value_type &b) {
  // a after b: a o b
  value_type out;
  out.rel.resize(BitWidth, llvm::APInt(BitWidth, 0));
  for (unsigned i = 0; i < BitWidth; ++i) {
    out.rel[i] = applyRel(a.rel, b.rel[i]);
  }
  out.gen = applyRel(a.rel, b.gen) | a.gen;
  return out;
}

TaintTransferDomain::value_type
TaintTransferDomain::extend_lin(const value_type &a, const value_type &b) {
  return extend(a, b);
}

TaintTransferDomain::value_type
TaintTransferDomain::subtract(const value_type &a, const value_type &b) {
  (void)b;
  return a;
}

llvm::APInt TaintTransferDomain::apply(const value_type &f,
                                       const llvm::APInt &in) {
  return applyRel(f.rel, in) | f.gen;
}

void TaintTransferDomain::addEdge(value_type &f, unsigned from, unsigned to) {
  if (from >= BitWidth || to >= BitWidth)
    return;
  f.rel[from].setBit(to);
}

void TaintTransferDomain::addGen(value_type &f, unsigned bit) {
  if (bit >= BitWidth)
    return;
  f.gen.setBit(bit);
}

} // namespace npa
