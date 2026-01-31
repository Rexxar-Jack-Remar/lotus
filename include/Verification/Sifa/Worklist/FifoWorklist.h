//===-- Verification/Sifa/Worklist/FifoWorklist.h -------------------------===//
//
// FIFO worklist with merge-on-reinsert (ported from Ultimate Library-Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_WORKLIST_FIFOWORKLIST_H
#define LOTUS_VERIFICATION_SIFA_WORKLIST_FIFOWORKLIST_H

#include "Verification/Sifa/Worklist/IWorklistWithInputs.h"

#include <functional>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace lotus {
namespace sifa {

template <typename W, typename I>
class FifoWorklist final : public IWorklistWithInputs<W, I> {
public:
  using MergeFn = std::function<I(const I &, const I &)>;

  explicit FifoWorklist(MergeFn merge) : merge_(std::move(merge)) {}

  void add(W work, I addInput) override {
    auto it = items_.find(work);
    if (it != items_.end()) {
      it->second.input = merge_(it->second.input, addInput);
      return;
    }
    order_.push_back(work);
    items_.emplace(work, Item{std::move(addInput)});
  }

  bool advance() override {
    if (order_.empty()) {
      hasCurrent_ = false;
      return false;
    }
    currentWork_ = order_.front();
    order_.pop_front();
    auto it = items_.find(currentWork_);
    currentInput_ = it->second.input;
    items_.erase(it);
    hasCurrent_ = true;
    return true;
  }

  W getWork() const override {
    ensureAdvanced();
    return currentWork_;
  }

  I getInput() const override {
    ensureAdvanced();
    return currentInput_;
  }

  /// Ultimate-aligned: toString() — string representation (e.g. for logging).
  std::string toString() const {
    std::size_t n = order_.size() + (hasCurrent_ ? 1u : 0u);
    return "FifoWorklist(size=" + std::to_string(n) + ")";
  }

private:
  struct Item {
    I input;
  };

  void ensureAdvanced() const {
    if (!hasCurrent_) {
      throw std::logic_error("Never called advance() on this worklist.");
    }
  }

  MergeFn merge_;
  std::list<W> order_;
  std::unordered_map<W, Item> items_;

  bool hasCurrent_ = false;
  W currentWork_{};
  I currentInput_{};
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_WORKLIST_FIFOWORKLIST_H
