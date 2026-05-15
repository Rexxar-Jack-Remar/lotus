#include "Verification/SymAbsAI/Core/Foundation/RepresentedValue.h"

#include "Verification/SymAbsAI/Core/Foundation/Repr.h"

#include <iostream>

namespace symabs_ai {
std::ostream &operator<<(std::ostream &out, const RepresentedValue &value) {
  return out << repr(value);
}
} // namespace symabs_ai
