#include "Checker/Pulse/Topl/PulseTopl.h"

#include "PulseCheckerFixture.h"

using namespace pulse;

namespace {
const char *Protocol = R"(
property Protocol
  message "resource used after closing"
  start -> start: *
  start -> closed: close_resource(X, _) => x := X
  closed -> error: use_resource(X, _) when x == X
)";

class PulseToplTest : public PulseCheckerTest {
protected:
  size_t run(llvm::StringRef ir, llvm::StringRef property = Protocol) {
    auto module = lotus::unittest::parseModule(context, ir.str());
    EXPECT_NE(module, nullptr);
    if (!module)
      return 0;
    auto program = ToplProgram::parse(property);
    if (!program) {
      ADD_FAILURE() << llvm::toString(program.takeError());
      return 0;
    }
    PulseChecker checker(module.get());
    checker.setToplProgram(std::move(*program));
    checker.analyze();
    return getReportCountForType(BugReportMgr::get_instance(), "TOPL Error");
  }
};
} // namespace

TEST_F(PulseToplTest, ReportsDirectProtocolViolation) {
  EXPECT_EQ(run(R"(
    declare void @close_resource(i8*)
    declare void @use_resource(i8*)
    define void @bad(i8* %p) {
      call void @close_resource(i8* %p)
      call void @use_resource(i8* %p)
      ret void
    }
  )"),
            1u);
}

TEST_F(PulseToplTest, PatternCanOmitArgumentBindings) {
  EXPECT_EQ(run(R"(
    declare void @forbidden(i32)
    define void @bad() {
      call void @forbidden(i32 1)
      ret void
    }
  )",
                "property Forbidden\nstart -> error: forbidden"),
            1u);
}

TEST_F(PulseToplTest, WildcardStateChangeHasReportableTrace) {
  EXPECT_EQ(run(R"(
    declare void @event()
    define void @bad() {
      call void @event()
      ret void
    }
  )",
                "property EveryCall\nstart -> error: *"),
            1u);
}

TEST_F(PulseToplTest, DoesNotCombineMutuallyExclusiveBranches) {
  EXPECT_EQ(run(R"(
    declare void @close_resource(i8*)
    declare void @use_resource(i8*)
    define void @ok(i8* %p, i1 %flag) {
      br i1 %flag, label %a, label %b
    a:
      call void @close_resource(i8* %p)
      br label %exit
    b:
      call void @use_resource(i8* %p)
      br label %exit
    exit:
      ret void
    }
  )"),
            0u);
}

TEST_F(PulseToplTest, IgnoresUnreachableEvents) {
  EXPECT_EQ(run(R"(
    declare void @close_resource(i8*)
    declare void @use_resource(i8*)
    define void @ok(i8* %p) {
      call void @close_resource(i8* %p)
      br i1 false, label %dead, label %exit
    dead:
      call void @use_resource(i8* %p)
      ret void
    exit:
      ret void
    }
  )"),
            0u);
}

TEST_F(PulseToplTest, ComposesEventsAcrossCalleeSummaries) {
  EXPECT_EQ(run(R"(
    declare void @close_resource(i8*)
    declare void @use_resource(i8*)
    define void @helper(i8* %p) {
      call void @use_resource(i8* %p)
      ret void
    }
    define void @caller(i8* %p) {
      call void @close_resource(i8* %p)
      call void @helper(i8* %p)
      ret void
    }
  )"),
            1u);
}

TEST_F(PulseToplTest, KeepsDifferentActualResourcesSeparate) {
  EXPECT_EQ(run(R"(
    declare void @close_resource(i8*)
    declare void @use_resource(i8*)
    define void @helper(i8* %p) {
      call void @use_resource(i8* %p)
      ret void
    }
    define void @caller(i8* %p, i8* %q) {
      %distinct = icmp ne i8* %p, %q
      br i1 %distinct, label %body, label %exit
    body:
      call void @close_resource(i8* %p)
      call void @helper(i8* %q)
      br label %exit
    exit:
      ret void
    }
  )"),
            0u);
}

TEST_F(PulseToplTest, TracksReturnedObjectThroughTwoSummaries) {
  EXPECT_EQ(run(R"(
    declare i8* @source()
    declare void @sink(i8*)
    define i8* @get() {
      %p = call i8* @source()
      ret i8* %p
    }
    define void @send(i8* %p) {
      call void @sink(i8* %p)
      ret void
    }
    define void @caller() {
      %p = call i8* @get()
      call void @send(i8* %p)
      ret void
    }
  )",
                R"(
property Flow
  start -> start: *
  start -> tracking: source(Ret) => x := Ret
  tracking -> error: sink(Arg, _) when x == Arg
)"),
            1u);
}

TEST_F(PulseToplTest, SupportsPrefixesAndConstantGuards) {
  EXPECT_EQ(run(R"(
    declare void @api_step(i32)
    define void @bad() {
      call void @api_step(i32 7)
      ret void
    }
  )",
                R"(
property Guard
  prefix "api_"
  start -> error: step(N, _) when N >= 5 && N < 10
)"),
            1u);
}

TEST_F(PulseToplTest, CleanupCanLeaveErrorState) {
  EXPECT_EQ(run(R"(
    declare void @lock_resource(i8*)
    declare void @unlock_resource(i8*)
    define void @ok(i8* %p) {
      call void @lock_resource(i8* %p)
      call void @unlock_resource(i8* %p)
      ret void
    }
  )",
                R"(
property LockUnlock
  start -> start: *
  start -> error: lock_resource(X, _) => x := X
  error -> start: unlock_resource(X, _) when x == X
)"),
            0u);
}

TEST_F(PulseToplTest, CallerCanRepairCalleeErrorState) {
  EXPECT_EQ(run(R"(
    declare void @lock_resource(i8*)
    declare void @unlock_resource(i8*)
    define void @helper(i8* %p) {
      call void @lock_resource(i8* %p)
      ret void
    }
    define void @ok(i8* %p) {
      call void @helper(i8* %p)
      call void @unlock_resource(i8* %p)
      ret void
    }
  )",
                R"(
property LockUnlock
  start -> start: *
  start -> error: lock_resource(X, _) => x := X
  error -> start: unlock_resource(X, _) when x == X
)"),
            0u);
}

TEST_F(PulseToplTest, UnknownCleanupGuardCannotProveUnrepairedError) {
  EXPECT_EQ(run(R"(
    declare void @lock_resource(i8*)
    declare void @unlock_resource(i8*)
    define void @unknown(i8* %p, i8* %q) {
      call void @lock_resource(i8* %p)
      call void @unlock_resource(i8* %q)
      ret void
    }
  )",
                R"(
property LockUnlock
  start -> start: *
  start -> error: lock_resource(X, _) => x := X
  error -> start: unlock_resource(X, _) when x == X
)"),
            0u);
}

TEST_F(PulseToplTest, UnrepairedCalleeErrorIsReportedAtCallerBoundary) {
  EXPECT_EQ(run(R"(
    declare void @lock_resource(i8*)
    define void @helper(i8* %p) {
      call void @lock_resource(i8* %p)
      ret void
    }
    define void @bad(i8* %p) {
      call void @helper(i8* %p)
      ret void
    }
  )",
                R"(
property LockUnlock
  start -> error: lock_resource(X, _) => x := X
)"),
            1u);
}

TEST_F(PulseToplTest, DoesNotAssumeUnknownGuard) {
  EXPECT_EQ(run(R"(
    declare void @step(i32)
    define void @unknown(i32 %n) {
      call void @step(i32 %n)
      ret void
    }
  )",
                R"(
property Guard
  start -> error: step(N, _) when N == 7
)"),
            0u);
}

TEST_F(PulseToplTest, ResolvesCalleeNumericGuardUsingCallerFacts) {
  EXPECT_EQ(run(R"(
    declare void @step(i32)
    define void @helper(i32 %n) {
      call void @step(i32 %n)
      ret void
    }
    define void @bad() {
      call void @helper(i32 7)
      ret void
    }
  )",
                R"(
property Guard
  start -> error: step(N, _) when N >= 5
)"),
            1u);
}

TEST_F(PulseToplTest, SupportsArrayWriteEvent) {
  EXPECT_EQ(run(R"(
    define void @bad() {
      %a = alloca [8 x i8]
      %p = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i64 3
      store i8 1, i8* %p
      ret void
    }
  )",
                R"(
property Array
  start -> error: #ArrayWrite(A, I) when I == 3
)"),
            1u);
}

TEST(PulseToplParserTest, RejectsMalformedAndUnsupportedProperties) {
  for (const char *text :
       {"property Bad\nstart -> error: f(X, _) when X ~~> Y",
        "property Bad\nstart -> error: f(X, _) => x := Unbound",
        "property Bad\nstart -> error: \"[\"(X)",
        "property Bad\nstart -> done: f(X)",
        "property Bad\nstart -> error: f(X)\nproperty Bad"}) {
    auto program = ToplProgram::parse(text, "test.topl");
    EXPECT_FALSE(static_cast<bool>(program));
    if (!program)
      EXPECT_NE(llvm::toString(program.takeError()).find("test.topl:"),
                std::string::npos);
  }
}

TEST(PulseToplParserTest, EventBudgetKeepsPrefixAndStopsRecording) {
  auto program = ToplProgram::parse("property Limit\nstart -> error: bad(_)");
  ASSERT_TRUE(static_cast<bool>(program));
  ToplHistory history;
  for (size_t i = 0; i < ToplHistory::MaxEvents; ++i)
    history.append({ToplEvent::Kind::Call, "ok", {ToplValue{}}, nullptr, {}});
  history.append({ToplEvent::Kind::Call, "bad", {ToplValue{}}, nullptr, {}});
  EXPECT_TRUE(history.truncated);
  EXPECT_TRUE(program->evaluate(history, PulseFormula{}).empty());
}

TEST(PulseToplParserTest, TruncatedHistoryCannotProveUnrepairedError) {
  auto program = ToplProgram::parse(
      "property Limit\nstart -> error: bad(_)\nerror -> start: cleanup(_)");
  ASSERT_TRUE(static_cast<bool>(program));
  ToplHistory history;
  history.append({ToplEvent::Kind::Call, "bad", {ToplValue{}}, nullptr, {}});
  for (size_t i = 1; i < ToplHistory::MaxEvents; ++i)
    history.append({ToplEvent::Kind::Call, "ok", {ToplValue{}}, nullptr, {}});
  history.append(
      {ToplEvent::Kind::Call, "cleanup", {ToplValue{}}, nullptr, {}});
  EXPECT_TRUE(program->evaluate(history, PulseFormula{}).empty());
}
