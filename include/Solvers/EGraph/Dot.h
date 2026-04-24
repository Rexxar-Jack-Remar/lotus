#pragma once

#include "Solvers/EGraph/EGraph.h"

#ifndef LOTUS_EGRAPH_ENABLE_DOT
#define LOTUS_EGRAPH_ENABLE_DOT 1
#endif

#if LOTUS_EGRAPH_ENABLE_DOT

#include <cstdlib>
#include <filesystem>

#include <unistd.h>

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
          auto [anchor, label] =
              edge(arg, klass.nodes[i].children().size(), use_anchors_);
          if (leader == id) {
            oss << "  " << id.value() << "." << i << anchor << " -> "
                << id.value() << "." << i << ":n [lhead = cluster_"
                << id.value();
          } else {
            oss << "  " << id.value() << "." << i << anchor << " -> "
                << child.value() << ".0 [lhead = cluster_" << leader.value();
          }
          if (!label.empty()) {
            oss << ", " << label;
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

  void toSvg(const std::string &path) const { runDotImpl("svg", path); }
  void toPng(const std::string &path) const { runDotImpl("png", path); }
  void toPdf(const std::string &path) const { runDotImpl("pdf", path); }

  template <typename Range> void runDot(const Range &args) const {
    run("dot", args);
  }

  template <typename Program, typename Range>
  void run(const Program &program, const Range &args) const {
    namespace fs = std::filesystem;
    std::string path_template =
        (fs::temp_directory_path() / "lotus-egraph-dot-XXXXXX.dot").string();
    std::vector<char> tmp(path_template.begin(), path_template.end());
    tmp.push_back('\0');

    int fd = mkstemps(tmp.data(), 4);
    if (fd == -1) {
      throw std::runtime_error("Failed to create temporary dot file");
    }
    ::close(fd);

    fs::path input_path(tmp.data());
    try {
      toDot(input_path.string());

      std::ostringstream cmd;
      cmd << quoteShell(program);
      for (const auto &arg : args) {
        cmd << ' ' << quoteShell(arg);
      }
      cmd << ' ' << quoteShell(input_path.string());
      cmd << " > /dev/null";

      int rc = std::system(cmd.str().c_str());
      fs::remove(input_path);
      if (rc != 0) {
        throw std::runtime_error("Graphviz command failed with exit code " +
                                 std::to_string(rc));
      }
    } catch (...) {
      std::error_code ec;
      fs::remove(input_path, ec);
      throw;
    }
  }

private:
  static std::pair<std::string, std::string>
  edge(size_t i, size_t len, bool use_anchors) {
    if (i >= len) {
      throw std::runtime_error("Dot edge index out of range");
    }
    if (!use_anchors) {
      return {"", "label=" + std::to_string(i)};
    }
    switch (len) {
    case 1:
      return {"", ""};
    case 2:
      return {i == 0 ? ":sw" : ":se", ""};
    case 3:
      return {i == 0 ? ":sw" : (i == 1 ? ":s" : ":se"), ""};
    default:
      return {"", "label=" + std::to_string(i)};
    }
  }

  template <typename T> static std::string quoteShell(const T &value) {
    std::ostringstream oss;
    oss << value;
    std::string text = oss.str();
    std::string quoted = "'";
    for (char ch : text) {
      if (ch == '\'') {
        quoted += "'\\''";
      } else {
        quoted += ch;
      }
    }
    quoted += "'";
    return quoted;
  }

  void runDotImpl(const char *format, const std::string &path) const {
    std::vector<std::string> args = {std::string("-T") + format, "-o", path};
    runDot(args);
  }

  const EGraph<L, A> &egraph_;
  std::vector<std::string> config_;
  bool use_anchors_ = true;
};

template <typename L, typename A>
inline std::string toDot(const EGraph<L, A> &egraph) {
  return Dot<L, A>(egraph).str();
}

} // namespace lotus::egraph

#endif
