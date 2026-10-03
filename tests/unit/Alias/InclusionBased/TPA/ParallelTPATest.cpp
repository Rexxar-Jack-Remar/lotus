#include "Alias/InclusionBased/TPA/Context/ContextPolicy.h"
#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/WorkList.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/CallGraph.h"
#include "Alias/InclusionBased/TPA/Transforms/RunPrepass.h"
#include "TestUtils/LLVMHelpers.h"

#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace llvm;
using namespace tpa;
using namespace lotus::unittest;

class ParallelTPATest : public LlvmModuleTest {
protected:
  void TearDown() override {
    context::KLimitContext::setLimit(0);
    context::setContextStrategy(context::ContextStrategy::KLimit);
  }
  void compare(const char *source, unsigned limit = 0,
               bool externalModels = false) {
    context::KLimitContext::setLimit(limit);
    context::setContextStrategy(context::ContextStrategy::KLimit);
    auto module = parseModule(source);
    ASSERT_NE(module, nullptr);
    transform::runPrepassOn(*module);
    SemiSparseProgramBuilder builder;
    auto program = builder.runOnModule(*module);
    // __FILE__ is absolute in the collected test manifest, independent of cwd.
    auto spec = std::filesystem::path(__FILE__);
    for (unsigned i = 0; i < 6; ++i)
      spec = spec.parent_path();
    spec /= "config/ptr.spec";
    SemiSparsePointerAnalysis serial;
    if (externalModels)
      serial.loadExternalPointerTable(spec.string().c_str());
    serial.runOnProgram(program);
    for (unsigned threads : {1U, 2U, 4U}) {
      for (unsigned lookahead : {1U, 4U}) {
        SCOPED_TRACE(threads);
        SCOPED_TRACE(lookahead);
        SemiSparsePointerAnalysis parallel;
        if (externalModels)
          parallel.loadExternalPointerTable(spec.string().c_str());
        SemiSparsePointerAnalysis::Config config;
        config.parallel = true;
        config.threads = threads;
        config.lookahead = lookahead;
        parallel.runOnProgram(program, config);
        std::string difference;
        EXPECT_TRUE(parallel.hasSameSolution(serial, &difference))
            << difference;
        EXPECT_EQ(parallel.getStatistics().transfers,
                  serial.getStatistics().transfers);
        EXPECT_LE(parallel.getStatistics().peakWorkers, threads);
      }
    }
  }
};

TEST_F(ParallelTPATest, StrongUpdatesAndLoads) {
  compare(R"(
    define i8* @main() {
      %x = alloca i8
      %y = alloca i8
      %slot = alloca i8*
      store i8* %x, i8** %slot
      store i8* %y, i8** %slot
      %r = load i8*, i8** %slot
      ret i8* %r
    }
  )");
}

TEST_F(ParallelTPATest, ContextArgumentsAndReturns) {
  compare(R"(
    define i8* @id(i8* %p) { ret i8* %p }
    define i8* @main() {
      %x = alloca i8
      %y = alloca i8
      %a = call i8* @id(i8* %x)
      %b = call i8* @id(i8* %y)
      ret i8* %b
    }
  )",
          2);
}

TEST_F(ParallelTPATest, SingletonDestinationGrowsAcrossLoop) {
  compare(R"(
    define i8* @main(i1 %again) {
    entry:
      %x = alloca i8
      %y = alloca i8
      %a = alloca i8*
      %b = alloca i8*
      store i8* %x, i8** %a
      store i8* %y, i8** %b
      br label %loop
    loop:
      %dst = phi i8** [ %a, %entry ], [ %b, %loop ]
      %src = phi i8* [ %x, %entry ], [ %y, %loop ]
      store i8* %src, i8** %dst
      br i1 %again, label %loop, label %exit
    exit:
      %r = load i8*, i8** %a
      ret i8* %r
    }
  )");
}

TEST_F(ParallelTPATest, IndirectTargetsAndReturnedMemory) {
  compare(R"(
    @slot = global i8* null
    define i8* @left(i8* %p) { store i8* %p, i8** @slot ret i8* %p }
    define i8* @right(i8* %p) { ret i8* %p }
    define i8* @main(i1 %choice) {
      %x = alloca i8
      %fp = select i1 %choice, i8* (i8*)* @left, i8* (i8*)* @right
      %a = call i8* %fp(i8* %x)
      %b = load i8*, i8** @slot
      ret i8* %b
    }
  )",
          1);
}

TEST_F(ParallelTPATest, RecursiveContextAndPhiCycle) {
  compare(R"(
    define i8* @rec(i8* %p, i1 %stop) {
    entry:
      br i1 %stop, label %exit, label %again
    again:
      %r = call i8* @rec(i8* %p, i1 true)
      br label %exit
    exit:
      %v = phi i8* [ %p, %entry ], [ %r, %again ]
      ret i8* %v
    }
    define i8* @main() {
      %x = alloca i8
      %r = call i8* @rec(i8* %x, i1 false)
      ret i8* %r
    }
  )",
          1);
}

TEST_F(ParallelTPATest, GlobalInitializersAndFieldOffsets) {
  compare(R"(
    @x = global i8 0
    @pair = global {i8*, i8*} {i8* @x, i8* null}
    define i8* @main() {
      %a = getelementptr {i8*, i8*}, {i8*, i8*}* @pair, i64 0, i32 0
      %b = getelementptr {i8*, i8*}, {i8*, i8*}* @pair, i64 0, i32 1
      %p = load i8*, i8** %a
      store i8* %p, i8** %b
      %r = load i8*, i8** %b
      ret i8* %r
    }
  )");
}

TEST_F(ParallelTPATest, PointerViewsHidePredictedRegistrations) {
  auto module = parseModule("define i8* @main(i8* %p) { ret i8* %p }");
  ASSERT_NE(module, nullptr);
  PointerManager owner;
  PointerManager first(owner), second(owner);
  auto *value = &*module->getFunction("main")->arg_begin();
  const auto *ctx = context::Context::getGlobalContext();
  EXPECT_EQ(second.getPointer(ctx, value), nullptr);
  const auto *pointer = first.getOrCreatePointer(ctx, value);
  ASSERT_NE(pointer, nullptr);
  EXPECT_EQ(owner.getPointer(ctx, value), nullptr);
  EXPECT_EQ(second.getPointer(ctx, value), nullptr);
  EXPECT_TRUE(second.validateView());
  first.publishView();
  EXPECT_EQ(owner.getPointer(ctx, value), pointer);
  EXPECT_FALSE(second.validateView());
}

TEST_F(ParallelTPATest, ExternalAllocationAndMemcpy) {
  compare(R"(
    declare i8* @malloc(i64)
    declare i8* @memcpy(i8*, i8*, i64)
    define i8* @main() {
      %x = alloca i8
      %src = alloca i8*
      store i8* %x, i8** %src
      %raw = call i8* @malloc(i64 8)
      %dst = bitcast i8* %raw to i8**
      %s = bitcast i8** %src to i8*
      %unused = call i8* @memcpy(i8* %raw, i8* %s, i64 8)
      %r = load i8*, i8** %dst
      ret i8* %r
    }
  )",
          1, true);
}

TEST_F(ParallelTPATest, UnknownExternalAndCyclicMemoryFlow) {
  compare(R"(
    declare void @unknown(i8**)
    define i8* @main(i1 %again) {
    entry:
      %x = alloca i8
      %y = alloca i8
      %slot = alloca i8*
      store i8* %x, i8** %slot
      br label %loop
    loop:
      %p = phi i8* [ %x, %entry ], [ %y, %loop ]
      store i8* %p, i8** %slot
      call void @unknown(i8** %slot)
      br i1 %again, label %loop, label %exit
    exit:
      %r = load i8*, i8** %slot
      ret i8* %r
    }
  )");
}

TEST_F(ParallelTPATest, EmptyBindingPresenceInvalidatesPrediction) {
  auto module = parseModule("define i8* @main(i8* %p) { ret i8* %p }");
  ASSERT_NE(module, nullptr);
  PointerManager manager;
  const auto *pointer =
      manager.getOrCreatePointer(context::Context::getGlobalContext(),
                                 &*module->getFunction("main")->arg_begin());
  Env owner;
  auto view = owner.makeOverlay();
  EXPECT_TRUE(view.weakUpdate(pointer, PtsSet::getEmptySet()));
  EXPECT_TRUE(view.validateOverlay(owner));
  owner.weakUpdate(pointer, PtsSet::getEmptySet());
  EXPECT_FALSE(view.validateOverlay(owner));
  auto fresh = owner.makeOverlay();
  EXPECT_FALSE(fresh.weakUpdate(pointer, PtsSet::getEmptySet()));
}

TEST_F(ParallelTPATest, ConcurrentSetInterningPreservesCanonicalEquality) {
  const auto *object = MemoryManager::getUniversalObject();
  std::vector<PtsSet> sets(64, PtsSet::getEmptySet());
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 4; ++worker)
    workers.emplace_back([&, worker] {
      for (unsigned i = worker; i < sets.size(); i += 4)
        sets[i] = PtsSet::getSingletonSet(object);
    });
  for (auto &worker : workers)
    worker.join();
  for (const auto &set : sets)
    EXPECT_EQ(set, sets.front());
}

TEST_F(ParallelTPATest, InvalidConfigurationAndSingleRunAreExplicit) {
  auto module = parseModule("define i32 @main() { ret i32 0 }");
  ASSERT_NE(module, nullptr);
  SemiSparseProgramBuilder builder;
  auto program = builder.runOnModule(*module);
  SemiSparsePointerAnalysis analysis;
  SemiSparsePointerAnalysis::Config config;
  config.parallel = true;
  config.lookahead = 0;
  EXPECT_THROW(analysis.runOnProgram(program, config), std::invalid_argument);
  config.lookahead = 1;
  config.threads = 2;
  analysis.runOnProgram(program, config);
  EXPECT_THROW(analysis.runOnProgram(program, config), std::logic_error);
}

TEST_F(ParallelTPATest, CallGraphOverlayPublishesOnlyNewEdges) {
  CallGraph<int, int> owner;
  ASSERT_TRUE(owner.insertEdge(1, 10));
  ASSERT_TRUE(owner.insertEdge(2, 10));
  const auto revision = owner.getRevision();
  auto view = owner.makeOverlay();
  EXPECT_FALSE(view.insertEdge(1, 10));
  EXPECT_TRUE(view.insertEdge(1, 20));
  EXPECT_FALSE(view.insertEdge(1, 20));
  EXPECT_TRUE(view.insertEdge(3, 10));
  auto collect = [](const auto &range) {
    return std::vector<int>(range.begin(), range.end());
  };
  EXPECT_EQ(collect(owner.getCallees(1)), (std::vector<int>{10}));
  EXPECT_EQ(collect(owner.getCallers(10)), (std::vector<int>{1, 2}));
  EXPECT_EQ(collect(view.getCallees(1)), (std::vector<int>{10, 20}));
  EXPECT_EQ(collect(view.getCallers(10)), (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(view.getRevision(), revision + 2);
  view.commitOverlayTo(owner);
  EXPECT_EQ(owner.getRevision(), revision + 2);
  EXPECT_EQ(collect(owner.getCallees(1)), (std::vector<int>{10, 20}));
  EXPECT_EQ(collect(owner.getCallers(10)), (std::vector<int>{1, 2, 3}));
}

TEST_F(ParallelTPATest, MemoSnapshotsSurviveGrowthAndSkipUnchangedJoins) {
  auto module = parseModule("define i32 @main() { ret i32 0 }");
  ASSERT_NE(module, nullptr);
  SemiSparseProgramBuilder builder;
  auto program = builder.runOnModule(*module);
  const auto point = ProgramPoint(context::Context::getGlobalContext(),
                                  program.getEntryCFG()->getEntryNode());
  const auto *cell = MemoryManager::getUniversalObject();
  const auto *other = MemoryManager::getNullObject();
  Memo memo;
  EXPECT_EQ(memo.snapshot(point), nullptr);
  Store incoming;
  incoming.strongUpdate(cell, PtsSet::getSingletonSet(other));
  ASSERT_TRUE(memo.update(point, incoming));
  auto first = memo.snapshot(point);
  EXPECT_FALSE(memo.update(point, incoming));
  EXPECT_EQ(memo.lookup(point), first.get());
  ASSERT_TRUE(memo.update(point, cell, PtsSet::getSingletonSet(cell)));
  auto grown = memo.snapshot(point);
  EXPECT_NE(grown.get(), first.get());
  EXPECT_EQ(first->lookup(cell), PtsSet::getSingletonSet(other));
  EXPECT_EQ(grown->lookup(cell), PtsSet::getSingletonSet(cell));
  EXPECT_FALSE(memo.update(point, cell, PtsSet::getSingletonSet(other)));
  EXPECT_EQ(memo.lookup(point), grown.get());
  Store emptyBinding;
  emptyBinding.strongUpdate(other, PtsSet::getEmptySet());
  EXPECT_TRUE(memo.update(point, emptyBinding));
  EXPECT_NE(memo.lookup(point), grown.get());
  EXPECT_FALSE(grown->hasBinding(other));
  EXPECT_TRUE(memo.lookup(point)->hasBinding(other));
}

TEST_F(ParallelTPATest, WorklistPreviewPreservesOrderAndPriorityTies) {
  auto module = parseModule(R"(
    define i8* @id(i8* %p) { ret i8* %p }
    define i8* @main() {
      %x = alloca i8
      %slot = alloca i8*
      store i8* %x, i8** %slot
      %r = call i8* @id(i8* %x)
      ret i8* %r
    }
  )");
  ASSERT_NE(module, nullptr);
  SemiSparseProgramBuilder builder;
  auto program = builder.runOnModule(*module);
  struct TiedPriority {
    bool operator()(const CFGNode *, const CFGNode *) const { return false; }
  };
  auto check = [&](auto worklist) {
    for (const auto &cfg : program)
      for (const auto &node : cfg) {
        const auto point =
            ProgramPoint(context::Context::getGlobalContext(), node);
        worklist.enqueue(point);
        worklist.enqueue(point); // Preview must respect duplicate suppression.
      }
    auto reference = worklist;
    std::vector<ProgramPoint> expected;
    while (!reference.empty())
      expected.push_back(reference.dequeue());
    for (std::size_t limit : {std::size_t{0}, std::size_t{1}, std::size_t{3},
                              expected.size() + 2}) {
      auto prefix = worklist.peek(limit);
      EXPECT_EQ(prefix,
                std::vector<ProgramPoint>(
                    expected.begin(),
                    expected.begin() + std::min(limit, expected.size())));
      EXPECT_EQ(worklist.front(), expected.front());
    }
    EXPECT_EQ(worklist.peek(expected.size()), expected);
  };
  check(ForwardWorkList{});
  check(IDFAWorkList<TiedPriority>{});
}
