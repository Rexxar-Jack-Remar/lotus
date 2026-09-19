#include "Concurrency/Runtime/APIRegistry.h"

namespace concurrency::runtime {

APIRegistry& APIRegistry::get() {
  static APIRegistry instance;
  return instance;
}

void APIRegistry::registerAPI(const std::string& name, APIDescription description) {
  m_apis[name] = std::move(description);
}

const APIDescription* APIRegistry::lookup(const std::string& name) const {
  auto it = m_apis.find(name);
  if (it == m_apis.end()) {
    return nullptr;
  }
  return &it->second;
}

bool APIRegistry::hasAPI(const std::string& name) const {
  return m_apis.find(name) != m_apis.end();
}

std::vector<std::string> APIRegistry::names() const {
  std::vector<std::string> result;
  result.reserve(m_apis.size());
  for (const auto& entry : m_apis) {
    result.push_back(entry.first);
  }
  return result;
}

} // namespace concurrency::runtime