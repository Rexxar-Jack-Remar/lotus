#pragma once

#include "Solvers/EGraph/EGraph.h"

namespace lotus::egraph {

template <typename L, typename A> class Dot {
public:
  explicit Dot(const EGraph<L, A> &egraph) : egraph_(egraph) {}

  Dot withConfigLine(std::string line) const {
    Dot copy(*this);
    copy.config_.push_back(std::move(line));
    return copy;
  }

  Dot withAnchors(bool use_anchors) const {
    Dot copy(*this);
    copy.use_anchors_ = use_anchors;
    return copy;
  }

  std::string str() const {
    std::ostringstream oss;
    oss << "digraph egraph {\n";
    oss << "  compound=true\n";
    oss << "  clusterrank=local\n";
    for (const auto &line : config_) {
      oss << "  " << line << "\n";
    }
    for (Id id : egraph_.classIds()) {
      const auto &klass = egraph_[id];
      oss << "  subgraph cluster_" << id.value() << " {\n";
      oss << "    style=dotted\n";
      for (size_t i = 0; i < klass.nodes.size(); ++i) {
        oss << "    " << id.value() << "." << i << " [label=\""
            << displayNode(klass.nodes[i]) << "\"]\n";
      }
      oss << "  }\n";
    }
    for (Id id : egraph_.classIds()) {
      const auto &klass = egraph_[id];
      for (size_t i = 0; i < klass.nodes.size(); ++i) {
        for (size_t arg = 0; arg < klass.nodes[i].children().size(); ++arg) {
          Id child = klass.nodes[i].children()[arg];
          Id leader = egraph_.find(child);
          oss << "  " << id.value() << "." << i << " -> " << leader.value()
              << ".0 [lhead = cluster_" << leader.value();
          if (!use_anchors_) {
            oss << ", label=" << arg;
          }
          oss << "]\n";
        }
      }
    }
    oss << "}\n";
    return oss.str();
  }

  void toDot(const std::string &path) const {
    std::ofstream out(path);
    out << str();
  }

  void toSvg(const std::string &path) const { toDot(path); }
  void toPng(const std::string &path) const { toDot(path); }
  void toPdf(const std::string &path) const { toDot(path); }

  template <typename Range> void runDot(const Range &) const {
    throw std::runtime_error("Graphviz execution is not implemented in Lotus EGraph");
  }

  template <typename Program, typename Range> void run(const Program &, const Range &) const {
    throw std::runtime_error("External dot runners are not implemented in Lotus EGraph");
  }

private:
  const EGraph<L, A> &egraph_;
  std::vector<std::string> config_;
  bool use_anchors_ = true;
};

template <typename L, typename A>
inline std::string toDot(const EGraph<L, A> &egraph) {
  return Dot<L, A>(egraph).str();
}

} // namespace lotus::egraph
