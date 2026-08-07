#ifndef NPA_CORE_SYMBOL_H
#define NPA_CORE_SYMBOL_H

#include <cstddef>
#include <functional>
#include <string>

namespace npa {

using Symbol = std::string;

template <class T> inline void hash_combine(std::size_t &hash, const T &value) {
  hash ^= std::hash<T>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
}

} // namespace npa

#endif // NPA_CORE_SYMBOL_H
