//===-- Verification/Sifa/Worklist/IWorklistWithInputs.h ------------------===//
//
// Worklist interface (ported from Ultimate Library-Sifa).
//
// Stores pairs (work, input) and provides an iteration protocol via advance().
//
//===----------------------------------------------------------------------===//

#pragma once

namespace lotus {
namespace sifa {

template <typename W, typename I> class IWorklistWithInputs {
public:
  virtual ~IWorklistWithInputs() = default;

  virtual void add(W work, I input) = 0;
  virtual bool advance() = 0;
  virtual W getWork() const = 0;
  virtual I getInput() const = 0;
};

} // namespace sifa
} // namespace lotus

