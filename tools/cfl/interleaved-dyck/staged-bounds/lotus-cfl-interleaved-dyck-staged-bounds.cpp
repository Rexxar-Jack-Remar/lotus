#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/StagedBounds/Solver.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace approximation = lotus::cfl::interleaved_dyck::staged_bounds;
namespace interleaved_dyck = lotus::cfl::interleaved_dyck;

namespace {

enum class PrintedPairs { None, Lower, Result };

struct CommandLine {
  std::string input;
  std::string output;
  approximation::Method method = approximation::Method::All;
  unsigned parity_groups = 2;
  bool value_flow = false;
  bool factorized_tracing = false;
  PrintedPairs printed_pairs = PrintedPairs::None;
};

void usage(std::ostream &output) {
  output << "usage: lotus-cfl-interleaved-dyck-staged-bounds [options] "
            "<graph.dot>\n"
            "\n"
            "Compute staged lower/upper typed interleaved-Dyck bounds.\n"
            "\n"
            "options:\n"
            "  --value-flow       use value-flow benchmark preprocessing\n"
            "  --method NAME      regularization, intersection, "
            "underapproximation,\n"
            "                     mutual-refinement, stronger-grammar, "
            "on-demand,\n"
            "                     or all (default: all)\n"
            "  --parity-groups N  parity groups, 1-4 (default: 2)\n"
            "  --factorized-tracing\n"
            "                     reconstruct provenance from CFL closure\n"
            "  --print-lower      print certified lower-bound pairs\n"
            "  --print-result     print pairs produced by the selected method\n"
            "  -o FILE            write output to FILE\n"
            "  -h, --help         show this help\n";
}

approximation::Method parseMethod(std::string_view text) {
  if (text == "all") {
    return approximation::Method::All;
  }
  if (text == "regularization") {
    return approximation::Method::Regularization;
  }
  if (text == "intersection") {
    return approximation::Method::Intersection;
  }
  if (text == "underapproximation") {
    return approximation::Method::Underapproximation;
  }
  if (text == "mutual-refinement") {
    return approximation::Method::MutualRefinement;
  }
  if (text == "stronger-grammar") {
    return approximation::Method::StrongerGrammar;
  }
  if (text == "on-demand") {
    return approximation::Method::OnDemand;
  }
  throw std::invalid_argument("unknown method: " + std::string(text));
}

unsigned parseUnsigned(std::string_view text, std::string_view option) {
  unsigned value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid value for " + std::string(option));
  }
  return value;
}

CommandLine parseCommandLine(int argc, char **argv) {
  CommandLine result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "-h" || argument == "--help") {
      usage(std::cout);
      std::exit(0);
    }
    if (argument == "--value-flow") {
      result.value_flow = true;
      continue;
    }
    if (argument == "--method") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --method");
      }
      result.method = parseMethod(argv[i]);
      continue;
    }
    if (argument == "--parity-groups") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --parity-groups");
      }
      result.parity_groups = parseUnsigned(argv[i], argument);
      continue;
    }
    if (argument == "--factorized-tracing") {
      result.factorized_tracing = true;
      continue;
    }
    if (argument == "--print-lower") {
      result.printed_pairs = PrintedPairs::Lower;
      continue;
    }
    if (argument == "--print-result") {
      result.printed_pairs = PrintedPairs::Result;
      continue;
    }
    if (argument == "-o") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for -o");
      }
      result.output = argv[i];
      continue;
    }
    if (!argument.empty() && argument.front() == '-') {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
    if (!result.input.empty()) {
      throw std::invalid_argument("more than one input graph was provided");
    }
    result.input = argument;
  }
  if (result.input.empty()) {
    throw std::invalid_argument("no input graph was provided");
  }
  return result;
}

const char *methodLabel(approximation::Method method) {
  switch (method) {
  case approximation::Method::Regularization:
    return "regularization";
  case approximation::Method::Intersection:
    return "intersection upper bound";
  case approximation::Method::Underapproximation:
    return "certified lower bound";
  case approximation::Method::MutualRefinement:
    return "mutual-refinement upper bound";
  case approximation::Method::StrongerGrammar:
    return "stronger-grammar upper bound";
  case approximation::Method::OnDemand:
  case approximation::Method::All:
    return "final upper bound";
  }
  throw std::logic_error("unhandled approximation method");
}

const approximation::PairSet &
methodResult(const approximation::ApproximationResult &result,
             approximation::Method method) {
  switch (method) {
  case approximation::Method::Regularization:
    return result.regularization;
  case approximation::Method::Intersection:
    return result.intersection;
  case approximation::Method::Underapproximation:
    return result.underapproximation;
  case approximation::Method::MutualRefinement:
    return result.mutual_refinement;
  case approximation::Method::StrongerGrammar:
    return result.stronger_grammar;
  case approximation::Method::OnDemand:
  case approximation::Method::All:
    return result.on_demand;
  }
  throw std::logic_error("unhandled approximation method");
}

void printResult(std::ostream &output, const CommandLine &command_line,
                 const approximation::ApproximationResult &result,
                 std::int64_t elapsed_ms) {
  if (command_line.method == approximation::Method::All) {
    output << "regularization: " << result.regularization.size() << '\n'
           << "intersection upper bound: " << result.intersection.size() << '\n'
           << "certified lower bound: " << result.underapproximation.size()
           << '\n'
           << "mutual-refinement upper bound: "
           << result.mutual_refinement.size() << '\n'
           << "stronger-grammar upper bound: " << result.stronger_grammar.size()
           << '\n'
           << "final upper bound: " << result.on_demand.size() << '\n';
  } else {
    output << methodLabel(command_line.method) << ": "
           << methodResult(result, command_line.method).size() << '\n';
  }
  output << "Time (ms): " << elapsed_ms << '\n';

  const approximation::PairSet *pairs = nullptr;
  if (command_line.printed_pairs == PrintedPairs::Lower) {
    pairs = &result.underapproximation;
  } else if (command_line.printed_pairs == PrintedPairs::Result) {
    pairs = &methodResult(result, command_line.method);
  }
  if (pairs != nullptr) {
    for (const approximation::Pair &pair : *pairs) {
      output << pair.source << ' ' << pair.target << '\n';
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const CommandLine command_line = parseCommandLine(argc, argv);
    const interleaved_dyck::Graph graph =
        interleaved_dyck::Graph::parseDotFile(command_line.input);

    approximation::Options options;
    options.method = command_line.method;
    options.parity_groups = command_line.parity_groups;
    options.factorized_tracing = command_line.factorized_tracing;
    const auto start = std::chrono::steady_clock::now();
    const approximation::ApproximationResult result =
        approximation::Solver{}.analyze(
            graph,
            command_line.value_flow ? approximation::BenchmarkKind::ValueFlow
                                    : approximation::BenchmarkKind::Taint,
            options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    std::ofstream output_file;
    std::ostream *output = &std::cout;
    if (!command_line.output.empty()) {
      output_file.open(command_line.output);
      if (!output_file) {
        throw std::runtime_error("cannot open output file: " +
                                 command_line.output);
      }
      output = &output_file;
    }
    printResult(*output, command_line, result, elapsed.count());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-interleaved-dyck-staged-bounds: " << error.what()
              << '\n';
    return 1;
  }
}
