#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace npa {

/**
 * Immutable persistent set of fact identifiers.
 *
 * The representation is a deterministic treap. Updates preserve value
 * semantics while sharing unchanged subtrees, which keeps all cached AST
 * prefix values linearithmic instead of copying an ever-growing bit vector at
 * every prefix.
 */
class SparseFactSet {
private:
  struct Node;
  using NodePtr = std::shared_ptr<const Node>;

  struct Node {
    unsigned key;
    std::uint64_t priority;
    NodePtr left;
    NodePtr right;
    std::size_t size;

    Node(unsigned key, std::uint64_t priority, NodePtr left, NodePtr right)
        : key(key), priority(priority), left(std::move(left)),
          right(std::move(right)),
          size(1 + (this->left ? this->left->size : 0) +
               (this->right ? this->right->size : 0)) {}
  };

  struct SplitResult {
    NodePtr less;
    NodePtr greater;
    bool found = false;
  };

  NodePtr root_;

  static std::uint64_t priorityOf(unsigned key) {
    std::uint64_t value = static_cast<std::uint64_t>(key) +
                          UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
  }

  static bool hasHigherPriority(const NodePtr &lhs, const NodePtr &rhs) {
    if (lhs->priority != rhs->priority)
      return lhs->priority > rhs->priority;
    return lhs->key > rhs->key;
  }

  static bool hasHigherPriority(unsigned key, std::uint64_t priority,
                                const NodePtr &rhs) {
    if (priority != rhs->priority)
      return priority > rhs->priority;
    return key > rhs->key;
  }

  static NodePtr makeNode(unsigned key, NodePtr left = nullptr,
                          NodePtr right = nullptr) {
    return std::make_shared<Node>(key, priorityOf(key), std::move(left),
                                  std::move(right));
  }

  static NodePtr rebuild(const NodePtr &node, NodePtr left, NodePtr right) {
    if (left == node->left && right == node->right)
      return node;
    return makeNode(node->key, std::move(left), std::move(right));
  }

  static NodePtr merge(NodePtr left, NodePtr right) {
    if (!left)
      return right;
    if (!right)
      return left;
    if (hasHigherPriority(left, right)) {
      NodePtr mergedRight = merge(left->right, std::move(right));
      return rebuild(left, left->left, std::move(mergedRight));
    }
    NodePtr mergedLeft = merge(std::move(left), right->left);
    return rebuild(right, std::move(mergedLeft), right->right);
  }

  static SplitResult split(const NodePtr &root, unsigned key) {
    if (!root)
      return {};
    if (key == root->key)
      return {root->left, root->right, true};
    if (key < root->key) {
      SplitResult parts = split(root->left, key);
      parts.greater =
          rebuild(root, std::move(parts.greater), root->right);
      return parts;
    }
    SplitResult parts = split(root->right, key);
    parts.less = rebuild(root, root->left, std::move(parts.less));
    return parts;
  }

  static NodePtr insert(const NodePtr &root, unsigned key) {
    if (!root)
      return makeNode(key);
    if (key == root->key)
      return root;

    if (hasHigherPriority(key, priorityOf(key), root)) {
      SplitResult parts = split(root, key);
      return makeNode(key, std::move(parts.less), std::move(parts.greater));
    }
    if (key < root->key) {
      NodePtr left = insert(root->left, key);
      return rebuild(root, std::move(left), root->right);
    }
    NodePtr right = insert(root->right, key);
    return rebuild(root, root->left, std::move(right));
  }

  static NodePtr erase(const NodePtr &root, unsigned key) {
    if (!root)
      return root;
    if (key == root->key)
      return merge(root->left, root->right);
    if (key < root->key) {
      NodePtr left = erase(root->left, key);
      return rebuild(root, std::move(left), root->right);
    }
    NodePtr right = erase(root->right, key);
    return rebuild(root, root->left, std::move(right));
  }

  static NodePtr unite(NodePtr lhs, NodePtr rhs) {
    if (lhs == rhs || !rhs)
      return lhs;
    if (!lhs)
      return rhs;
    if (!hasHigherPriority(lhs, rhs))
      std::swap(lhs, rhs);

    SplitResult parts = split(rhs, lhs->key);
    NodePtr left = unite(lhs->left, std::move(parts.less));
    NodePtr right = unite(lhs->right, std::move(parts.greater));
    return rebuild(lhs, std::move(left), std::move(right));
  }

  static NodePtr intersect(NodePtr lhs, NodePtr rhs) {
    if (lhs == rhs || !lhs)
      return lhs;
    if (!rhs)
      return nullptr;
    if (!hasHigherPriority(lhs, rhs))
      std::swap(lhs, rhs);

    SplitResult parts = split(rhs, lhs->key);
    NodePtr left = intersect(lhs->left, std::move(parts.less));
    NodePtr right = intersect(lhs->right, std::move(parts.greater));
    if (!parts.found)
      return merge(std::move(left), std::move(right));
    return rebuild(lhs, std::move(left), std::move(right));
  }

  static NodePtr difference(const NodePtr &lhs, const NodePtr &rhs) {
    if (!lhs || !rhs)
      return lhs;
    if (lhs == rhs)
      return nullptr;

    SplitResult parts = split(rhs, lhs->key);
    NodePtr left = difference(lhs->left, parts.less);
    NodePtr right = difference(lhs->right, parts.greater);
    if (parts.found)
      return merge(std::move(left), std::move(right));
    return rebuild(lhs, std::move(left), std::move(right));
  }

  static bool equalTrees(const NodePtr &lhs, const NodePtr &rhs) {
    if (lhs == rhs)
      return true;
    if (!lhs || !rhs || lhs->size != rhs->size || lhs->key != rhs->key)
      return false;
    return equalTrees(lhs->left, rhs->left) &&
           equalTrees(lhs->right, rhs->right);
  }

public:
  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = unsigned;
    using difference_type = std::ptrdiff_t;
    using pointer = const unsigned *;
    using reference = unsigned;

    const_iterator() = default;

    reference operator*() const { return stack_.back()->key; }

    const_iterator &operator++() {
      const Node *current = stack_.back();
      stack_.pop_back();
      pushLeft(current->right.get());
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator previous = *this;
      ++*this;
      return previous;
    }

    bool operator==(const const_iterator &other) const {
      if (stack_.empty() || other.stack_.empty())
        return stack_.empty() == other.stack_.empty();
      return stack_.back() == other.stack_.back();
    }

    bool operator!=(const const_iterator &other) const {
      return !(*this == other);
    }

  private:
    friend class SparseFactSet;

    explicit const_iterator(const Node *root) { pushLeft(root); }

    void pushLeft(const Node *node) {
      while (node) {
        stack_.push_back(node);
        node = node->left.get();
      }
    }

    std::vector<const Node *> stack_;
  };

  bool empty() const { return !root_; }
  std::size_t count() const { return root_ ? root_->size : 0; }

  bool test(unsigned key) const {
    const Node *node = root_.get();
    while (node) {
      if (key == node->key)
        return true;
      node = key < node->key ? node->left.get() : node->right.get();
    }
    return false;
  }

  bool set(unsigned key) {
    NodePtr updated = insert(root_, key);
    bool changed = updated != root_;
    root_ = std::move(updated);
    return changed;
  }

  bool reset(unsigned key) {
    NodePtr updated = erase(root_, key);
    bool changed = updated != root_;
    root_ = std::move(updated);
    return changed;
  }

  void clear() { root_.reset(); }

  bool operator|=(const SparseFactSet &other) {
    NodePtr updated = unite(root_, other.root_);
    bool changed = updated != root_;
    root_ = std::move(updated);
    return changed;
  }

  bool operator&=(const SparseFactSet &other) {
    NodePtr updated = intersect(root_, other.root_);
    bool changed = updated != root_;
    root_ = std::move(updated);
    return changed;
  }

  bool intersectWithComplement(const SparseFactSet &other) {
    NodePtr updated = difference(root_, other.root_);
    bool changed = updated != root_;
    root_ = std::move(updated);
    return changed;
  }

  bool operator==(const SparseFactSet &other) const {
    return equalTrees(root_, other.root_);
  }

  bool operator!=(const SparseFactSet &other) const {
    return !(*this == other);
  }

  const_iterator begin() const { return const_iterator(root_.get()); }
  const_iterator end() const { return {}; }
};

} // namespace npa

