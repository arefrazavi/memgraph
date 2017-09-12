#include <gtest/gtest.h>
#include <memory>

#include "database/dbms.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/plan/cost_estimator.hpp"
#include "query/plan/operator.hpp"
#include "storage/vertex_accessor.hpp"

using namespace query;
using namespace query::plan;

using CardParam = CostEstimator<GraphDbAccessor>::CardParam;
using CostParam = CostEstimator<GraphDbAccessor>::CostParam;
using MiscParam = CostEstimator<GraphDbAccessor>::MiscParam;

/** A fixture for cost estimation. Sets up the database
 * and accessor (adds some vertices). Provides convenience
 * functions for creating the logical plan. Note that the
 * resulting plan is NOT fit for execution, only for cost
 * estimation testing. */
class QueryCostEstimator : public ::testing::Test {
 protected:
  Dbms dbms;
  std::unique_ptr<GraphDbAccessor> dba = dbms.active();
  GraphDbTypes::Label label = dba->Label("label");
  GraphDbTypes::Property property = dba->Property("property");

  // we incrementally build the logical operator plan
  // start it off with Once
  std::shared_ptr<LogicalOperator> last_op_ = std::make_shared<Once>();

  AstTreeStorage storage_;
  SymbolTable symbol_table_;
  int symbol_count = 0;

  void SetUp() {
    // create the index in the current db accessor and then swap it to a new one
    dba->BuildIndex(label, property);
    auto new_dba = dbms.active();
    dba.swap(new_dba);
  }

  Symbol NextSymbol() {
    return symbol_table_.CreateSymbol("Symbol" + std::to_string(symbol_count++),
                                      true);
  }

  /** Adds the given number of vertices to the DB, of which
   * the given numbers are labeled and have a property set. */
  void AddVertices(int vertex_count, int labeled_count,
                   int property_count = 0) {
    for (int i = 0; i < vertex_count; i++) {
      auto vertex = dba->InsertVertex();
      if (i < labeled_count) vertex.add_label(label);
      if (i < property_count) vertex.PropsSet(property, i);
    }

    dba->AdvanceCommand();
  }

  auto Cost() {
    CostEstimator<GraphDbAccessor> cost_estimator(*dba);
    last_op_->Accept(cost_estimator);
    return cost_estimator.cost();
  }

  template <typename TLogicalOperator, typename... TArgs>
  void MakeOp(TArgs... args) {
    last_op_ = std::make_shared<TLogicalOperator>(args...);
  }

  template <typename TValue>
  Expression *Literal(TValue value) {
    return storage_.Create<PrimitiveLiteral>(value);
  }

  auto InclusiveBound(int bound) {
    return std::experimental::make_optional(
        utils::MakeBoundInclusive(Literal(bound)));
  };

  const std::experimental::nullopt_t nullopt = std::experimental::nullopt;
};

// multiply with 1 to avoid linker error (possibly fixed in CLang >= 3.81)
#define EXPECT_COST(COST) EXPECT_FLOAT_EQ(Cost(), 1 * COST)

TEST_F(QueryCostEstimator, Once) { EXPECT_COST(0); }

TEST_F(QueryCostEstimator, ScanAll) {
  AddVertices(100, 30, 20);
  MakeOp<ScanAll>(last_op_, NextSymbol());
  EXPECT_COST(100 * CostParam::kScanAll);
}

TEST_F(QueryCostEstimator, ScanAllByLabelCardinality) {
  AddVertices(100, 30, 20);
  MakeOp<ScanAllByLabel>(last_op_, NextSymbol(), label);
  EXPECT_COST(30 * CostParam::kScanAllByLabel);
}

TEST_F(QueryCostEstimator, ScanAllByLabelPropertyValueLiteral) {
  AddVertices(100, 30, 20);
  MakeOp<ScanAllByLabelPropertyValue>(last_op_, NextSymbol(), label, property,
                                      Literal(12));
  EXPECT_COST(1 * CostParam::MakeScanAllByLabelPropertyValue);
}

TEST_F(QueryCostEstimator, ScanAllByLabelPropertyValueExpr) {
  AddVertices(100, 30, 20);
  MakeOp<ScanAllByLabelPropertyValue>(
      last_op_, NextSymbol(), label, property,
      // once we make expression const-folding this test case will fail
      storage_.Create<UnaryPlusOperator>(Literal(12)));
  EXPECT_COST(20 * CardParam::kFilter *
              CostParam::MakeScanAllByLabelPropertyValue);
}

TEST_F(QueryCostEstimator, ScanAllByLabelPropertyRangeUpper) {
  AddVertices(100, 30, 20);
  MakeOp<ScanAllByLabelPropertyRange>(last_op_, NextSymbol(), label, property,
                                      nullopt, InclusiveBound(12));
  // cardinality estimation is exact for very small indexes
  EXPECT_COST(13 * CostParam::MakeScanAllByLabelPropertyRange);
}

TEST_F(QueryCostEstimator, ScanAllByLabelPropertyRangeLower) {
  AddVertices(100, 30, 20);
  MakeOp<ScanAllByLabelPropertyRange>(last_op_, NextSymbol(), label, property,
                                      InclusiveBound(17), nullopt);
  // cardinality estimation is exact for very small indexes
  EXPECT_COST(3 * CostParam::MakeScanAllByLabelPropertyRange);
}

TEST_F(QueryCostEstimator, ScanAllByLabelPropertyRangeNonLiteral) {
  AddVertices(100, 30, 20);
  auto bound = std::experimental::make_optional(
      utils::MakeBoundInclusive(static_cast<Expression *>(
          storage_.Create<UnaryPlusOperator>(Literal(12)))));
  MakeOp<ScanAllByLabelPropertyRange>(last_op_, NextSymbol(), label, property,
                                      bound, nullopt);
  EXPECT_COST(20 * CardParam::kFilter *
              CostParam::MakeScanAllByLabelPropertyRange);
}

TEST_F(QueryCostEstimator, Expand) {
  MakeOp<Expand>(NextSymbol(), NextSymbol(), EdgeAtom::Direction::IN, nullptr,
                 last_op_, NextSymbol(), false, false);
  EXPECT_COST(CardParam::kExpand * CostParam::kExpand);
}

TEST_F(QueryCostEstimator, ExpandVariable) {
  MakeOp<ExpandVariable>(NextSymbol(), NextSymbol(), EdgeAtom::Direction::IN,
                         nullptr, false, nullptr, nullptr, last_op_,
                         NextSymbol(), false, false);
  EXPECT_COST(CardParam::kExpandVariable * CostParam::kExpandVariable);
}

TEST_F(QueryCostEstimator, ExpandBreadthFirst) {
  MakeOp<ExpandBreadthFirst>(
      NextSymbol(), NextSymbol(), EdgeAtom::Direction::IN, nullptr, Literal(3),
      NextSymbol(), NextSymbol(), Literal(true), last_op_, NextSymbol(), false);
  EXPECT_COST(CardParam::kExpandBreadthFirst * CostParam::kExpandBreadthFirst);
}

// Helper for testing an operations cost and cardinality.
// Only for operations that first increment cost, then modify cardinality.
// Intentially a macro (instead of function) for better test feedback.
#define TEST_OP(OP, OP_COST_PARAM, OP_CARD_PARAM) \
  OP;                                             \
  EXPECT_COST(OP_COST_PARAM);                     \
  OP;                                             \
  EXPECT_COST(OP_COST_PARAM + OP_CARD_PARAM * OP_COST_PARAM);

TEST_F(QueryCostEstimator, Filter) {
  TEST_OP(MakeOp<Filter>(last_op_, Literal(true)), CostParam::kFilter,
          CardParam::kFilter);
}

TEST_F(QueryCostEstimator, ExpandUniquenessFilter) {
  TEST_OP(MakeOp<ExpandUniquenessFilter<VertexAccessor>>(last_op_, NextSymbol(),
                                                         std::vector<Symbol>()),
          CostParam::kExpandUniquenessFilter,
          CardParam::kExpandUniquenessFilter);
}

TEST_F(QueryCostEstimator, UnwindLiteral) {
  TEST_OP(
      MakeOp<query::plan::Unwind>(
          last_op_,
          storage_.Create<ListLiteral>(std::vector<Expression *>(7, nullptr)),
          NextSymbol()),
      CostParam::kUnwind, 7);
}

TEST_F(QueryCostEstimator, UnwindNoLiteral) {
  TEST_OP(MakeOp<query::plan::Unwind>(last_op_, nullptr, NextSymbol()),
          CostParam::kUnwind, MiscParam::kUnwindNoLiteral);
}

#undef TEST_OP
#undef EXPECT_COST
//
// TODO test cost when ScanAll, Expand, Accumulate, Limit
// vs cost for SA, Expand, Limit
