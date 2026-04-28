/*
 * Migrated from IKOS: https://github.com/NASA-SW-VnV/ikos/blob/ac7f7c1738976cabc58c6a53413df6e458995c38/core/include/ikos/core/fixpoint/wpo.hpp
 * The construction of weak partial orderings is based on Sung Kook Kim's,
 * Arnaud J. Venet's, and Aditya V. Thakur's paper: "Deterministic Parallel
 * Fixpoint Computation", in POPL 2020.
*/
#pragma once

#include <crab/cfg/cfg_bgl.hpp>
#include <crab/support/debug.hpp>
#include <crab/support/os.hpp>

#include <boost/pending/disjoint_sets.hpp>

#include <algorithm>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ikos {

template <typename GraphRef> class Wpo;
template <typename GraphRef> class WpoNode;

namespace wpo_impl {

template <typename GraphRef> class WpoBuilder;

} // namespace wpo_impl

template <typename GraphRef>
inline typename boost::graph_traits<GraphRef>::vertex_descriptor
wpo_entry(GraphRef cfg) {
  return entry(cfg);
}

template <typename GraphRef> class WpoNode final {
public:
  enum class Kind { Plain, Head, Exit };

private:
  using graph_traits_t = boost::graph_traits<GraphRef>;
  using NodeRef = typename graph_traits_t::vertex_descriptor;
  using WpoIndex = std::size_t;

  NodeRef _node;
  Kind _kind;
  std::vector<WpoIndex> _successors;
  std::vector<WpoIndex> _predecessors;
  std::vector<WpoIndex> _successors_lifted;
  WpoIndex _head_or_exit;
  std::size_t _num_predecessors;
  std::size_t _num_predecessors_reducible;
  std::size_t _post_order;
  std::unordered_map<WpoIndex, std::size_t> _irreducibles;
  std::size_t _size;

public:
  WpoNode(NodeRef node, Kind kind, std::size_t size, std::size_t post_order)
      : _node(node), _kind(kind), _num_predecessors(0),
        _num_predecessors_reducible(0), _post_order(post_order), _size(size) {}

  WpoNode(const WpoNode &) = delete;
  WpoNode(WpoNode &&) = default;
  WpoNode &operator=(const WpoNode &) = delete;
  WpoNode &operator=(WpoNode &&) = delete;

  NodeRef node() const { return this->_node; }
  Kind kind() const { return this->_kind; }
  bool is_plain() const { return this->_kind == Kind::Plain; }
  bool is_head() const { return this->_kind == Kind::Head; }
  bool is_exit() const { return this->_kind == Kind::Exit; }

  const std::vector<WpoIndex> &successors() const { return this->_successors; }

  const std::vector<WpoIndex> &predecessors() const {
    return this->_predecessors;
  }

  const std::vector<WpoIndex> &successors_lifted() const {
    return this->_successors_lifted;
  }

  const std::unordered_map<WpoIndex, std::size_t> &irreducibles() const {
    if (!this->is_exit()) {
      CRAB_ERROR("trying to get irreducibles from non-exit");
    }
    return this->_irreducibles;
  }

  std::size_t size() const { return this->_size; }
  std::size_t num_predecessors() const { return this->_num_predecessors; }

  std::size_t num_predecessors_reducible() const {
    return this->_num_predecessors_reducible;
  }

  std::size_t post_order() const { return this->_post_order; }

  WpoIndex head() const {
    if (!this->is_exit()) {
      CRAB_ERROR("trying to get head from non-exit");
    }
    return this->_head_or_exit;
  }

  WpoIndex exit() const {
    if (!this->is_head()) {
      CRAB_ERROR("trying to get exit from non-head");
    }
    return this->_head_or_exit;
  }

private:
  void add_successor(WpoIndex idx) { this->_successors.push_back(idx); }
  void add_predecessor(WpoIndex idx) { this->_predecessors.push_back(idx); }

  bool is_successor(WpoIndex idx) const {
    return std::find(this->_successors.begin(), this->_successors.end(), idx) !=
           this->_successors.end();
  }

  void add_successor_lifted(WpoIndex idx) {
    this->_successors_lifted.push_back(idx);
  }

  bool is_successor_lifted(WpoIndex idx) const {
    return std::find(this->_successors_lifted.begin(),
                     this->_successors_lifted.end(),
                     idx) != this->_successors_lifted.end();
  }

  void inc_irreducible(WpoIndex idx) {
    if (!this->is_exit()) {
      CRAB_ERROR("trying to access irreducibles from non-exit");
    }
    this->_irreducibles[idx]++;
  }

  void inc_num_predecessors() { this->_num_predecessors++; }

  void inc_num_predecessors_reducible() {
    this->_num_predecessors_reducible++;
  }

  void set_head_exit(WpoIndex idx) {
    if (this->is_plain()) {
      CRAB_ERROR("trying to access head_or_exit from plain node");
    }
    this->_head_or_exit = idx;
  }

public:
  void write(crab::crab_os &o, WpoIndex idx) const {
    o << "WpoNode:\n";
    o << "  index: " << idx << "\n";
    o << "  node: " << this->_node << "\n";
    switch (this->_kind) {
    case Kind::Plain:
      o << "  kind: plain\n";
      break;
    case Kind::Head:
      o << "  kind: head\n";
      o << "  exit: " << this->_head_or_exit << "\n";
      break;
    case Kind::Exit:
      o << "  kind: exit\n";
      o << "  head: " << this->_head_or_exit << "\n";
      break;
    }
    o << "  successors: ";
    for (auto succ : this->_successors) {
      o << succ << " ";
    }
    o << "\n";
    o << "  successors_lifted: ";
    for (auto succ : this->_successors_lifted) {
      o << succ << " ";
    }
    o << "\n";
    o << "  predecessors: ";
    for (auto pred : this->_predecessors) {
      o << pred << " ";
    }
    o << "\n";
    o << "  number of predecessors: " << this->_num_predecessors << "\n";
    o << "  number of reducible predecessors: "
      << this->_num_predecessors_reducible << "\n";
    o << "  post order: " << this->_post_order << "\n";
    o << "  irreducibles: ";
    for (const auto &p : this->_irreducibles) {
      o << p.first << "," << p.second << " ";
    }
    o << "\n";
    o << "  size: " << this->_size << "\n";
  }

  template <typename> friend class wpo_impl::WpoBuilder;
};

template <typename GraphRef> class Wpo {
private:
  using graph_traits_t = boost::graph_traits<GraphRef>;
  using NodeRef = typename graph_traits_t::vertex_descriptor;
  using WpoNodeT = WpoNode<GraphRef>;
  using Kind = typename WpoNodeT::Kind;
  using WpoIndex = std::size_t;

  std::vector<WpoNodeT> _wpo_nodes;
  std::unordered_map<NodeRef, std::unordered_set<NodeRef>> _back_predecessors;

public:
  explicit Wpo(GraphRef cfg) {
    NodeRef root = wpo_entry(cfg);
    auto succs = out_edges(root, cfg);

    if (succs.first == succs.second) {
      this->_wpo_nodes.emplace_back(root, Kind::Plain, 1, 1);
      return;
    }

    wpo_impl::WpoBuilder<GraphRef>(cfg,
                                   this->_wpo_nodes,
                                   this->_back_predecessors);
  }

  Wpo(const Wpo &) = delete;
  Wpo(Wpo &&) = delete;
  Wpo &operator=(const Wpo &) = delete;
  Wpo &operator=(Wpo &&) = delete;

  std::size_t size() const { return this->_wpo_nodes.size(); }
  WpoIndex entry() const { return this->_wpo_nodes.size() - 1; }

  const std::vector<WpoIndex> &successors(WpoIndex idx) const {
    return this->_wpo_nodes[idx].successors();
  }

  const std::vector<WpoIndex> &predecessors(WpoIndex idx) const {
    return this->_wpo_nodes[idx].predecessors();
  }

  std::size_t num_predecessors(WpoIndex idx) const {
    return this->_wpo_nodes[idx].num_predecessors();
  }

  std::size_t num_predecessors_reducible(WpoIndex idx) const {
    return this->_wpo_nodes[idx].num_predecessors_reducible();
  }

  std::size_t post_order(WpoIndex idx) const {
    return this->_wpo_nodes[idx].post_order();
  }

  const std::unordered_map<WpoIndex, std::size_t> &
  irreducibles(WpoIndex exit) const {
    return this->_wpo_nodes[exit].irreducibles();
  }

  WpoIndex head_of_exit(WpoIndex exit) const {
    return this->_wpo_nodes[exit].head();
  }

  WpoIndex exit_of_head(WpoIndex head) const {
    return this->_wpo_nodes[head].exit();
  }

  NodeRef node(WpoIndex idx) const { return this->_wpo_nodes[idx].node(); }
  Kind kind(WpoIndex idx) const { return this->_wpo_nodes[idx].kind(); }
  bool is_plain(WpoIndex idx) const { return this->_wpo_nodes[idx].is_plain(); }
  bool is_head(WpoIndex idx) const { return this->_wpo_nodes[idx].is_head(); }
  bool is_exit(WpoIndex idx) const { return this->_wpo_nodes[idx].is_exit(); }

  bool is_back_edge(NodeRef head, NodeRef pred) const {
    auto it = this->_back_predecessors.find(head);
    if (it == this->_back_predecessors.end()) {
      return false;
    }
    return it->second.find(pred) != it->second.end();
  }

  void write(crab::crab_os &o) const {
    for (std::size_t idx = 0; idx < this->_wpo_nodes.size(); ++idx) {
      o << "# ";
      this->_wpo_nodes[idx].write(o, idx);
    }
  }
};

template <typename GraphRef>
inline crab::crab_os &operator<<(crab::crab_os &o, const Wpo<GraphRef> &wpo) {
  wpo.write(o);
  return o;
}

template <typename GraphRef> using wpo = Wpo<GraphRef>;
template <typename GraphRef> using wpo_node = WpoNode<GraphRef>;

namespace wpo_impl {

template <typename GraphRef> class WpoBuilder {
private:
  using graph_traits_t = boost::graph_traits<GraphRef>;
  using NodeRef = typename graph_traits_t::vertex_descriptor;
  using WpoNodeT = WpoNode<GraphRef>;
  using Kind = typename WpoNodeT::Kind;
  using WpoIndex = std::size_t;

  struct Edge {
    std::size_t from;
    std::size_t to;
  };

  std::vector<WpoNodeT> &_wpo_nodes;
  std::unordered_map<NodeRef, std::unordered_set<NodeRef>> &_back_predecessors;
  std::unordered_map<NodeRef, std::size_t> _node_to_dfn;
  std::unordered_map<NodeRef, std::size_t> _node_to_post_dfn;
  std::vector<NodeRef> _dfn_to_node;
  std::vector<std::vector<std::size_t>> _back_predecessors_dfn;
  std::vector<std::vector<std::size_t>> _non_back_predecessors_dfn;
  std::unordered_map<std::size_t, std::vector<Edge>> _cross_forward_edges;
  std::size_t _next_dfn;
  std::size_t _next_post_dfn;
  std::size_t _next_idx;
  std::vector<std::size_t> _dfn_to_index;

public:
  WpoBuilder(GraphRef cfg,
             std::vector<WpoNodeT> &wpo_nodes,
             std::unordered_map<NodeRef, std::unordered_set<NodeRef>>
                 &back_predecessors)
      : _wpo_nodes(wpo_nodes), _back_predecessors(back_predecessors),
        _next_dfn(1), _next_post_dfn(1), _next_idx(0) {
    this->construct_auxiliary(cfg);
    this->construct_wpo();
  }

private:
  void construct_auxiliary(GraphRef cfg) {
    using RankMap = std::unordered_map<std::size_t, std::size_t>;
    using RankPropertyMap = boost::associative_property_map<RankMap>;
    using ParentMap = std::unordered_map<std::size_t, std::size_t>;
    using ParentPropertyMap = boost::associative_property_map<ParentMap>;

    struct Tuple {
      NodeRef node;
      bool finished;
      std::size_t pred_dfn;
    };

    std::stack<Tuple> stack;
    std::vector<bool> black;
    std::vector<std::size_t> ancestor;
    RankMap rank_map;
    ParentMap parent_map;
    RankPropertyMap rank_property_map(rank_map);
    ParentPropertyMap parent_property_map(parent_map);
    boost::disjoint_sets<RankPropertyMap, ParentPropertyMap> dsets(
        rank_property_map,
        parent_property_map);

    stack.push(Tuple{entry(cfg), false, 0});

    while (!stack.empty()) {
      NodeRef node = stack.top().node;
      bool finished = stack.top().finished;
      std::size_t pred_dfn = stack.top().pred_dfn;
      stack.pop();

      if (finished) {
        this->_node_to_post_dfn[node] = this->_next_post_dfn++;

        std::size_t dfn = this->node_to_dfn(node);
        black[dfn] = true;

        if (pred_dfn != 0) {
          dsets.union_set(dfn, pred_dfn);
          ancestor[dsets.find_set(pred_dfn)] = pred_dfn;
        }
      } else if (this->node_to_dfn(node) != 0) {
        continue;
      } else {
        std::size_t dfn = this->_next_dfn++;
        this->_dfn_to_node.push_back(node);
        this->_node_to_dfn[node] = dfn;

        black.resize(this->_next_dfn);
        this->_back_predecessors_dfn.resize(this->_next_dfn);
        this->_non_back_predecessors_dfn.resize(this->_next_dfn);

        dsets.make_set(dfn);
        ancestor.resize(this->_next_dfn);
        ancestor[dfn] = dfn;

        stack.push(Tuple{node, true, pred_dfn});

        std::vector<NodeRef> successors;
        auto succ_edges = out_edges(node, cfg);
        for (auto it = succ_edges.first; it != succ_edges.second; ++it) {
          successors.push_back(target(*it, cfg));
        }

        for (auto it = successors.rbegin(); it != successors.rend(); ++it) {
          NodeRef succ = *it;
          std::size_t succ_dfn = this->node_to_dfn(succ);
          if (succ_dfn == 0) {
            stack.push(Tuple{succ, false, dfn});
          } else if (black[succ_dfn]) {
            auto lca = ancestor[dsets.find_set(succ_dfn)];
            this->_cross_forward_edges[lca].push_back(Edge{dfn, succ_dfn});
          } else {
            this->_back_predecessors_dfn[succ_dfn].push_back(dfn);
            this->_back_predecessors[succ].insert(node);
          }
        }

        if (pred_dfn != 0) {
          this->_non_back_predecessors_dfn[dfn].push_back(pred_dfn);
        }
      }
    }
  }

  void construct_wpo() {
    using RankMap = std::unordered_map<std::size_t, std::size_t>;
    using RankPropertyMap = boost::associative_property_map<RankMap>;
    using ParentMap = std::unordered_map<std::size_t, std::size_t>;
    using ParentPropertyMap = boost::associative_property_map<ParentMap>;

    RankMap rank_map;
    ParentMap parent_map;
    RankPropertyMap rank_property_map(rank_map);
    ParentPropertyMap parent_property_map(parent_map);
    boost::disjoint_sets<RankPropertyMap, ParentPropertyMap> dsets(
        rank_property_map,
        parent_property_map);

    std::vector<std::size_t> rep(this->_next_dfn);
    std::vector<std::size_t> exit(this->_next_dfn);
    std::vector<std::size_t> size(this->_next_dfn);
    std::vector<std::vector<Edge>> origin(this->_next_dfn);
    this->_dfn_to_index.resize(2 * this->_next_dfn);

    std::size_t dfn = this->_next_dfn;

    for (std::size_t v = 1; v < this->_next_dfn; ++v) {
      dsets.make_set(v);
      rep[v] = v;
      exit[v] = v;
      for (std::size_t u : this->_non_back_predecessors_dfn[v]) {
        origin[v].push_back(Edge{u, v});
      }
    }

    for (std::size_t h = this->_next_dfn - 1; h > 0; --h) {
      auto it = this->_cross_forward_edges.find(h);
      if (it != this->_cross_forward_edges.end()) {
        for (const Edge &edge : it->second) {
          std::size_t rep_to = rep[dsets.find_set(edge.to)];
          this->_non_back_predecessors_dfn[rep_to].push_back(edge.from);
          origin[rep_to].push_back(edge);
        }
      }

      bool is_scc = false;
      std::unordered_set<std::size_t> exits_h;
      for (std::size_t v : this->_back_predecessors_dfn[h]) {
        if (v != h) {
          exits_h.insert(rep[dsets.find_set(v)]);
        } else {
          is_scc = true;
        }
      }
      if (!exits_h.empty()) {
        is_scc = true;
      }

      std::unordered_set<std::size_t> components_h(exits_h);
      std::vector<std::size_t> worklist_h(exits_h.begin(), exits_h.end());

      while (!worklist_h.empty()) {
        std::size_t v = worklist_h.back();
        worklist_h.pop_back();
        for (std::size_t u : this->_non_back_predecessors_dfn[v]) {
          std::size_t rep_u = rep[dsets.find_set(u)];
          if (components_h.find(rep_u) == components_h.end() && rep_u != h) {
            components_h.insert(rep_u);
            worklist_h.push_back(rep_u);
          }
        }
      }

      if (!is_scc) {
        size[h] = 1;
        this->add_wpo_node(h, this->dfn_to_node(h), Kind::Plain, 1);
        continue;
      }

      std::size_t size_h = 2;
      for (std::size_t v : components_h) {
        size_h += size[v];
      }
      size[h] = size_h;

      std::size_t x = dfn++;
      this->add_wpo_node(x, this->dfn_to_node(h), Kind::Exit, size_h);
      this->add_wpo_node(h, this->dfn_to_node(h), Kind::Head, size_h);
      this->set_head_exit(h, x);

      if (exits_h.empty()) {
        this->add_successor(h, x, x, false);
      } else {
        for (std::size_t xx : exits_h) {
          this->add_successor(exit[xx], x, x, false);
        }
      }

      for (std::size_t v : components_h) {
        for (const Edge &edge : origin[v]) {
          std::size_t u = edge.from;
          std::size_t vv = edge.to;
          std::size_t x_u = exit[rep[dsets.find_set(u)]];
          std::size_t x_v = exit[v];
          this->add_successor(x_u, vv, x_v, v != vv);
        }
      }

      for (std::size_t v : components_h) {
        dsets.union_set(v, h);
        rep[dsets.find_set(v)] = h;
      }

      exit[h] = x;
    }

    for (std::size_t v = 1; v < this->_next_dfn; ++v) {
      if (rep[dsets.find_set(v)] == v) {
        for (const Edge &edge : origin[v]) {
          std::size_t u = edge.from;
          std::size_t vv = edge.to;
          std::size_t x_u = exit[rep[dsets.find_set(u)]];
          std::size_t x_v = exit[v];
          this->add_successor(x_u, vv, x_v, v != vv);
        }
      }
    }
  }

  std::size_t node_to_dfn(NodeRef node) const {
    auto it = this->_node_to_dfn.find(node);
    return (it != this->_node_to_dfn.end()) ? it->second : 0;
  }

  const NodeRef &dfn_to_node(std::size_t dfn) const {
    return this->_dfn_to_node.at(dfn - 1);
  }

  void add_wpo_node(std::size_t dfn,
                    NodeRef node,
                    Kind kind,
                    std::size_t size) {
    this->_dfn_to_index[dfn] = this->_next_idx++;
    this->_wpo_nodes.emplace_back(node,
                                  kind,
                                  size,
                                  this->_node_to_post_dfn[node]);
  }

  WpoNodeT &dfn_to_wpo_node(std::size_t dfn) {
    return this->_wpo_nodes[this->_dfn_to_index[dfn]];
  }

  WpoIndex dfn_to_index(std::size_t dfn) const {
    return this->_dfn_to_index[dfn];
  }

  void set_head_exit(std::size_t h, std::size_t x) {
    auto head_idx = this->dfn_to_index(h);
    auto exit_idx = this->dfn_to_index(x);
    this->dfn_to_wpo_node(x).set_head_exit(head_idx);
    this->dfn_to_wpo_node(h).set_head_exit(exit_idx);
  }

  void add_successor(std::size_t from_dfn,
                     std::size_t to_dfn,
                     std::size_t exit_dfn,
                     bool irreducible) {
    WpoNodeT &from_node = this->dfn_to_wpo_node(from_dfn);
    WpoNodeT &to_node = this->dfn_to_wpo_node(to_dfn);
    std::size_t to_idx = this->dfn_to_index(to_dfn);

    if (from_node.is_successor(to_idx)) {
      return;
    }

    from_node.add_successor(to_idx);
    to_node.inc_num_predecessors();
    to_node.add_predecessor(this->dfn_to_index(from_dfn));

    if (irreducible) {
      this->dfn_to_wpo_node(exit_dfn).inc_irreducible(to_idx);
    } else {
      to_node.inc_num_predecessors_reducible();
    }
  }
};

} // namespace wpo_impl
} // namespace ikos
