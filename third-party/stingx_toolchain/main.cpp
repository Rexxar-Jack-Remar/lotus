#include "LinTS.h"
#include "lstingx.h"
#include "parser.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

extern FILE* yyin;

namespace {

void PrintUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <input.in> [lin|over]\n"
              << "  lin  : compute invariants with ComputeLinTSInv() (default)\n"
              << "  over : compute invariants with ComputeOverInv()\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        PrintUsage(argv[0]);
        return 1;
    }

    const std::string input_path = argv[1];
    const std::string mode = (argc == 3) ? argv[2] : "lin";
    if (mode != "lin" && mode != "over") {
        std::cerr << "Unknown mode: " << mode << "\n";
        PrintUsage(argv[0]);
        return 1;
    }

    FILE* input = fopen(input_path.c_str(), "r");
    if (!input) {
        std::perror(("Failed to open input file: " + input_path).c_str());
        return 1;
    }

    LinTS root;
    Initialize();

    yyin = input;
    const int parse_result = yyparse(&root);
    fclose(input);

    if (parse_result != 0) {
        std::cerr << "Parsing failed for: " << input_path << "\n";
        return 2;
    }

    if (root.getLocNum() == 0) {
        std::cerr << "No locations were parsed from: " << input_path << "\n";
        return 2;
    }

    if (mode == "over") {
        root.ComputeOverInv();
    } else {
        root.ComputeLinTSInv();
    }

    const bool empty = root.PrintInv();
    return empty ? 3 : 0;
}
