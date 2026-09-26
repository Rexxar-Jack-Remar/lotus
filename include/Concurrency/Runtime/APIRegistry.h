#pragma once
#include "Concurrency/Runtime/RuntimeKind.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace concurrency::runtime {
  using Traits = std::vector<std::string>;
  struct ArgumentLayout {
      // empty for now, we'll populate later
  };
  struct APIDescription {
      RuntimeKind runtime;
      std::string semanticTag;
      Traits traits;
      ArgumentLayout arguments;
  };

  class APIRegistry {
  public:
      static APIRegistry& get();
      // Add methods for registering and looking up APIDescriptions by name.
      void registerAPI(const std::string& name, APIDescription description);
      const APIDescription* lookup(const std::string& name) const;
      bool hasAPI(const std::string& name) const;
      std::vector<std::string> names() const;

  private:
      APIRegistry() = default;
      std::unordered_map<std::string, APIDescription> m_apis;
  };
} // namespace concurrency::runtime