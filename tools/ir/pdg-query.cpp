/**
 * @file pdg-query.cpp
 * @brief Command-line tool for querying Program Dependence Graphs using Cypher
 *
 * This tool provides a command-line interface for executing Cypher queries
 * against Program Dependence Graphs. It supports both interactive and batch
 * modes for query execution.
 *
 * Cypher Query Examples:
 *   MATCH (n) RETURN n                          - Get all nodes
 *   MATCH (n:INST_FUNCALL) RETURN n             - Get all function call nodes
 *   MATCH (a)-[r]->(b) RETURN a, b              - Get nodes connected by edges
 *   MATCH (n:FUNC_ENTRY) WHERE n.name = 'main'  - Filter by properties
 */

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/ControlDependencyGraph.h"
#include "IR/PDG/CypherQuery.h"
#include "IR/PDG/DataDependencyGraph.h"
#include "IR/PDG/ProgramDependencyGraph.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

using namespace llvm;
using namespace pdg;

static std::string describeNode(CypherQueryExecutor &executor, Node *node) {
  if (!node)
    return "<null>";
  std::string s;
  s += executor.getNodePropertyString(node, "label");
  const std::string func = executor.getNodePropertyString(node, "func");
  if (!func.empty())
    s += " func=" + func;
  const std::string opcode = executor.getNodePropertyString(node, "opcode");
  if (!opcode.empty())
    s += " opcode=" + opcode;
  const std::string callee = executor.getNodePropertyString(node, "callee");
  if (!callee.empty())
    s += " callee=" + callee;
  const std::string src = executor.getNodePropertyString(node, "src");
  if (!src.empty())
    s += " @" + src;
  return s;
}

static std::string describeEdge(CypherQueryExecutor &executor, Edge *edge) {
  if (!edge)
    return "<null>";
  std::string s;
  s += executor.getEdgePropertyString(edge, "label");
  const std::string src = executor.getEdgePropertyString(edge, "src_src");
  const std::string dst = executor.getEdgePropertyString(edge, "dst_src");
  if (!src.empty())
    s += " src@" + src;
  if (!dst.empty())
    s += " dst@" + dst;
  return s;
}

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

static cl::opt<std::string>
    QueryString("query", "q", cl::desc("Execute a single Cypher query"),
                cl::value_desc("cypher_query"));

static cl::opt<std::string>
    QueryFile("query-file", "f", cl::desc("Execute Cypher queries from file"),
              cl::value_desc("filename"));

static cl::opt<bool> Interactive("interactive", "i",
                                 cl::desc("Run in interactive mode"));

static cl::opt<bool> Verbose("verbose", "v", cl::desc("Enable verbose output"));

static cl::opt<bool> Explain("explain", "e",
                             cl::desc("Show query execution plan"));

static cl::opt<int> Timeout("timeout", "t",
                            cl::desc("Query timeout in seconds (default: 30)"),
                            cl::init(30));

static cl::opt<bool>
    BuildPDG("build-pdg",
             cl::desc("Build full PDG (adds data/control/param edges)"),
             cl::init(true));

static cl::opt<std::string>
    TargetFunction("function", cl::desc("Target function for analysis"),
                   cl::value_desc("function_name"));

static cl::opt<int>
    ResultLimit("limit",
                cl::desc("Maximum number of results to return (default: 100)"),
                cl::init(100));

static cl::opt<std::string>
    OutputFormat("output-format",
                 cl::desc("Output format: text, json (default: text)"),
                 cl::init("text"));

static cl::opt<bool> ShowVersion("show-version",
                                 cl::desc("Show version information"));

void printVersion() {
  outs() << "PDG Cypher Query Tool v1.0\n";
  outs() << "Part of the Lotus Program Analysis Framework\n";
}

void printUsage(const char *programName) {
  printVersion();
  errs() << "\nUsage: " << programName << " [options] <input bitcode file>\n";
  errs() << "\nOptions:\n";
  errs() << "  -q, --query <query>       Execute a single Cypher query\n";
  errs() << "  -f, --query-file <file>   Execute queries from file\n";
  errs() << "  -i, --interactive         Run in interactive mode\n";
  errs() << "  -v, --verbose             Enable verbose output\n";
  errs() << "  -e, --explain             Show query execution plan\n";
  errs() << "  -t, --timeout <seconds>   Query timeout (default: 30)\n";
  errs() << "  --limit <num>             Maximum results (default: 100)\n";
}

void printPDGInfo(ProgramGraph &pdg) {
  outs() << "PDG Information:\n";
  outs() << "  Total nodes: " << pdg.numNode() << "\n";
  outs() << "  Total edges: " << pdg.numEdge() << "\n";
  outs() << "  Functions: " << pdg.getFuncWrapperMap().size() << "\n";
}


bool executeQuery(CypherQueryExecutor &executor, const std::string &queryStr) {
  if (Verbose) {
    outs() << "Executing query: " << queryStr << "\n";
  }

  auto start = std::chrono::high_resolution_clock::now();

  CypherParser parser;
  auto query = parser.parse(queryStr);

  if (!query) {
    const auto &error = parser.getLastError();
    errs() << "Parse error: " << error.message;
    if (error.line > 0) errs() << " (line " << error.line << ")";
    if (!error.suggestion.empty()) errs() << " - " << error.suggestion;
    errs() << "\n";
    return false;
  }

  if (Explain) {
    outs() << "Plan: " << query->getPatterns().size() << " patterns, "
           << query->getReturnItems().size() << " returns";
    if (query->hasWhere()) outs() << ", WHERE";
    if (query->hasLimit()) outs() << ", LIMIT " << query->getLimit();
    outs() << "\n";
  }

  // Apply limit from command line if query doesn't have one
  if (!query->hasLimit() && ResultLimit > 0) {
    const_cast<CypherQuery *>(query.get())->setLimit(ResultLimit);
  }

  auto result = executor.execute(*query);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  if (result) {
    outs() << "Result: " << result->toString() << "\n";

    const bool wantDetails = Verbose || !query->getReturnItems().empty();
    if (wantDetails && result->getType() == CypherResult::ResultType::NODES) {
      // Group RETURN items by variable name (e.g., "n", "m").
      struct Item {
        std::string var;
        std::string prop;
        std::string header;
      };
      std::vector<Item> items;
      std::vector<std::string> varsInOrder;
      std::unordered_set<std::string> seenVars;

      if (query->getReturnItems().empty()) {
        items.push_back({"n", "", "n"});
        items.push_back({"n", "func", "func"});
        items.push_back({"n", "opcode", "opcode"});
        items.push_back({"n", "callee", "callee"});
        items.push_back({"n", "src", "src"});
        varsInOrder.push_back("n");
        seenVars.insert("n");
      } else {
        for (const auto &ri : query->getReturnItems()) {
          std::string expr = ri->getVariable();
          std::string var = expr;
          std::string prop;
          auto dot = expr.find('.');
          if (dot != std::string::npos) {
            var = expr.substr(0, dot);
            prop = expr.substr(dot + 1);
          }
          const std::string header =
              ri->hasAlias() ? ri->getAlias() : ri->getVariable();
          items.push_back({var, prop, header});
          if (!var.empty() && !seenVars.count(var)) {
            varsInOrder.push_back(var);
            seenVars.insert(var);
          }
        }
      }

      for (const auto &var : varsInOrder) {
        const auto *boundEdges = executor.getBoundRelationship(var);
        const auto *boundNodes = executor.getBoundVariable(var);

        if (varsInOrder.size() > 1) {
          outs() << "RETURN " << var << ":\n";
        }

        // Header
        bool first = true;
        for (const auto &it : items) {
          if (it.var != var)
            continue;
          if (!first)
            outs() << "\t";
          outs() << it.header;
          first = false;
        }
        outs() << "\n";

        if (boundEdges) {
          const auto &edges = *boundEdges;
          const size_t limit =
              std::min(edges.size(), static_cast<size_t>(ResultLimit));
          for (size_t i = 0; i < limit; ++i) {
            Edge *e = edges[i];
            bool colFirst = true;
            for (const auto &it : items) {
              if (it.var != var)
                continue;
              if (!colFirst)
                outs() << "\t";
              if (it.prop.empty())
                outs() << describeEdge(executor, e);
              else
                outs() << executor.getEdgePropertyString(e, it.prop);
              colFirst = false;
            }
            outs() << "\n";
          }
          if (edges.size() > limit) {
            outs() << "... (" << (edges.size() - limit) << " more)\n";
          }
        } else {
          const auto &nodes = boundNodes ? *boundNodes : result->getNodes();
          const size_t limit =
              std::min(nodes.size(), static_cast<size_t>(ResultLimit));
          for (size_t i = 0; i < limit; ++i) {
            Node *n = nodes[i];
            bool colFirst = true;
            for (const auto &it : items) {
              if (it.var != var)
                continue;
              if (!colFirst)
                outs() << "\t";
              if (it.prop.empty())
                outs() << describeNode(executor, n);
              else
                outs() << executor.getNodePropertyString(n, it.prop);
              colFirst = false;
            }
            outs() << "\n";
          }
          if (nodes.size() > limit) {
            outs() << "... (" << (nodes.size() - limit) << " more)\n";
          }
        }
      }
    }

    if (Verbose) {
      const auto &stats = executor.getLastStats();
      outs() << "Time: " << duration.count() << "µs, "
             << "Nodes: " << stats.nodesVisited << ", "
             << "Edges: " << stats.edgesVisited << ", "
             << "Results: " << stats.resultsReturned << "\n";
    }
    return true;
  } else {
    errs() << "Error: " << executor.getLastError() << "\n";
    return false;
  }
}

void runInteractiveMode(CypherQueryExecutor &executor) {
  outs() << "PDG Query (type 'help' or 'quit')\n> ";

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      outs() << "> ";
      continue;
    }

    if (line == "quit" || line == "exit") {
      break;
    }

    if (line == "help") {
      outs() << "Commands: help, quit, info, clear\n";
      outs() << "Labels: :INST, :INST_FUNCALL, :INST_RET, :INST_BR, :FUNC_ENTRY, :PARAM, :VAR, :ANNO\n";
      outs() << "Edges: :DATA_DEP, :DATA_RAW, :DATA_READ, :DATA_ALIAS, :CONTROL_DEP, :CALL_INV, :CALL_RET, :PARAM_IN, :PARAM_OUT\n";
    } else if (line == "info") {
      printPDGInfo(executor.getPDG());
    } else if (line == "clear") {
      for (int i = 0; i < 50; ++i) outs() << "\n";
    } else {
      executeQuery(executor, line);
    }

    outs() << "> ";
  }
}

void runBatchMode(CypherQueryExecutor &executor, const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    errs() << "Error: Could not open file " << filename << "\n";
    return;
  }

  std::string line;
  int queryCount = 0, successCount = 0;

  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;

    size_t start = line.find_first_not_of(" \t");
    size_t end = line.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) continue;
    line = line.substr(start, end - start + 1);

    outs() << "\n[" << (++queryCount) << "] " << line << "\n";
    if (executeQuery(executor, line)) successCount++;
  }

  outs() << "\nComplete: " << successCount << "/" << queryCount << "\n";
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  cl::ParseCommandLineOptions(argc, argv, "PDG Cypher Query Tool\n");

  if (ShowVersion) {
    printVersion();
    return 0;
  }

  if (InputFilename.empty()) {
    printUsage(argv[0]);
    return 1;
  }

  // Create LLVM context and load module
  LLVMContext Context;
  SMDiagnostic Err;

  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  ProgramGraph &pdg = ProgramGraph::getInstance();
  if (BuildPDG) {
    // Ensure LLVM analysis passes are registered (required by PDG passes).
    auto &registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(registry);
    llvm::initializeAnalysis(registry);
    llvm::initializeTransformUtils(registry);

    llvm::legacy::PassManager PM;
    // Add required passes in order: DataDependencyGraph and ControlDependencyGraph
    // must run before ProgramDependencyGraph
    PM.add(new pdg::DataDependencyGraph());
    PM.add(new pdg::ControlDependencyGraph());
    PM.add(new pdg::ProgramDependencyGraph());
    PM.run(*M);
  } else {
    pdg.build(*M);
    pdg.bindDITypeToNodes(*M);
  }

  if (Verbose) {
    outs() << "Loaded: " << InputFilename << "\n";
    printPDGInfo(pdg);
  }

  // Create query executor
  CypherQueryExecutor executor(pdg);

  // Execute queries based on mode
  if (Interactive) {
    runInteractiveMode(executor);
  } else if (!QueryString.empty()) {
    executeQuery(executor, QueryString);
  } else if (!QueryFile.empty()) {
    runBatchMode(executor, QueryFile);
  } else {
    errs() << "No query specified. Use -q, -i, or -f\n";
    return 1;
  }

  return 0;
}
