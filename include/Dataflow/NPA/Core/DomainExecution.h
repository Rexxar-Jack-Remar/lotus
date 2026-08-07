#ifndef NPA_CORE_DOMAIN_EXECUTION_H
#define NPA_CORE_DOMAIN_EXECUTION_H

#include <cassert>

namespace npa {

struct NoopDomainRunState {};

struct NoopDomainRunStateScope {
  explicit NoopDomainRunStateScope(const NoopDomainRunState &) {}
};

template <class D> struct DomainExecutionStateTraits {
  using state_type = NoopDomainRunState;
  using scope_type = NoopDomainRunStateScope;

  static state_type capture() { return {}; }
};

/// Domain-owned dynamic state used while constructing width-dependent values.
template <class Tag> class DomainWidthContext {
public:
  struct state_type {
    bool active = false;
    unsigned bit_width = 1;
  };

  class scope_type {
  public:
    scope_type() = default;
    explicit scope_type(unsigned bit_width)
        : scope_type(state_type{true, bit_width}) {}
    explicit scope_type(const state_type &state) { reset(state); }

    scope_type(const scope_type &) = delete;
    scope_type &operator=(const scope_type &) = delete;

    scope_type(scope_type &&other) noexcept
        : previous_width_(other.previous_width_),
          previous_active_(other.previous_active_),
          installed_(other.installed_) {
      other.installed_ = false;
    }

    scope_type &operator=(scope_type &&other) noexcept {
      if (this == &other)
        return *this;
      restore();
      previous_width_ = other.previous_width_;
      previous_active_ = other.previous_active_;
      installed_ = other.installed_;
      other.installed_ = false;
      return *this;
    }

    ~scope_type() { restore(); }

    void reset(unsigned bit_width) { reset(state_type{true, bit_width}); }

    void reset(const state_type &state) {
      restore();
      previous_width_ = current_bit_width_slot();
      previous_active_ = has_current_bit_width_slot();
      installed_ = true;
      if (state.active) {
        current_bit_width_slot() = state.bit_width;
        has_current_bit_width_slot() = true;
      } else {
        current_bit_width_slot() = 1;
        has_current_bit_width_slot() = false;
      }
    }

  private:
    void restore() {
      if (!installed_)
        return;
      current_bit_width_slot() = previous_width_;
      has_current_bit_width_slot() = previous_active_;
      installed_ = false;
    }

    unsigned previous_width_ = 1;
    bool previous_active_ = false;
    bool installed_ = false;
  };

  static state_type capture() {
    return state_type{has_current_bit_width_slot(), current_bit_width_slot()};
  }

  static unsigned require(const char *message) {
    assert(has_current_bit_width_slot() && message);
    return current_bit_width_slot();
  }

private:
  static unsigned &current_bit_width_slot() {
    static thread_local unsigned width = 1;
    return width;
  }

  static bool &has_current_bit_width_slot() {
    static thread_local bool active = false;
    return active;
  }
};

} // namespace npa

#endif // NPA_CORE_DOMAIN_EXECUTION_H
