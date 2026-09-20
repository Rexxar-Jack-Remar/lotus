#include "Alias/InclusionBased/GPG/Analysis.h"
#include "Alias/InclusionBased/GPG/Graph.h"
#include "Alias/InclusionBased/GPG/LLVMFrontend.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "TestUtils/LLVMHelpers.h"

#include <set>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

using namespace lotus::gpg;
using namespace lotus::unittest;

namespace {

Access access(LocationId id, unsigned indirections,
              bool upward_exposed = false) {
  return {id, IndirectionList::dereferences(indirections), upward_exposed,
          nullptr};
}

GPU gpu(LocationId source, unsigned source_indirections, LocationId target,
        unsigned target_indirections, StatementId statement) {
  GPU result;
  result.source = access(source, source_indirections);
  result.target = access(target, target_indirections);
  result.statement = statement;
  return result;
}

bool allTypesCompatible(const llvm::Type *, const llvm::Type *) { return true; }

} // namespace

TEST(GPGIndirectionList, SummarizedRemainderMatchesAppendixB8) {
  const Indirection dereference = Indirection::dereference();
  const Indirection field_n = Indirection::fieldAt(7);
  IndirectionList summarized({dereference, field_n, field_n}, true);

  std::vector<IndirectionList> one =
      summarized.remaindersAfter(IndirectionList({dereference}), 3);
  ASSERT_EQ(one.size(), 2u);
  EXPECT_EQ(one[0], IndirectionList({field_n, field_n}));
  EXPECT_EQ(one[1],
            IndirectionList({field_n, field_n, Indirection::anyField()}, true));

  std::vector<IndirectionList> two =
      summarized.remaindersAfter(IndirectionList({dereference, field_n}), 3);
  ASSERT_EQ(two.size(), 3u);
  EXPECT_EQ(two[0], IndirectionList({field_n}));
  EXPECT_EQ(two[1], IndirectionList({field_n, Indirection::anyField()}));
  EXPECT_EQ(two[2], IndirectionList({field_n, Indirection::anyField(),
                                     Indirection::anyField()},
                                    true));
}

TEST(GPGComposition, TargetSourceCompositionReducesIndirection) {
  GPU consumer = gpu(1, 1, 2, 2, 23);
  GPU producer = gpu(2, 1, 3, 0, 21);

  CompositionResult result =
      composeGPU(consumer, producer, CompositionKind::TargetSource);
  ASSERT_TRUE(result.valid);
  ASSERT_TRUE(result.desirable);
  ASSERT_EQ(result.gpus.size(), 1u);
  const GPU &composed = *result.gpus.begin();
  EXPECT_EQ(composed.source, access(1, 1));
  EXPECT_EQ(composed.target, access(3, 1));
  EXPECT_EQ(composed.statement, 23u);
}

TEST(GPGComposition, SourceSourceCompositionReducesStore) {
  GPU consumer = gpu(1, 2, 4, 1, 2);
  GPU producer = gpu(1, 1, 3, 0, 1);

  CompositionResult result =
      composeGPU(consumer, producer, CompositionKind::SourceSource);
  ASSERT_TRUE(result.valid);
  ASSERT_TRUE(result.desirable);
  ASSERT_EQ(result.gpus.size(), 1u);
  const GPU &composed = *result.gpus.begin();
  EXPECT_EQ(composed.source, access(3, 1));
  EXPECT_EQ(composed.target, access(4, 1));
}

TEST(GPGReduction, PerformsAChainOfCompositions) {
  GPU consumer = gpu(1, 1, 2, 2, 23);
  GPU first = gpu(2, 1, 3, 0, 21);
  GPU second = gpu(3, 1, 4, 0, 22);
  GPUSet context = {first, second};

  ReductionResult result = reduceGPU(consumer, context, context);
  ASSERT_EQ(result.reduced.size(), 1u);
  EXPECT_EQ(*result.reduced.begin(), gpu(1, 1, 4, 0, 23));
  EXPECT_TRUE(result.queued.empty());
}

TEST(GPGReduction, QueuesBlockedProducerForLaterInlining) {
  GPU consumer = gpu(1, 1, 2, 1, 4);
  GPU producer = gpu(2, 1, 3, 0, 2);

  ReductionResult result = reduceGPU(consumer, {}, {producer});
  EXPECT_EQ(result.reduced, GPUSet({consumer}));
  EXPECT_EQ(result.queued, GPUSet({producer}));
}

TEST(GPGReachingAnalysis, BarrierPostponesUnsoundComposition) {
  GPG graph;
  graph.addBlock({1, GPBKind::Start});

  GPB definition{2, GPBKind::Normal};
  definition.gpus.insert(gpu(2, 1, 3, 0, 2));
  definition.original_gpus = definition.gpus;
  graph.addBlock(definition);

  GPB barrier{3, GPBKind::Normal};
  barrier.gpus.insert(gpu(4, 2, 5, 0, 3));
  barrier.original_gpus = barrier.gpus;
  graph.addBlock(barrier);

  GPB use{4, GPBKind::Normal};
  use.gpus.insert(gpu(1, 1, 2, 1, 4));
  use.original_gpus = use.gpus;
  graph.addBlock(use);
  graph.addBlock({5, GPBKind::End});

  graph.setEntry(1);
  graph.setExit(5);
  graph.addEdge(1, 2);
  graph.addEdge(2, 3);
  graph.addEdge(3, 4);
  graph.addEdge(4, 5);

  Access y = access(2, 1);
  graph.setBoundaryDefinitions({GPG::makeBoundaryDefinition(y)});

  ReachingPair reaching = graph.analyzeReaching(allTypesCompatible, 3);
  EXPECT_EQ(reaching.with_blocking.blocked[3], GPUSet({gpu(2, 1, 3, 0, 2)}));
  EXPECT_EQ(reaching.without_blocking.generated[4],
            GPUSet({gpu(1, 1, 3, 0, 4)}));
  EXPECT_EQ(reaching.with_blocking.generated[4],
            GPUSet({GPU{access(1, 1), access(2, 1, true), 4}}));
  EXPECT_EQ(reaching.with_blocking.queued, GPUSet({gpu(2, 1, 3, 0, 2)}));
}

TEST(GPGGraph, EliminatesAnEmptyBlockWithoutChangingPaths) {
  GPG graph;
  graph.addBlock({1, GPBKind::Start});
  graph.addBlock({2, GPBKind::Normal});
  graph.addBlock({3, GPBKind::End});
  graph.setEntry(1);
  graph.setExit(3);
  graph.addEdge(1, 2);
  graph.addEdge(2, 3);

  ASSERT_TRUE(graph.eliminateEmptyGPBs());
  EXPECT_FALSE(graph.hasBlock(2));
  EXPECT_EQ(graph.successors(1), GPBIdSet({3}));
  EXPECT_TRUE(graph.validate());
}

TEST(GPGGraph, EliminatesGPUKilledOnEveryPathToExit) {
  GPG graph;
  graph.addBlock({1, GPBKind::Start});
  GPB first{2, GPBKind::Normal};
  first.gpus.insert(gpu(1, 1, 2, 0, 1));
  first.original_gpus = first.gpus;
  graph.addBlock(first);
  GPB second{3, GPBKind::Normal};
  second.gpus.insert(gpu(1, 1, 3, 0, 2));
  second.original_gpus = second.gpus;
  graph.addBlock(second);
  graph.addBlock({4, GPBKind::End});
  graph.setEntry(1);
  graph.setExit(4);
  graph.addEdge(1, 2);
  graph.addEdge(2, 3);
  graph.addEdge(3, 4);

  ASSERT_TRUE(graph.eliminateDeadGPUs(allTypesCompatible, 3));
  EXPECT_TRUE(graph.getBlock(2)->gpus.empty());
  EXPECT_EQ(graph.getBlock(3)->gpus, GPUSet({gpu(1, 1, 3, 0, 2)}));
}

TEST(GPGGraph, CoalescesIndependentBlocksButPreservesWaWOrdering) {
  auto make_graph = [](bool same_source) {
    GPG graph;
    graph.addBlock({1, GPBKind::Start});
    GPB first{2, GPBKind::Normal};
    first.gpus.insert(gpu(1, 1, 3, 0, 1));
    first.original_gpus = first.gpus;
    graph.addBlock(first);
    GPB second{3, GPBKind::Normal};
    second.gpus.insert(gpu(same_source ? 1 : 2, 1, 4, 0, 2));
    second.original_gpus = second.gpus;
    graph.addBlock(second);
    graph.addBlock({4, GPBKind::End});
    graph.setEntry(1);
    graph.setExit(4);
    graph.addEdge(1, 2);
    graph.addEdge(2, 3);
    graph.addEdge(3, 4);
    return graph;
  };

  GPG independent = make_graph(false);
  ASSERT_TRUE(independent.coalesce(allTypesCompatible, 3));
  EXPECT_EQ(independent.blocks().size(), 1u);
  EXPECT_TRUE(independent.validate());

  GPG dependent = make_graph(true);
  ASSERT_TRUE(dependent.coalesce(allTypesCompatible, 3));
  EXPECT_GT(dependent.blocks().size(), 1u);
  EXPECT_TRUE(dependent.validate());
}

TEST(GPGLLVMFrontend, BuildsAndReducesLLVMStoresAndLoads) {
  const char *ir = R"(
    define i8* @main() {
    entry:
      %cell = alloca i8*
      %object = alloca i8
      store i8* %object, i8** %cell
      %loaded = load i8*, i8** %cell
      ret i8* %loaded
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGLLVMFrontend");
  ASSERT_NE(module, nullptr);
  ProgramModel model(*module);
  GPGConfig config;
  LLVMFrontend frontend(model, config);
  llvm::Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  GPG graph = frontend.buildInitialGPG(*main);
  ASSERT_TRUE(graph.validate());
  auto compatible = [&](const llvm::Type *lhs, const llvm::Type *rhs) {
    return model.typesCompatible(lhs, rhs);
  };
  EXPECT_TRUE(graph.strengthReduce(compatible, config.heap_indirection_limit));

  llvm::Instruction *load = findInstructionByName(main, "loaded");
  ASSERT_NE(load, nullptr);
  const StatementId statement = model.statementId(load);
  const LocationId loaded_location = model.valueLocation(load);
  llvm::Instruction *object = findInstructionByName(main, "object");
  ASSERT_NE(object, nullptr);
  const LocationId object_location = model.objectLocation(object);

  bool found = false;
  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    for (const GPU &update : block.gpus) {
      if (update.statement != statement)
        continue;
      found |= update.source.location == loaded_location &&
               update.source.indirections == IndirectionList::dereferences(1) &&
               update.target.location == object_location &&
               update.target.indirections.empty();
    }
  }
  EXPECT_TRUE(found);
}

TEST(GPGLLVMFrontend, PreservesStructFieldsAndFunctionAddresses) {
  const char *ir = R"(
    %S = type { i32 (i32)*, i32 (i32)* }

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %s = alloca %S
      %field = getelementptr %S, %S* %s, i32 0, i32 1
      store i32 (i32)* @foo, i32 (i32)** %field
      %loaded = load i32 (i32)*, i32 (i32)** %field
      %result = call i32 %loaded(i32 7)
      ret i32 %result
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGLLVMFrontend");
  ASSERT_NE(module, nullptr);
  ProgramModel model(*module);
  GPGConfig config;
  LLVMFrontend frontend(model, config);
  GPG graph = frontend.buildInitialGPG(*module->getFunction("main"));
  auto compatible = [&](const llvm::Type *lhs, const llvm::Type *rhs) {
    return model.typesCompatible(lhs, rhs);
  };
  graph.strengthReduce(compatible, config.heap_indirection_limit);

  const LocationId function_location =
      model.functionLocation(module->getFunction("foo"));
  bool found_field_store = false;
  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    for (const GPU &update : block.gpus) {
      if (update.target.location != function_location)
        continue;
      const auto &path = update.source.indirections.elements();
      found_field_store |= path.size() == 2 &&
                           path[0] == Indirection::fieldAt(1) &&
                           path[1] == Indirection::dereference();
    }
  }
  EXPECT_TRUE(found_field_store);
}

TEST(GPGLLVMFrontend, ExtractsGlobalFunctionPointerInitializers) {
  const char *ir = R"(
    @callback = global i32 (i32)* @foo

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGLLVMFrontend");
  ASSERT_NE(module, nullptr);
  ProgramModel model(*module);
  GPGConfig config;
  LLVMFrontend frontend(model, config);

  ASSERT_EQ(frontend.globalInitializers().size(), 1u);
  const GPU &initializer = *frontend.globalInitializers().begin();
  EXPECT_EQ(initializer.source.location,
            model.objectLocation(module->getGlobalVariable("callback")));
  EXPECT_EQ(initializer.source.indirections, IndirectionList::dereferences(1));
  EXPECT_EQ(initializer.target.location,
            model.functionLocation(module->getFunction("foo")));
}

TEST(GPGAnalysis, ResolvesAFunctionPointerReturnedByDirectCall) {
  const char *ir = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 (i32)* @id(i32 (i32)* %function) {
    entry:
      ret i32 (i32)* %function
    }

    define i32 @main() {
    entry:
      %pointer = call i32 (i32)* @id(i32 (i32)* @foo)
      %result = call i32 %pointer(i32 7)
      ret i32 %result
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGAnalysis");
  ASSERT_NE(module, nullptr);
  GPGAnalysisEngine analysis(*module);
  analysis.run();

  auto calls = getIndirectCalls(*module->getFunction("main"));
  ASSERT_EQ(calls.size(), 1u);
  const auto *targets = analysis.result().callTargets(calls.front());
  ASSERT_NE(targets, nullptr);
  EXPECT_EQ(*targets,
            (std::set<const llvm::Function *>{module->getFunction("foo")}));
  llvm::Instruction *pointer =
      findInstructionByName(module->getFunction("main"), "pointer");
  ASSERT_NE(pointer, nullptr);
  EXPECT_EQ(analysis.result().pointees(calls.front(), pointer),
            (std::set<const llvm::Value *>{module->getFunction("foo")}));
}

TEST(GPGAnalysis, StrongUpdateRemovesOverwrittenFunctionTarget) {
  const char *ir = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %result = sub i32 %x, 1
      ret i32 %result
    }

    define i32 @main() {
    entry:
      %cell = alloca i32 (i32)*
      store i32 (i32)* @foo, i32 (i32)** %cell
      store i32 (i32)* @bar, i32 (i32)** %cell
      %pointer = load i32 (i32)*, i32 (i32)** %cell
      %result = call i32 %pointer(i32 7)
      ret i32 %result
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGAnalysis");
  ASSERT_NE(module, nullptr);
  GPGAnalysisEngine analysis(*module);
  analysis.run();

  auto calls = getIndirectCalls(*module->getFunction("main"));
  ASSERT_EQ(calls.size(), 1u);
  const auto *targets = analysis.result().callTargets(calls.front());
  ASSERT_NE(targets, nullptr);
  EXPECT_EQ(*targets,
            (std::set<const llvm::Function *>{module->getFunction("bar")}));

  const std::set<const llvm::Function *> flow_insensitive_targets = {
      module->getFunction("foo"), module->getFunction("bar")};
  for (AnalysisMode mode : {
           AnalysisMode::FlowInsensitiveContextSensitive,
           AnalysisMode::FlowAndContextInsensitive,
       }) {
    GPGConfig config;
    config.mode = mode;
    GPGAnalysisEngine flow_insensitive(*module, config);
    flow_insensitive.run();
    const auto *mode_targets =
        flow_insensitive.result().callTargets(calls.front());
    ASSERT_NE(mode_targets, nullptr);
    EXPECT_EQ(*mode_targets, flow_insensitive_targets);
  }
}

TEST(GPGAnalysis, DistinguishesTwoCallingContexts) {
  const char *ir = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %result = sub i32 %x, 1
      ret i32 %result
    }

    define i32 (i32)* @id(i32 (i32)* %function) {
    entry:
      ret i32 (i32)* %function
    }

    define i32 @main() {
    entry:
      %first = call i32 (i32)* @id(i32 (i32)* @foo)
      %second = call i32 (i32)* @id(i32 (i32)* @bar)
      %a = call i32 %first(i32 1)
      %b = call i32 %second(i32 2)
      %sum = add i32 %a, %b
      ret i32 %sum
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGAnalysis");
  ASSERT_NE(module, nullptr);
  GPGAnalysisEngine analysis(*module);
  analysis.run();

  auto calls = getIndirectCalls(*module->getFunction("main"));
  ASSERT_EQ(calls.size(), 2u);
  const auto *first = analysis.result().callTargets(calls[0]);
  const auto *second = analysis.result().callTargets(calls[1]);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(*first,
            (std::set<const llvm::Function *>{module->getFunction("foo")}));
  EXPECT_EQ(*second,
            (std::set<const llvm::Function *>{module->getFunction("bar")}));
}

TEST(GPGAnalysis, RefinesRecursiveSummaryToAFixedPoint) {
  const char *ir = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 (i32)* @choose(i1 %base_case) {
    entry:
      br i1 %base_case, label %base, label %recursive
    base:
      ret i32 (i32)* @foo
    recursive:
      %nested = call i32 (i32)* @choose(i1 true)
      ret i32 (i32)* %nested
    }

    define i32 @main() {
    entry:
      %pointer = call i32 (i32)* @choose(i1 false)
      %result = call i32 %pointer(i32 7)
      ret i32 %result
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGAnalysis");
  ASSERT_NE(module, nullptr);
  GPGAnalysisEngine analysis(*module);
  analysis.run();

  auto calls = getIndirectCalls(*module->getFunction("main"));
  ASSERT_EQ(calls.size(), 1u);
  const auto *targets = analysis.result().callTargets(calls.front());
  ASSERT_NE(targets, nullptr);
  EXPECT_EQ(*targets,
            (std::set<const llvm::Function *>{module->getFunction("foo")}));
}

TEST(GPGAnalysis, IntegratesWithAliasAnalysisWrapper) {
  const char *ir = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %result = call i32 @invoke()
      ret i32 %result
    }

    @callback = global i32 (i32)* @foo

    define i32 @invoke() {
    entry:
      %pointer = load i32 (i32)*, i32 (i32)** @callback
      %result = call i32 %pointer(i32 9)
      ret i32 %result
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGAnalysis");
  ASSERT_NE(module, nullptr);
  lotus::AliasAnalysisWrapper wrapper(*module, lotus::AAConfig::GPG());
  ASSERT_TRUE(wrapper.isInitialized());

  auto calls = getIndirectCalls(*module->getFunction("invoke"));
  ASSERT_EQ(calls.size(), 1u);
  std::vector<const llvm::Function *> targets;
  wrapper.getIndirectCallTargets(calls.front(), targets);
  EXPECT_EQ(targets,
            (std::vector<const llvm::Function *>{module->getFunction("foo")}));
}

TEST(GPGAnalysis, ModelsArraysIndexInsensitivelyByDefault) {
  const char *ir = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %array = alloca [2 x i32 (i32)*]
      %first = getelementptr [2 x i32 (i32)*],
                             [2 x i32 (i32)*]* %array, i64 0, i64 0
      %second = getelementptr [2 x i32 (i32)*],
                              [2 x i32 (i32)*]* %array, i64 0, i64 1
      store i32 (i32)* @foo, i32 (i32)** %first
      store i32 (i32)* @bar, i32 (i32)** %second
      %pointer = load i32 (i32)*, i32 (i32)** %first
      %result = call i32 %pointer(i32 1)
      ret i32 %result
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, ir, "GPGAnalysis");
  ASSERT_NE(module, nullptr);
  auto calls = getIndirectCalls(*module->getFunction("main"));
  ASSERT_EQ(calls.size(), 1u);

  {
    ProgramModel model(*module);
    GPGConfig config;
    LLVMFrontend frontend(model, config);
    GPG graph = frontend.buildInitialGPG(*module->getFunction("main"));
    auto compatible = [&](const llvm::Type *lhs, const llvm::Type *rhs) {
      return model.typesCompatible(lhs, rhs);
    };
    graph.strengthReduce(compatible, config.heap_indirection_limit);
    std::set<const llvm::Function *> frontend_targets;
    for (GPBId id : graph.callBlocks()) {
      for (const GPU &update : graph.getBlock(id)->gpus) {
        if (const llvm::Function *target =
                model.asFunction(update.target.location))
          frontend_targets.insert(target);
      }
    }
    EXPECT_EQ(frontend_targets,
              (std::set<const llvm::Function *>{module->getFunction("foo"),
                                                module->getFunction("bar")}));
  }

  GPGAnalysisEngine index_insensitive(*module);
  index_insensitive.run();
  const auto *insensitive_targets =
      index_insensitive.result().callTargets(calls.front());
  ASSERT_NE(insensitive_targets, nullptr);
  EXPECT_EQ(*insensitive_targets,
            (std::set<const llvm::Function *>{module->getFunction("foo"),
                                              module->getFunction("bar")}));

  GPGConfig config;
  config.array_index_sensitive = true;
  {
    ProgramModel model(*module);
    LLVMFrontend frontend(model, config);
    GPG graph = frontend.buildInitialGPG(*module->getFunction("main"));
    auto compatible = [&](const llvm::Type *lhs, const llvm::Type *rhs) {
      return model.typesCompatible(lhs, rhs);
    };
    graph.strengthReduce(compatible, config.heap_indirection_limit);
    std::set<const llvm::Function *> frontend_targets;
    for (GPBId id : graph.callBlocks()) {
      const GPB *block = graph.getBlock(id);
      for (const GPU &update : block->gpus) {
        if (const llvm::Function *target =
                model.asFunction(update.target.location))
          frontend_targets.insert(target);
      }
    }
    EXPECT_EQ(frontend_targets,
              (std::set<const llvm::Function *>{module->getFunction("foo")}));
  }
  GPGAnalysisEngine index_sensitive(*module, config);
  index_sensitive.run();
  const auto *sensitive_targets =
      index_sensitive.result().callTargets(calls.front());
  ASSERT_NE(sensitive_targets, nullptr);
  EXPECT_EQ(*sensitive_targets,
            (std::set<const llvm::Function *>{module->getFunction("foo")}));
}
