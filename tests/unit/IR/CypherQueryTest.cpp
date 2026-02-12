#include "IR/PDG/Analysis/CypherQuery.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"

#include "IR/PDG/Core/ProgramDependencyGraph.h"

#include <iostream>
#include <sstream>

#include <gtest/gtest.h>

using namespace llvm;
using namespace pdg;

class CypherQueryTest : public ::testing::Test {
protected:
  void SetUp() override {
    LLVMContext Context;
    SMDiagnostic Err;

    std::string testIR = R"(
      define i32 @test_func(i32 %x) {
      entry:
        %cmp = icmp sgt i32 %x, 0
        br i1 %cmp, label %then, label %else
      then:
        %add = add i32 %x, 1
        ret i32 %add
      else:
        ret i32 0
      }
    )";

    auto M = parseIR(MemoryBuffer::getMemBuffer(testIR)->getMemBufferRef(), Err,
                     Context);
    if (M) {
      pdg_ = &ProgramGraph::getInstance();
      pdg_->build(*M);
    }
  }

  ProgramGraph *pdg_ = nullptr;
};

TEST_F(CypherQueryTest, ParseSimple) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("MATCH (n) RETURN n");

  ASSERT_NE(query, nullptr);
  EXPECT_FALSE(query->getPatterns().empty());
  EXPECT_FALSE(query->getReturnItems().empty());
}

TEST_F(CypherQueryTest, ParseMatchWithLabel) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n:INST_FUNCALL) RETURN n");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getStartNode(), nullptr);
  EXPECT_EQ(pattern->getStartNode()->getLabel(), "INST_FUNCALL");
}

TEST_F(CypherQueryTest, ParseMatchWithRelationship) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[r:EDGE_TYPE]->(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getRelationship(), nullptr);
  EXPECT_EQ(pattern->getRelationship()->getType(), "EDGE_TYPE");
}

TEST_F(CypherQueryTest, ParseMatchWithWhere) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) WHERE n.type = 'INST_FUNCALL' RETURN n");

  ASSERT_NE(query, nullptr);
  EXPECT_NE(query->getWhereClause(), nullptr);
}

TEST_F(CypherQueryTest, ParseMatchWithOrderBy) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) RETURN n ORDER BY n.id DESC");

  ASSERT_NE(query, nullptr);
  ASSERT_NE(query->getOrderBy(), nullptr);
  EXPECT_EQ(query->getOrderBy()->getDirection(),
            CypherOrderBy::Direction::DESC);
}

TEST_F(CypherQueryTest, ParseMatchWithLimit) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) RETURN n LIMIT 10");

  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->getLimit(), 10);
}

TEST_F(CypherQueryTest, ParseMatchWithVariableLength) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[*]->(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getRelationship(), nullptr);
  EXPECT_EQ(pattern->getRelationship()->getMinHops(), 1);
  EXPECT_EQ(pattern->getRelationship()->getMaxHops(), -1);
}

TEST_F(CypherQueryTest, ParseErrorHandling) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("INVALID QUERY SYNTAX");

  EXPECT_TRUE(parser.hasError());
}

TEST_F(CypherQueryTest, ExecuteSimpleMatch) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);

  std::unique_ptr<CypherQuery> query = parser.parse("MATCH (n) RETURN n");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(result->isEmpty());
}

TEST_F(CypherQueryTest, ExecuteMatchWithLabel) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);

  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n:FUNC_ENTRY) RETURN n");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(result->isEmpty());
}

TEST_F(CypherQueryTest, QueryResultOperations) {
  CypherResult result(CypherResult::ResultType::NODES);

  EXPECT_TRUE(result.isEmpty());
  result.setScalarValue("test");
  EXPECT_FALSE(result.isEmpty());
  EXPECT_EQ(result.getScalarValue(), "test");
}

TEST_F(CypherQueryTest, CypherResultToString) {
  CypherResult nodesResult(CypherResult::ResultType::NODES);
  std::string nodesStr = nodesResult.toString();
  EXPECT_TRUE(nodesStr.find("nodes") != std::string::npos);

  CypherResult scalarResult(CypherResult::ResultType::SCALAR);
  scalarResult.setScalarValue("hello");
  EXPECT_EQ(scalarResult.toString(), "hello");
}

TEST_F(CypherQueryTest, EmptyQuery) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("");

  EXPECT_EQ(query, nullptr);
  EXPECT_TRUE(parser.hasError());
}

TEST_F(CypherQueryTest, UnclosedParenthesis) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query = parser.parse("MATCH (n RETURN n");

  EXPECT_EQ(query, nullptr);
  EXPECT_TRUE(parser.hasError());
}

TEST_F(CypherQueryTest, CaseInsensitiveKeywords) {
  CypherParser parser;

  std::unique_ptr<CypherQuery> queryLower = parser.parse("match (n) return n");
  EXPECT_NE(queryLower, nullptr);

  std::unique_ptr<CypherQuery> queryUpper = parser.parse("MATCH (n) RETURN n");
  EXPECT_NE(queryUpper, nullptr);
}

TEST_F(CypherQueryTest, BidirectionalRelationship) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)<-[r]-(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());

  const auto *pattern = query->getPatterns()[0].get();
  ASSERT_NE(pattern->getRelationship(), nullptr);
  EXPECT_EQ(pattern->getRelationship()->getDirection(),
            CypherRelationshipPattern::Direction::IN);
  EXPECT_FALSE(pattern->getRelationship()->isBidirectional());
}

TEST_F(CypherQueryTest, MultipleReturnItems) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a), (b) RETURN a, b, a.id, b.name");

  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->getReturnItems().size(), 4);
}

TEST_F(CypherQueryTest, ParseComparisonOperators) {
  CypherParser parser;

  auto q1 = parser.parse("MATCH (n) WHERE n.type <= 10 RETURN n");
  ASSERT_NE(q1, nullptr);

  auto q2 = parser.parse("MATCH (n) WHERE n.type >= 10 RETURN n");
  ASSERT_NE(q2, nullptr);

  auto q3 = parser.parse("MATCH (n) WHERE n.type != 10 RETURN n");
  ASSERT_NE(q3, nullptr);
}

TEST_F(CypherQueryTest, ParseChainedPattern) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[r1]->(b)-[r2]->(c) RETURN a, b, c");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());
  const auto *root = query->getPatterns()[0].get();
  ASSERT_NE(root, nullptr);
  ASSERT_NE(root->getRelationship(), nullptr);
  ASSERT_FALSE(root->getNextElements().empty());
  ASSERT_NE(root->getNextElements()[0]->getRelationship(), nullptr);
}

TEST_F(CypherQueryTest, ParsePathBindingPattern) {
  CypherParser parser;
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH p = (a)-[*]->(b) RETURN a, b");

  ASSERT_NE(query, nullptr);
  ASSERT_FALSE(query->getPatterns().empty());
}

TEST_F(CypherQueryTest, ReturnRelationshipProjection) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (a)-[r]->(b) RETURN r LIMIT 5");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::RELATIONSHIPS);
}

TEST_F(CypherQueryTest, ReturnPropertyProjection) {
  if (!pdg_) {
    GTEST_SKIP() << "PDG not available";
  }

  CypherParser parser;
  CypherQueryExecutor executor(*pdg_);
  std::unique_ptr<CypherQuery> query =
      parser.parse("MATCH (n) RETURN n.label LIMIT 3");
  ASSERT_NE(query, nullptr);

  std::unique_ptr<CypherResult> result = executor.execute(*query);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->getType(), CypherResult::ResultType::SCALAR);
  EXPECT_FALSE(result->getScalarValue().empty());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
