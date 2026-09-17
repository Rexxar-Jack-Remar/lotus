#include "CFL/DynamicDyck/IO.h"
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dyck = lotus::cfl::dynamic_dyck;

namespace {
void usage(std::ostream &output) {
  output << "usage: lotus-cfl-dynamic-dyck <0|1> <initial.dot> <updates.seq>\n"
            "       lotus-cfl-dynamic-dyck [--recompute] [--stats] "
            "[--print-components] "
            "<initial.dot> <updates.seq>\n"
            "       lotus-cfl-dynamic-dyck --algorithm primary-component "
            "[--backend deterministic|hdt] [--counted] [--stats] "
            "[--print-components] <initial.dot> <updates.seq>\n"
            "0=recompute, 1=dynamic. Default output is elapsed seconds and a "
            "space,\n"
            "matching the original DyckReach binary.\n"
            "Default algorithm: popl22. POPL 2024 defaults to set semantics; "
            "--counted removes one reference per deletion.\n";
}

// Intern the original artifact's opaque IDs and labels into the numeric API.
// Preallocate sequence endpoints, including absent deletions, just as runFiles
// does. Parsing and initial saturation are excluded from update timing.
dyck::RunResult
runPrimaryComponent(const std::string &initial_path,
                    const std::string &sequence_path,
                    dyck::PrimaryComponentEdgeSemantics semantics,
                    dyck::PrimaryComponentConnectivityBackend backend) {
  std::ifstream initial(initial_path), sequence(sequence_path);
  if (!initial)
    throw std::runtime_error("cannot open initial graph: " + initial_path);
  if (!sequence)
    throw std::runtime_error("cannot open update sequence: " + sequence_path);
  std::map<std::string, dyck::Vertex> vertex_ids;
  std::map<std::string, dyck::Label> label_ids;
  std::vector<std::string> names;
  dyck::Graph graph;
  auto vertex = [&](const std::string &name) {
    const auto found = vertex_ids.find(name);
    if (found != vertex_ids.end())
      return found->second;
    if (names.size() >=
        static_cast<std::uint64_t>(std::numeric_limits<dyck::Vertex>::max()))
      throw std::length_error("too many vertices");
    const auto id = static_cast<dyck::Vertex>(names.size());
    vertex_ids.emplace(name, id);
    names.push_back(name);
    graph.vertices.push_back(id);
    return id;
  };
  auto edge = [&](const std::string &source, const std::string &target,
                  const std::string &label) {
    if (label.size() <= 4 ||
        (label.compare(0, 4, "op--") != 0 && label.compare(0, 4, "cp--") != 0))
      throw std::invalid_argument("expected op--TYPE or cp--TYPE label");
    const auto type = label.substr(4);
    auto found = label_ids.find(type);
    if (found == label_ids.end()) {
      if (label_ids.size() > std::numeric_limits<dyck::Label>::max())
        throw std::length_error("too many labels");
      found =
          label_ids.emplace(type, static_cast<dyck::Label>(label_ids.size()))
              .first;
    }
    return dyck::Edge{vertex(source), vertex(target), found->second,
                      label.front() == 'o' ? dyck::Parenthesis::Open
                                           : dyck::Parenthesis::Close};
  };
  auto read = [](std::istream &input, const std::string &path, auto parse) {
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      const auto first = line.find_first_not_of(" \t\r\n");
      if (first == std::string::npos || line[first] == '#' ||
          line.compare(first, 2, "//") == 0)
        continue;
      const auto last = line.find_last_not_of(" \t\r\n");
      try {
        parse(line.substr(first, last - first + 1));
      } catch (const std::invalid_argument &error) {
        throw std::invalid_argument(path + ":" + std::to_string(line_number) +
                                    ": " + error.what());
      }
    }
    if (input.bad() || (input.fail() && !input.eof()))
      throw std::runtime_error("failed reading " + path);
  };
  const std::regex edge_pattern(
      R"re(^(.+?)\s*->\s*(.+?)\s*\[\s*label\s*=\s*"([^"]*)"\s*\]\s*;?$)re");
  const std::regex wrapper_pattern(
      R"re(^digraph(\s+[A-Za-z_][A-Za-z_0-9]*)?\s*\{\s*\}?\s*;?$)re");
  const std::regex vertex_pattern(R"re(^("[^"]+"|[A-Za-z_0-9.-]+)\s*;?$)re");
  read(initial, initial_path, [&](const std::string &line) {
    std::smatch match;
    if (line == "{" || line == "}" || line == "};" ||
        std::regex_match(line, wrapper_pattern))
      return;
    if (std::regex_match(line, match, edge_pattern))
      graph.edges.push_back(
          edge(match[1].str(), match[2].str(), match[3].str()));
    else if (std::regex_match(line, match, vertex_pattern))
      vertex(match[1].str());
    else
      throw std::invalid_argument("expected a node or labeled edge record");
  });
  std::vector<dyck::Update> updates;
  read(sequence, sequence_path, [&](const std::string &line) {
    std::istringstream fields(line);
    std::string operation, source, target, label, extra;
    if (!(fields >> operation >> source >> target >> label) ||
        (fields >> extra) || (operation != "A" && operation != "D"))
      throw std::invalid_argument(
          "expected A|D SOURCE TARGET op--TYPE|cp--TYPE");
    updates.push_back(
        {operation == "A" ? dyck::UpdateKind::Insert : dyck::UpdateKind::Delete,
         edge(source, target, label)});
  });
  dyck::PrimaryComponentSolver solver(graph, semantics, backend);
  const auto initial_statistics = solver.statistics();
  dyck::RunResult result;
  result.updates = updates.size();
  for (const auto &update : updates) {
    const auto start = std::chrono::steady_clock::now();
    solver.apply(update);
    result.elapsed_seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
  }
  result.statistics = solver.statistics();
  result.statistics.insertions -= initial_statistics.insertions;
  result.statistics.deletions -= initial_statistics.deletions;
  for (const auto &component : solver.components()) {
    std::vector<std::string> strings;
    for (dyck::Vertex id : component)
      strings.push_back(names.at(static_cast<std::size_t>(id)));
    std::sort(strings.begin(), strings.end());
    result.components.push_back(std::move(strings));
  }
  std::sort(result.components.begin(), result.components.end());
  return result;
}
} // namespace

int main(int argc, char **argv) {
  try {
    bool dynamic = true, print_components = false, print_stats = false;
    std::string algorithm = "popl22";
    auto semantics = dyck::PrimaryComponentEdgeSemantics::Set;
    auto backend = dyck::PrimaryComponentConnectivityBackend::Deterministic;
    bool backend_selected = false, counted = false;
    std::vector<std::string> paths;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--help" || argument == "-h") {
        usage(std::cout);
        return 0;
      }
      if (argument == "--recompute")
        dynamic = false;
      else if (argument == "--stats")
        print_stats = true;
      else if (argument == "--print-components")
        print_components = true;
      else if (argument == "--algorithm") {
        if (++index == argc)
          throw std::invalid_argument(
              "--algorithm requires popl22 or primary-component");
        algorithm = argv[index];
        if (algorithm != "popl22" && algorithm != "primary-component")
          throw std::invalid_argument("unknown algorithm '" + algorithm + "'");
      } else if (argument == "--backend") {
        if (++index == argc)
          throw std::invalid_argument(
              "--backend requires deterministic or hdt");
        const std::string name = argv[index];
        if (name != "deterministic" && name != "hdt")
          throw std::invalid_argument("unknown backend '" + name + "'");
        backend =
            name == "hdt"
                ? dyck::PrimaryComponentConnectivityBackend::HDT
                : dyck::PrimaryComponentConnectivityBackend::Deterministic;
        backend_selected = true;
      } else if (argument == "--counted") {
        semantics = dyck::PrimaryComponentEdgeSemantics::ReferenceCounted;
        counted = true;
      } else if (argument.compare(0, 2, "--") == 0)
        throw std::invalid_argument("unknown option '" + argument + "'");
      else
        paths.push_back(argument);
    }
    if (paths.size() == 3 && (paths.front() == "0" || paths.front() == "1")) {
      dynamic = paths.front() == "1";
      paths.erase(paths.begin());
    }
    if (paths.size() != 2) {
      usage(std::cerr);
      return 1;
    }
    if (algorithm == "primary-component" && !dynamic)
      throw std::invalid_argument(
          "POPL 2024 supports dynamic mode only (not 0/--recompute)");
    if (algorithm != "primary-component" && (backend_selected || counted))
      throw std::invalid_argument(
          "--backend and --counted require --algorithm primary-component");
    const auto result =
        algorithm == "primary-component"
            ? runPrimaryComponent(paths[0], paths[1], semantics, backend)
            : dyck::runFiles(dynamic, paths[0], paths[1]);
    if (print_stats) {
      const auto &stats = result.statistics;
      std::cout << "mode=" << (dynamic ? "dynamic" : "recompute")
                << " vertices=" << stats.vertices << " edges=" << stats.edges
                << " components=" << stats.components
                << " updates=" << result.updates
                << " changed_updates=" << stats.insertions + stats.deletions
                << " cycle_rebuilds=" << stats.cycle_rebuilds
                << " elapsed_seconds=" << result.elapsed_seconds << '\n';
      if (algorithm == "primary-component")
        std::cout << "algorithm=primary-component backend="
                  << (backend == dyck::PrimaryComponentConnectivityBackend::HDT
                          ? "hdt"
                          : "deterministic")
                  << " semantics=" << (counted ? "counted" : "set") << '\n';
    } else {
      std::cout << result.elapsed_seconds << ' ';
      if (print_components)
        std::cout << '\n';
    }
    if (print_components)
      for (const auto &component : result.components) {
        std::cout << "component:";
        for (const auto &vertex : component)
          std::cout << ' ' << vertex;
        std::cout << '\n';
      }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-dynamic-dyck: " << error.what() << '\n';
    return 1;
  }
}
