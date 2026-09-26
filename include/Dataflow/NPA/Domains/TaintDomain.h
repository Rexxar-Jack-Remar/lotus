#pragma once

#include "Dataflow/NPA/Core/Domain.h"
#include "Dataflow/NPA/Domains/SparseFactSet.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace npa {

/// Persistent sparse map from an input fact to its complete output row.
class SparseTaintRows {
private:
  struct Node;
  using NodePtr = std::shared_ptr<const Node>;

  struct Node {
    unsigned key;
    std::uint64_t priority;
    SparseFactSet row;
    NodePtr left;
    NodePtr right;
    std::size_t size;

    Node(unsigned key, std::uint64_t priority, SparseFactSet row, NodePtr left,
         NodePtr right)
        : key(key), priority(priority), row(std::move(row)),
          left(std::move(left)), right(std::move(right)),
          size(1 + (this->left ? this->left->size : 0) +
               (this->right ? this->right->size : 0)) {}
  };

  NodePtr root_;

  static std::uint64_t priorityOf(unsigned key) {
    std::uint64_t value = static_cast<std::uint64_t>(key) +
                          UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
  }

  static bool higher(unsigned key, std::uint64_t priority,
                     const NodePtr &node) {
    if (priority != node->priority)
      return priority > node->priority;
    return key > node->key;
  }

  static bool higher(const NodePtr &lhs, const NodePtr &rhs) {
    return higher(lhs->key, lhs->priority, rhs);
  }

  static NodePtr makeNode(unsigned key, SparseFactSet row,
                          NodePtr left = nullptr, NodePtr right = nullptr) {
    return std::make_shared<Node>(key, priorityOf(key), std::move(row),
                                  std::move(left), std::move(right));
  }

  static NodePtr rebuild(const NodePtr &node, NodePtr left, NodePtr right) {
    if (left == node->left && right == node->right)
      return node;
    return makeNode(node->key, node->row, std::move(left), std::move(right));
  }

  static NodePtr merge(NodePtr left, NodePtr right) {
    if (!left)
      return right;
    if (!right)
      return left;
    if (higher(left, right)) {
      NodePtr mergedRight = merge(left->right, std::move(right));
      return rebuild(left, left->left, std::move(mergedRight));
    }
    NodePtr mergedLeft = merge(std::move(left), right->left);
    return rebuild(right, std::move(mergedLeft), right->right);
  }

  struct SplitResult {
    NodePtr less;
    NodePtr greater;
  };

  static SplitResult split(const NodePtr &root, unsigned key) {
    if (!root)
      return {};
    if (key == root->key)
      return {root->left, root->right};
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

  static NodePtr insert(const NodePtr &root, unsigned key,
                        const SparseFactSet &row) {
    if (!root)
      return makeNode(key, row);
    if (key == root->key) {
      if (row == root->row)
        return root;
      return makeNode(key, row, root->left, root->right);
    }
    const std::uint64_t priority = priorityOf(key);
    if (higher(key, priority, root)) {
      SplitResult parts = split(root, key);
      return makeNode(key, row, std::move(parts.less),
                      std::move(parts.greater));
    }
    if (key < root->key) {
      NodePtr left = insert(root->left, key, row);
      return rebuild(root, std::move(left), root->right);
    }
    NodePtr right = insert(root->right, key, row);
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

  static bool equalTrees(const NodePtr &lhs, const NodePtr &rhs) {
    if (lhs == rhs)
      return true;
    if (!lhs || !rhs || lhs->size != rhs->size || lhs->key != rhs->key ||
        lhs->row != rhs->row) {
      return false;
    }
    return equalTrees(lhs->left, rhs->left) &&
           equalTrees(lhs->right, rhs->right);
  }

  template <class Fn> static void forEach(const NodePtr &node, Fn &fn) {
    if (!node)
      return;
    forEach(node->left, fn);
    fn(node->key, node->row);
    forEach(node->right, fn);
  }

public:
  bool empty() const { return !root_; }
  std::size_t size() const { return root_ ? root_->size : 0; }

  const SparseFactSet *find(unsigned key) const {
    const Node *node = root_.get();
    while (node) {
      if (key == node->key)
        return &node->row;
      node = key < node->key ? node->left.get() : node->right.get();
    }
    return nullptr;
  }

  bool set(unsigned key, const SparseFactSet &row) {
    NodePtr updated = insert(root_, key, row);
    bool changed = updated != root_;
    root_ = std::move(updated);
    return changed;
  }

  bool erase(unsigned key) {
    NodePtr updated = SparseTaintRows::erase(root_, key);
    bool changed = updated != root_;
    root_ = std::move(updated);
    return changed;
  }

  template <class Fn> void forEach(Fn &&fn) const {
    SparseTaintRows::forEach(root_, fn);
  }

  bool operator==(const SparseTaintRows &other) const {
    return equalTrees(root_, other.root_);
  }

  bool operator!=(const SparseTaintRows &other) const {
    return !(*this == other);
  }
};

struct TaintTransfer {
  bool identity = false;
  SparseTaintRows rows;
  SparseFactSet gen;

  bool operator==(const TaintTransfer &other) const {
    return identity == other.identity && rows == other.rows &&
           gen == other.gen;
  }
};

class TaintTransformer {
public:
  using value_type = TaintTransfer;
  using fact_type = SparseFactSet;
  using test_type = bool;
  static constexpr bool idempotent = true;
  // Like other generating transfer functions, zero() annihilates only when it
  // is the outer (left) composition operand.
  static constexpr bool sparse_npa_zero_left_annihilator = true;
  static constexpr bool sparse_npa_zero_right_annihilator = false;

  static value_type zero();
  static value_type one();
  static value_type star(const value_type &value);
  static bool equal(const value_type &a, const value_type &b);
  static value_type combine(const value_type &a, const value_type &b);
  static value_type ndetCombine(const value_type &a, const value_type &b);
  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e);
  static value_type extend(const value_type &a, const value_type &b);
  static value_type extend_lin(const value_type &a, const value_type &b);
  static value_type subtract(const value_type &a, const value_type &b);

  static fact_type apply(const value_type &transfer, const fact_type &input);

  static void addEdge(value_type &transfer, unsigned from, unsigned to);
  static void addGen(value_type &transfer, unsigned bit);
  static void clearInput(value_type &transfer, unsigned input);
  static void clearOutput(value_type &transfer, unsigned output);
  static void kill(value_type &transfer, unsigned bit);

private:
  static fact_type row(const value_type &transfer, unsigned input);
  static void setRow(value_type &transfer, unsigned input, fact_type row);
  static fact_type applyRelation(const value_type &transfer,
                                 const fact_type &input);
};

} // namespace npa

