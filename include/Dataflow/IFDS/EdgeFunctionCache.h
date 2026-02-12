/*
 * Edge Function Cache
 *
 * Provides singleton/deduplication caching for edge functions to reduce
 * memory usage and improve composition performance.
 *
 * Based on Phasar's EdgeFunctionCache design.
 */

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ifds {

// ============================================================================
// Edge Function Cache Statistics
// ============================================================================

struct EdgeFunctionCacheStats {
  size_t singleton_cache_hits = 0;
  size_t singleton_cache_misses = 0;
  size_t compose_cache_hits = 0;
  size_t compose_cache_misses = 0;
  size_t join_cache_hits = 0;
  size_t join_cache_misses = 0;
  size_t total_edge_functions = 0;
  size_t unique_edge_functions = 0;

  double singleton_hit_rate() const {
    size_t total = singleton_cache_hits + singleton_cache_misses;
    return total > 0 ? (double)singleton_cache_hits / total : 0.0;
  }

  double compose_hit_rate() const {
    size_t total = compose_cache_hits + compose_cache_misses;
    return total > 0 ? (double)compose_cache_hits / total : 0.0;
  }

  double join_hit_rate() const {
    size_t total = join_cache_hits + join_cache_misses;
    return total > 0 ? (double)join_cache_hits / total : 0.0;
  }

  void reset() {
    singleton_cache_hits = 0;
    singleton_cache_misses = 0;
    compose_cache_hits = 0;
    compose_cache_misses = 0;
    join_cache_hits = 0;
    join_cache_misses = 0;
    total_edge_functions = 0;
    unique_edge_functions = 0;
  }

  void print(llvm::raw_ostream& os) const {
    os << "Edge Function Cache Statistics:\n";
    os << "  Total edge functions created: " << total_edge_functions << "\n";
    os << "  Unique edge functions: " << unique_edge_functions << "\n";
    os << "  Singleton cache hit rate: " << (singleton_hit_rate() * 100.0) << "%\n";
    os << "  Compose cache hit rate: " << (compose_hit_rate() * 100.0) << "%\n";
    os << "  Join cache hit rate: " << (join_hit_rate() * 100.0) << "%\n";
  }
};

// ============================================================================
// Edge Function Wrapper with Identity
// ============================================================================

template<typename Value>
class EdgeFunctionWrapper {
public:
  using EdgeFunction = std::function<Value(const Value&)>;
  using EdgeFunctionPtr = std::shared_ptr<EdgeFunctionWrapper<Value>>;

  explicit EdgeFunctionWrapper(EdgeFunction ef, bool is_identity = false)
      : m_function(std::move(ef)), m_is_identity(is_identity) {}

  Value operator()(const Value& v) const {
    return m_function(v);
  }

  bool is_identity() const { return m_is_identity; }

  const EdgeFunction& get_function() const { return m_function; }

private:
  EdgeFunction m_function;
  bool m_is_identity;
};

// ============================================================================
// Edge Function Cache
// ============================================================================

template<typename Value>
class EdgeFunctionCache {
public:
  using EdgeFunction = std::function<Value(const Value&)>;
  using EdgeFunctionPtr = std::shared_ptr<EdgeFunctionWrapper<Value>>;

  EdgeFunctionCache(bool enable_stats = false)
      : m_enable_stats(enable_stats) {}

  // Create or retrieve a singleton edge function
  EdgeFunctionPtr get_or_create(const EdgeFunction& ef, bool is_identity = false) {
    if (is_identity) {
      return get_identity();
    }

    // Try to find existing singleton
    // Note: This is a simplified version. A production implementation would
    // need a more sophisticated equality check for std::function
    size_t hash = compute_hash(ef);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_singleton_cache.find(hash);
    if (it != m_singleton_cache.end()) {
      if (m_enable_stats) {
        m_stats.singleton_cache_hits++;
      }
      return it->second;
    }

    // Create new edge function
    auto wrapper = std::make_shared<EdgeFunctionWrapper<Value>>(ef, is_identity);
    m_singleton_cache[hash] = wrapper;

    if (m_enable_stats) {
      m_stats.singleton_cache_misses++;
      m_stats.total_edge_functions++;
      m_stats.unique_edge_functions = m_singleton_cache.size() + 1; // +1 for identity
    }

    return wrapper;
  }

  // Get identity edge function (singleton)
  EdgeFunctionPtr get_identity() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_identity_func) {
      m_identity_func = std::make_shared<EdgeFunctionWrapper<Value>>(
          [](const Value& v) { return v; }, true);
    }
    return m_identity_func;
  }

  // Compose two edge functions with caching
  EdgeFunctionPtr compose(EdgeFunctionPtr f1, EdgeFunctionPtr f2) {
    // Identity optimizations
    if (f1->is_identity()) return f2;
    if (f2->is_identity()) return f1;

    ComposePair key{f1, f2};
    
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_compose_cache.find(key);
      if (it != m_compose_cache.end()) {
        if (m_enable_stats) {
          m_stats.compose_cache_hits++;
        }
        return it->second;
      }
    }

    // Create composed function
    EdgeFunction composed = [f1, f2](const Value& v) {
      return (*f1)((*f2)(v));
    };

    EdgeFunctionPtr result = get_or_create(composed, false);

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_compose_cache[key] = result;
    }

    if (m_enable_stats) {
      m_stats.compose_cache_misses++;
    }

    return result;
  }

  // Join two edge functions with caching
  template<typename JoinOp>
  EdgeFunctionPtr join(EdgeFunctionPtr f1, EdgeFunctionPtr f2, JoinOp join_op) {
    ComposePair key{f1, f2};
    
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_join_cache.find(key);
      if (it != m_join_cache.end()) {
        if (m_enable_stats) {
          m_stats.join_cache_hits++;
        }
        return it->second;
      }
    }

    // Create joined function
    EdgeFunction joined = [f1, f2, join_op](const Value& v) {
      return join_op((*f1)(v), (*f2)(v));
    };

    EdgeFunctionPtr result = get_or_create(joined, false);

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_join_cache[key] = result;
    }

    if (m_enable_stats) {
      m_stats.join_cache_misses++;
    }

    return result;
  }

  // Statistics
  const EdgeFunctionCacheStats& get_stats() const { return m_stats; }
  void reset_stats() { m_stats.reset(); }
  void enable_stats(bool enable) { m_enable_stats = enable; }

  // Cache management
  void clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_singleton_cache.clear();
    m_compose_cache.clear();
    m_join_cache.clear();
    m_identity_func.reset();
    m_stats.reset();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_singleton_cache.size();
  }

private:
  struct ComposePair {
    EdgeFunctionPtr f1;
    EdgeFunctionPtr f2;

    bool operator==(const ComposePair& other) const {
      return f1 == other.f1 && f2 == other.f2;
    }
  };

  struct ComposePairHash {
    size_t operator()(const ComposePair& cp) const {
      return std::hash<EdgeFunctionPtr>{}(cp.f1) ^
             (std::hash<EdgeFunctionPtr>{}(cp.f2) << 1);
    }
  };

  // Compute a hash for an edge function
  // Note: This is a simplified version. In practice, you'd need a more
  // sophisticated approach to hash std::function objects
  size_t compute_hash(const EdgeFunction& ef) const {
    // Use the function pointer address as a hash
    // This assumes edge functions are created in consistent ways
    return std::hash<const void*>{}(ef.template target<void>());
  }

  std::unordered_map<size_t, EdgeFunctionPtr> m_singleton_cache;
  std::unordered_map<ComposePair, EdgeFunctionPtr, ComposePairHash> m_compose_cache;
  std::unordered_map<ComposePair, EdgeFunctionPtr, ComposePairHash> m_join_cache;
  EdgeFunctionPtr m_identity_func;
  
  mutable std::mutex m_mutex;
  bool m_enable_stats;
  EdgeFunctionCacheStats m_stats;
};

} // namespace ifds
