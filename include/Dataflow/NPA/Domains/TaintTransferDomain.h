#ifndef NPA_TAINT_TRANSFER_DOMAIN_H
#define NPA_TAINT_TRANSFER_DOMAIN_H

#include "Utils/LLVM/SystemHeaders.h"

#include <vector>

#include <llvm/ADT/APInt.h>

namespace npa {

struct TaintTransfer {
  TaintTransfer() : rel(), gen(1, 0) {}
  std::vector<llvm::APInt> rel;
  llvm::APInt gen;
};

class TaintTransferDomain {
public:
  using value_type = TaintTransfer;
  using test_type = bool;
  static constexpr bool idempotent = true;

  static void setBitWidth(unsigned W) { BitWidth = W; }
  static unsigned getBitWidth() { return BitWidth; }

  static value_type zero();
  static value_type one();
  static bool equal(const value_type &a, const value_type &b);
  static value_type combine(const value_type &a, const value_type &b);
  static value_type ndetCombine(const value_type &a, const value_type &b);
  static value_type condCombine(bool /*phi*/, const value_type &t,
                                const value_type &e);
  static value_type extend(const value_type &a, const value_type &b);
  static value_type extend_lin(const value_type &a, const value_type &b);
  static value_type subtract(const value_type &a, const value_type &b);

  static llvm::APInt apply(const value_type &f, const llvm::APInt &in);

  static void addEdge(value_type &f, unsigned from, unsigned to);
  static void addGen(value_type &f, unsigned bit);

private:
  static llvm::APInt applyRel(const std::vector<llvm::APInt> &rel,
                              const llvm::APInt &in);
  static std::vector<llvm::APInt> identityRel();
  static unsigned BitWidth;
};

} // namespace npa

#endif // NPA_TAINT_TRANSFER_DOMAIN_H
