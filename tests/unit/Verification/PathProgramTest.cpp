#include "Verification/PathProgram/CompressedCfg.h"
#include "Verification/PathProgram/PathProgramBuilder.h"
#include "Verification/PathProgram/PathProgramView.h"

#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

namespace lotus::verification::pathprogram {
namespace {

static std::optional<unsigned> successorIndex(const BasicBlock &source,
                                              const BasicBlock &target) {
  unsigned index = 0;
  for (const BasicBlock *successor : llvm::successors(&source)) {
    if (successor == &target) {
      return index;
    }
    ++index;
  }
  return std::nullopt;
}

static TraceTransition edge(const BasicBlock &source, const BasicBlock &target) {
  auto index = successorIndex(source, target);
  EXPECT_TRUE(index.has_value());
  return {&source, &target, *index};
}

TEST(PathProgram, BuildsFromTransitionTraceAndKeepsVisitedLocations) {
  const char *ir = R"IR(
    define void @f(i1 %c1, i1 %c2) {
    entry:
      br label %hdr
    hdr:
      br i1 %c1, label %body, label %exit
    body:
      br i1 %c2, label %hdr, label %exit
    exit:
      ret void
    }
  )IR";

  LLVMContext ctx;
  auto module = parseModuleChecked(ctx, ir, "PathProgram");
  Function *function = getFunctionChecked(*module, "f");

  const BasicBlock *entry = getBlockChecked(*function, "entry");
  const BasicBlock *hdr = getBlockChecked(*function, "hdr");
  const BasicBlock *body = getBlockChecked(*function, "body");
  const BasicBlock *exit = getBlockChecked(*function, "exit");

  std::vector<TraceTransition> trace = {
      edge(*entry, *hdr),
      edge(*hdr, *body),
      edge(*body, *hdr),
      edge(*hdr, *exit),
  };

  auto programOrErr = PathProgramBuilder::build(*function, trace);
  ASSERT_TRUE(static_cast<bool>(programOrErr))
      << toString(programOrErr.takeError());

  const PathProgram &program = *programOrErr;
  ASSERT_EQ(program.pathTransitions().size(), trace.size());
  EXPECT_EQ(program.pathTransitions()[0], trace[0]);
  EXPECT_EQ(program.pathTransitions()[2], trace[2]);
  EXPECT_EQ(program.entryLocation(), entry);
  EXPECT_EQ(program.exitLocation(), exit);

  ASSERT_EQ(program.locations().size(), 4u);
  EXPECT_EQ(program.locations()[0], entry);
  EXPECT_EQ(program.locations()[1], hdr);
  EXPECT_EQ(program.locations()[2], body);
  EXPECT_EQ(program.locations()[3], exit);
  EXPECT_TRUE(program.containsLocation(*hdr));
  EXPECT_TRUE(program.containsLocation(*exit));
}

TEST(PathProgram, TransitionsAreExactlyThoseThatOccurInTheTrace) {
  const char *ir = R"IR(
    define void @f(i1 %c1, i1 %c2, i1 %c3) {
    entry:
      br label %hdr
    hdr:
      br i1 %c1, label %body, label %exit
    body:
      br i1 %c2, label %hdr, label %after
    after:
      br i1 %c3, label %hdr, label %exit
    exit:
      ret void
    }
  )IR";

  LLVMContext ctx;
  auto module = parseModuleChecked(ctx, ir, "PathProgram");
  Function *function = getFunctionChecked(*module, "f");

  const BasicBlock *entry = getBlockChecked(*function, "entry");
  const BasicBlock *hdr = getBlockChecked(*function, "hdr");
  const BasicBlock *body = getBlockChecked(*function, "body");
  const BasicBlock *after = getBlockChecked(*function, "after");
  const BasicBlock *exit = getBlockChecked(*function, "exit");

  const TraceTransition entryToHdr = edge(*entry, *hdr);
  const TraceTransition hdrToBody = edge(*hdr, *body);
  const TraceTransition bodyToHdr = edge(*body, *hdr);
  const TraceTransition hdrToExit = edge(*hdr, *exit);
  const TraceTransition bodyToAfter = edge(*body, *after);
  const TraceTransition afterToHdr = edge(*after, *hdr);

  auto programOrErr = PathProgramBuilder::build(
      *function, {entryToHdr, hdrToBody, bodyToHdr, hdrToExit});
  ASSERT_TRUE(static_cast<bool>(programOrErr))
      << toString(programOrErr.takeError());

  const PathProgram &program = *programOrErr;
  ASSERT_EQ(program.transitions().size(), 4u);
  EXPECT_TRUE(program.containsTransition(entryToHdr));
  EXPECT_TRUE(program.containsTransition(hdrToBody));
  EXPECT_TRUE(program.containsTransition(bodyToHdr));
  EXPECT_TRUE(program.containsTransition(hdrToExit));
  EXPECT_FALSE(program.containsTransition(bodyToAfter));
  EXPECT_FALSE(program.containsTransition(afterToHdr));

  auto outgoingHdr = program.outgoingTransitions(*hdr);
  ASSERT_EQ(outgoingHdr.size(), 2u);
  EXPECT_EQ(outgoingHdr[0], hdrToBody);
  EXPECT_EQ(outgoingHdr[1], hdrToExit);
}

TEST(PathProgram, RepeatedTransitionAppearsOnceInTransitionSetButStaysInPath) {
  const char *ir = R"IR(
    define void @f(i1 %c1) {
    entry:
      br label %hdr
    hdr:
      br i1 %c1, label %body, label %exit
    body:
      br label %hdr
    exit:
      ret void
    }
  )IR";

  LLVMContext ctx;
  auto module = parseModuleChecked(ctx, ir, "PathProgram");
  Function *function = getFunctionChecked(*module, "f");

  const BasicBlock *entry = getBlockChecked(*function, "entry");
  const BasicBlock *hdr = getBlockChecked(*function, "hdr");
  const BasicBlock *body = getBlockChecked(*function, "body");
  const BasicBlock *exit = getBlockChecked(*function, "exit");

  const TraceTransition entryToHdr = edge(*entry, *hdr);
  const TraceTransition hdrToBody = edge(*hdr, *body);
  const TraceTransition bodyToHdr = edge(*body, *hdr);
  const TraceTransition hdrToExit = edge(*hdr, *exit);

  std::vector<TraceTransition> trace = {
      entryToHdr, hdrToBody, bodyToHdr, hdrToBody, bodyToHdr, hdrToExit};
  auto programOrErr = PathProgramBuilder::build(*function, trace);
  ASSERT_TRUE(static_cast<bool>(programOrErr))
      << toString(programOrErr.takeError());

  const PathProgram &program = *programOrErr;
  ASSERT_EQ(program.pathTransitions().size(), trace.size());
  ASSERT_EQ(program.transitions().size(), 4u);
  EXPECT_EQ(program.pathTransitions()[1], hdrToBody);
  EXPECT_EQ(program.pathTransitions()[3], hdrToBody);
  EXPECT_EQ(program.pathTransitions()[4], bodyToHdr);
}

TEST(PathProgram, RejectsDisconnectedOrInvalidTransitionTrace) {
  const char *ir = R"IR(
    define void @f(i1 %c1) {
    entry:
      br i1 %c1, label %mid, label %exit
    mid:
      br label %exit
    exit:
      ret void
    }
  )IR";

  LLVMContext ctx;
  auto module = parseModuleChecked(ctx, ir, "PathProgram");
  Function *function = getFunctionChecked(*module, "f");

  const BasicBlock *entry = getBlockChecked(*function, "entry");
  const BasicBlock *mid = getBlockChecked(*function, "mid");
  const BasicBlock *exit = getBlockChecked(*function, "exit");

  const TraceTransition entryToExit = edge(*entry, *exit);
  const TraceTransition midToExit = edge(*mid, *exit);

  auto disconnected =
      PathProgramBuilder::build(*function, {entryToExit, midToExit});
  EXPECT_FALSE(static_cast<bool>(disconnected));
  EXPECT_EQ(toString(disconnected.takeError()),
            "transition trace is not a connected path");

  TraceTransition invalid{entry, mid, 3};
  auto invalidTrace = PathProgramBuilder::build(*function, {invalid});
  EXPECT_FALSE(static_cast<bool>(invalidTrace));
  EXPECT_EQ(toString(invalidTrace.takeError()),
            "transition trace contains an invalid CFG transition");
}

TEST(CompressedCfg, UtilityStillCompressesSccs) {
  const char *ir = R"IR(
    define void @f(i1 %c1, i1 %c2) {
    entry:
      br i1 %c1, label %loop.hdr, label %exit
    loop.hdr:
      br label %loop.body
    loop.body:
      br i1 %c2, label %loop.hdr, label %after
    after:
      br label %exit
    exit:
      ret void
    }
  )IR";

  LLVMContext ctx;
  auto module = parseModuleChecked(ctx, ir, "CompressedCfg");
  Function *function = getFunctionChecked(*module, "f");

  CompressedCfg cfg = CompressedCfg::build(*function);

  ASSERT_EQ(cfg.nodes().size(), 4u);
  ASSERT_EQ(cfg.edges().size(), 4u);

  const BasicBlock *loopHdr = getBlockChecked(*function, "loop.hdr");
  const BasicBlock *loopBody = getBlockChecked(*function, "loop.body");
  auto loopNode = cfg.nodeContaining(*loopHdr);
  auto bodyNode = cfg.nodeContaining(*loopBody);
  ASSERT_TRUE(loopNode.has_value());
  ASSERT_TRUE(bodyNode.has_value());
  EXPECT_EQ(*loopNode, *bodyNode);
}

} // namespace
} // namespace lotus::verification::pathprogram
