/**
 * @file CMDOptions.cpp
 * @brief Command-line options for AserPTA pointer analysis.
 *
 * Defines global command-line options that control the behavior of the pointer
 * analysis, including debugging output, call graph construction, and points-to
 * set dumping.
 *
 * @author peiming
 */
#include <llvm/Support/CommandLine.h>

using namespace llvm;

cl::opt<bool>
    ConfigPrintConstraintGraph("consgraph",
                               cl::desc("Dump Constraint Graph to dot file"));
cl::opt<bool> ConfigPrintCallGraph("callgraph",
                                   cl::desc("Dump Call Graph to dot file"));
cl::opt<bool>
    ConfigDumpPointsToSet("dump-pts",
                          cl::desc("Dump the Points-to Set of every pointer"));
cl::opt<bool> ConfigUseOnTheFlyCallGraph(
    "on-the-fly-callgraph",
    cl::desc("Use on-the-fly call graph construction during pointer analysis"),
    cl::init(true));