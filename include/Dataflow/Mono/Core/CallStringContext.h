#ifndef ANALYSIS_MONO_CORE_CALLSTRING_CONTEXT_H_
#define ANALYSIS_MONO_CORE_CALLSTRING_CONTEXT_H_

#include "llvm/ADT/Hashing.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

namespace mono {

template <typename N, unsigned K> class CallStringCTX {
protected:
  std::deque<N> CallString;
  static constexpr unsigned KLimit = K;
  friend struct std::hash<mono::CallStringCTX<N, K>>;

public:
  CallStringCTX() = default;

  CallStringCTX(std::initializer_list<N> IList) : CallString(IList) {
    if (IList.size() > KLimit) {
      throw std::runtime_error(
          "initial call string length exceeds maximal length K");
    }
  }

  void push_back(N Stmt) { // NOLINT
    if (CallString.size() > KLimit - 1) {
      CallString.pop_front();
    }
    CallString.push_back(Stmt);
  }

  N pop_back() { // NOLINT
    if (!CallString.empty()) {
      N Stmt = CallString.back();
      CallString.pop_back();
      return Stmt;
    }
    return N{};
  }

  bool isEqual(const CallStringCTX &Rhs) const {
    return CallString == Rhs.CallString;
  }

  bool isDifferent(const CallStringCTX &Rhs) const {
    return !isEqual(Rhs);
  }

  friend bool operator==(const CallStringCTX &Lhs, const CallStringCTX &Rhs) {
    return Lhs.isEqual(Rhs);
  }

  friend bool operator!=(const CallStringCTX &Lhs, const CallStringCTX &Rhs) {
    return !Lhs.isEqual(Rhs);
  }

  friend bool operator<(const CallStringCTX &Lhs, const CallStringCTX &Rhs) {
    return Lhs.CallString < Rhs.CallString;
  }

  llvm::raw_ostream &print(llvm::raw_ostream &OS) const {
    OS << "Call string: [ ";
    bool First = true;
    for (auto C : CallString) {
      if (!First) {
        OS << " * ";
      }
      First = false;
      printElement(OS, C);
    }
    return OS << " ]";
  }

  bool empty() const { return CallString.empty(); }

  std::size_t size() const { return CallString.size(); }

private:
  template <typename T>
  static typename std::enable_if<
      std::is_pointer<T>::value &&
          std::is_base_of<llvm::Value, typename std::remove_pointer<T>::type>::value,
      void>::type
  printElement(llvm::raw_ostream &OS, T V) {
    if (V != nullptr) {
      OS << *V;
    } else {
      OS << "<null>";
    }
  }

  template <typename T>
  static typename std::enable_if<
      !(std::is_pointer<T>::value &&
        std::is_base_of<llvm::Value, typename std::remove_pointer<T>::type>::value),
      void>::type
  printElement(llvm::raw_ostream &OS, const T &) {
    OS << "<elem>";
  }
};

} // namespace mono

namespace std {

template <typename N, unsigned K> struct hash<mono::CallStringCTX<N, K>> {
  size_t operator()(const mono::CallStringCTX<N, K> &CS) const noexcept {
    llvm::hash_code CallStringHash =
        llvm::hash_combine_range(CS.CallString.begin(), CS.CallString.end());
    return static_cast<size_t>(llvm::hash_combine(K, CallStringHash));
  }
};

} // namespace std

#endif // ANALYSIS_MONO_CORE_CALLSTRING_CONTEXT_H_
