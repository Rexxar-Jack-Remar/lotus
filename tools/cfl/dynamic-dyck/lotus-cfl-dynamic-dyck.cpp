#include "CFL/DynamicDyck/IO.h"

#include <exception>
#include <iostream>
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
            "0=recompute, 1=dynamic. Default output is elapsed seconds and a "
            "space,\n"
            "matching the original DyckReach binary.\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    bool dynamic = true, print_components = false, print_stats = false;
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
      else if (argument.compare(0, 2, "--") == 0)
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
    const auto result = dyck::runFiles(dynamic, paths[0], paths[1]);
    if (print_stats) {
      const auto &stats = result.statistics;
      std::cout << "mode=" << (dynamic ? "dynamic" : "recompute")
                << " vertices=" << stats.vertices << " edges=" << stats.edges
                << " components=" << stats.components
                << " updates=" << result.updates
                << " changed_updates=" << stats.insertions + stats.deletions
                << " cycle_rebuilds=" << stats.cycle_rebuilds
                << " elapsed_seconds=" << result.elapsed_seconds << '\n';
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
