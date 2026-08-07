#ifndef NPA_LLVM_CALL_RESOLUTION_H
#define NPA_LLVM_CALL_RESOLUTION_H

namespace npa {

enum class IndirectCallResolutionMode {
  ClosedWorldTypeCompatible,
  DeclaredOnlyFallback,
  CustomResolverRequired,
};

} // namespace npa

#endif // NPA_LLVM_CALL_RESOLUTION_H
