#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "query/frontend/semantic/symbol_generator.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/plan/operator.hpp"
#include "query/plan/planner.hpp"
#include "query/plan/preprocess.hpp"

namespace query::plan {

class BaseOpChecker {
 public:
  virtual ~BaseOpChecker() {}

  virtual void CheckOp(LogicalOperator &, const SymbolTable &) = 0;
};

class PlanChecker : public virtual HierarchicalLogicalOperatorVisitor {
 public:
  using HierarchicalLogicalOperatorVisitor::PostVisit;
  using HierarchicalLogicalOperatorVisitor::PreVisit;
  using HierarchicalLogicalOperatorVisitor::Visit;

  PlanChecker(const std::list<std::unique_ptr<BaseOpChecker>> &checkers,
              const SymbolTable &symbol_table)
      : symbol_table_(symbol_table) {
    for (const auto &checker : checkers) checkers_.emplace_back(checker.get());
  }

  PlanChecker(const std::list<BaseOpChecker *> &checkers,
              const SymbolTable &symbol_table)
      : checkers_(checkers), symbol_table_(symbol_table) {}

#define PRE_VISIT(TOp)              \
  bool PreVisit(TOp &op) override { \
    CheckOp(op);                    \
    return true;                    \
  }

#define VISIT(TOp)               \
  bool Visit(TOp &op) override { \
    CheckOp(op);                 \
    return true;                 \
  }

  PRE_VISIT(CreateNode);
  PRE_VISIT(CreateExpand);
  PRE_VISIT(Delete);
  PRE_VISIT(ScanAll);
  PRE_VISIT(ScanAllByLabel);
  PRE_VISIT(ScanAllByLabelPropertyValue);
  PRE_VISIT(ScanAllByLabelPropertyRange);
  PRE_VISIT(Expand);
  PRE_VISIT(ExpandVariable);
  PRE_VISIT(Filter);
  PRE_VISIT(ConstructNamedPath);
  PRE_VISIT(Produce);
  PRE_VISIT(SetProperty);
  PRE_VISIT(SetProperties);
  PRE_VISIT(SetLabels);
  PRE_VISIT(RemoveProperty);
  PRE_VISIT(RemoveLabels);
  PRE_VISIT(EdgeUniquenessFilter);
  PRE_VISIT(Accumulate);
  PRE_VISIT(Aggregate);
  PRE_VISIT(Skip);
  PRE_VISIT(Limit);
  PRE_VISIT(OrderBy);
  bool PreVisit(Merge &op) override {
    CheckOp(op);
    op.input()->Accept(*this);
    return false;
  }
  bool PreVisit(Optional &op) override {
    CheckOp(op);
    op.input()->Accept(*this);
    return false;
  }
  PRE_VISIT(Unwind);
  PRE_VISIT(Distinct);

  bool Visit(Once &) override {
    // Ignore checking Once, it is implicitly at the end.
    return true;
  }

  bool PreVisit(Cartesian &op) override {
    CheckOp(op);
    return false;
  }

#undef PRE_VISIT
#undef VISIT

  void CheckOp(LogicalOperator &op) {
    ASSERT_FALSE(checkers_.empty());
    checkers_.back()->CheckOp(op, symbol_table_);
    checkers_.pop_back();
  }

  std::list<BaseOpChecker *> checkers_;
  const SymbolTable &symbol_table_;
};

template <class TOp>
class OpChecker : public BaseOpChecker {
 public:
  void CheckOp(LogicalOperator &op, const SymbolTable &symbol_table) override {
    auto *expected_op = dynamic_cast<TOp *>(&op);
    ASSERT_TRUE(expected_op);
    ExpectOp(*expected_op, symbol_table);
  }

  virtual void ExpectOp(TOp &, const SymbolTable &) {}
};

using ExpectCreateNode = OpChecker<CreateNode>;
using ExpectCreateExpand = OpChecker<CreateExpand>;
using ExpectDelete = OpChecker<Delete>;
using ExpectScanAll = OpChecker<ScanAll>;
using ExpectScanAllByLabel = OpChecker<ScanAllByLabel>;
using ExpectExpand = OpChecker<Expand>;
using ExpectFilter = OpChecker<Filter>;
using ExpectConstructNamedPath = OpChecker<ConstructNamedPath>;
using ExpectProduce = OpChecker<Produce>;
using ExpectSetProperty = OpChecker<SetProperty>;
using ExpectSetProperties = OpChecker<SetProperties>;
using ExpectSetLabels = OpChecker<SetLabels>;
using ExpectRemoveProperty = OpChecker<RemoveProperty>;
using ExpectRemoveLabels = OpChecker<RemoveLabels>;
using ExpectEdgeUniquenessFilter = OpChecker<EdgeUniquenessFilter>;
using ExpectSkip = OpChecker<Skip>;
using ExpectLimit = OpChecker<Limit>;
using ExpectOrderBy = OpChecker<OrderBy>;
using ExpectUnwind = OpChecker<Unwind>;
using ExpectDistinct = OpChecker<Distinct>;

class ExpectExpandVariable : public OpChecker<ExpandVariable> {
 public:
  void ExpectOp(ExpandVariable &op, const SymbolTable &) override {
    EXPECT_EQ(op.type_, query::EdgeAtom::Type::DEPTH_FIRST);
  }
};

class ExpectExpandBfs : public OpChecker<ExpandVariable> {
 public:
  void ExpectOp(ExpandVariable &op, const SymbolTable &) override {
    EXPECT_EQ(op.type_, query::EdgeAtom::Type::BREADTH_FIRST);
  }
};

class ExpectAccumulate : public OpChecker<Accumulate> {
 public:
  explicit ExpectAccumulate(const std::unordered_set<Symbol> &symbols)
      : symbols_(symbols) {}

  void ExpectOp(Accumulate &op, const SymbolTable &) override {
    std::unordered_set<Symbol> got_symbols(op.symbols_.begin(),
                                           op.symbols_.end());
    EXPECT_EQ(symbols_, got_symbols);
  }

 private:
  const std::unordered_set<Symbol> symbols_;
};

class ExpectAggregate : public OpChecker<Aggregate> {
 public:
  ExpectAggregate(const std::vector<query::Aggregation *> &aggregations,
                  const std::unordered_set<query::Expression *> &group_by)
      : aggregations_(aggregations), group_by_(group_by) {}

  void ExpectOp(Aggregate &op, const SymbolTable &symbol_table) override {
    auto aggr_it = aggregations_.begin();
    for (const auto &aggr_elem : op.aggregations_) {
      ASSERT_NE(aggr_it, aggregations_.end());
      auto aggr = *aggr_it++;
      // TODO: Proper expression equality
      EXPECT_EQ(typeid(aggr_elem.value).hash_code(),
                typeid(aggr->expression1_).hash_code());
      EXPECT_EQ(typeid(aggr_elem.key).hash_code(),
                typeid(aggr->expression2_).hash_code());
      EXPECT_EQ(aggr_elem.op, aggr->op_);
      EXPECT_EQ(aggr_elem.output_sym, symbol_table.at(*aggr));
    }
    EXPECT_EQ(aggr_it, aggregations_.end());
    // TODO: Proper group by expression equality
    std::unordered_set<size_t> got_group_by;
    for (auto *expr : op.group_by_)
      got_group_by.insert(typeid(*expr).hash_code());
    std::unordered_set<size_t> expected_group_by;
    for (auto *expr : group_by_)
      expected_group_by.insert(typeid(*expr).hash_code());
    EXPECT_EQ(got_group_by, expected_group_by);
  }

 private:
  std::vector<query::Aggregation *> aggregations_;
  std::unordered_set<query::Expression *> group_by_;
};

class ExpectMerge : public OpChecker<Merge> {
 public:
  ExpectMerge(const std::list<BaseOpChecker *> &on_match,
              const std::list<BaseOpChecker *> &on_create)
      : on_match_(on_match), on_create_(on_create) {}

  void ExpectOp(Merge &merge, const SymbolTable &symbol_table) override {
    PlanChecker check_match(on_match_, symbol_table);
    merge.merge_match_->Accept(check_match);
    PlanChecker check_create(on_create_, symbol_table);
    merge.merge_create_->Accept(check_create);
  }

 private:
  const std::list<BaseOpChecker *> &on_match_;
  const std::list<BaseOpChecker *> &on_create_;
};

class ExpectOptional : public OpChecker<Optional> {
 public:
  explicit ExpectOptional(const std::list<BaseOpChecker *> &optional)
      : optional_(optional) {}

  ExpectOptional(const std::vector<Symbol> &optional_symbols,
                 const std::list<BaseOpChecker *> &optional)
      : optional_symbols_(optional_symbols), optional_(optional) {}

  void ExpectOp(Optional &optional, const SymbolTable &symbol_table) override {
    if (!optional_symbols_.empty()) {
      EXPECT_THAT(optional.optional_symbols_,
                  testing::UnorderedElementsAreArray(optional_symbols_));
    }
    PlanChecker check_optional(optional_, symbol_table);
    optional.optional_->Accept(check_optional);
  }

 private:
  std::vector<Symbol> optional_symbols_;
  const std::list<BaseOpChecker *> &optional_;
};

class ExpectScanAllByLabelPropertyValue
    : public OpChecker<ScanAllByLabelPropertyValue> {
 public:
  ExpectScanAllByLabelPropertyValue(
      storage::Label label,
      const std::pair<std::string, storage::Property> &prop_pair,
      query::Expression *expression)
      : label_(label), property_(prop_pair.second), expression_(expression) {}

  void ExpectOp(ScanAllByLabelPropertyValue &scan_all,
                const SymbolTable &) override {
    EXPECT_EQ(scan_all.label_, label_);
    EXPECT_EQ(scan_all.property_, property_);
    // TODO: Proper expression equality
    EXPECT_EQ(typeid(scan_all.expression_).hash_code(),
              typeid(expression_).hash_code());
  }

 private:
  storage::Label label_;
  storage::Property property_;
  query::Expression *expression_;
};

class ExpectScanAllByLabelPropertyRange
    : public OpChecker<ScanAllByLabelPropertyRange> {
 public:
  ExpectScanAllByLabelPropertyRange(
      storage::Label label, storage::Property property,
      std::experimental::optional<ScanAllByLabelPropertyRange::Bound>
          lower_bound,
      std::experimental::optional<ScanAllByLabelPropertyRange::Bound>
          upper_bound)
      : label_(label),
        property_(property),
        lower_bound_(lower_bound),
        upper_bound_(upper_bound) {}

  void ExpectOp(ScanAllByLabelPropertyRange &scan_all,
                const SymbolTable &) override {
    EXPECT_EQ(scan_all.label_, label_);
    EXPECT_EQ(scan_all.property_, property_);
    if (lower_bound_) {
      ASSERT_TRUE(scan_all.lower_bound_);
      // TODO: Proper expression equality
      EXPECT_EQ(typeid(scan_all.lower_bound_->value()).hash_code(),
                typeid(lower_bound_->value()).hash_code());
      EXPECT_EQ(scan_all.lower_bound_->type(), lower_bound_->type());
    }
    if (upper_bound_) {
      ASSERT_TRUE(scan_all.upper_bound_);
      // TODO: Proper expression equality
      EXPECT_EQ(typeid(scan_all.upper_bound_->value()).hash_code(),
                typeid(upper_bound_->value()).hash_code());
      EXPECT_EQ(scan_all.upper_bound_->type(), upper_bound_->type());
    }
  }

 private:
  storage::Label label_;
  storage::Property property_;
  std::experimental::optional<ScanAllByLabelPropertyRange::Bound> lower_bound_;
  std::experimental::optional<ScanAllByLabelPropertyRange::Bound> upper_bound_;
};

class ExpectCartesian : public OpChecker<Cartesian> {
 public:
  ExpectCartesian(const std::list<std::unique_ptr<BaseOpChecker>> &left,
                  const std::list<std::unique_ptr<BaseOpChecker>> &right)
      : left_(left), right_(right) {}

  void ExpectOp(Cartesian &op, const SymbolTable &symbol_table) override {
    ASSERT_TRUE(op.left_op_);
    PlanChecker left_checker(left_, symbol_table);
    op.left_op_->Accept(left_checker);
    ASSERT_TRUE(op.right_op_);
    PlanChecker right_checker(right_, symbol_table);
    op.right_op_->Accept(right_checker);
  }

 private:
  const std::list<std::unique_ptr<BaseOpChecker>> &left_;
  const std::list<std::unique_ptr<BaseOpChecker>> &right_;
};

template <class T>
std::list<std::unique_ptr<BaseOpChecker>> MakeCheckers(T arg) {
  std::list<std::unique_ptr<BaseOpChecker>> l;
  l.emplace_back(std::make_unique<T>(arg));
  return l;
}

template <class T, class... Rest>
std::list<std::unique_ptr<BaseOpChecker>> MakeCheckers(T arg, Rest &&... rest) {
  auto l = MakeCheckers(std::forward<Rest>(rest)...);
  l.emplace_front(std::make_unique<T>(arg));
  return std::move(l);
}

template <class TPlanner, class TDbAccessor>
TPlanner MakePlanner(TDbAccessor *dba, AstStorage &storage,
                     SymbolTable &symbol_table, CypherQuery *query) {
  auto planning_context =
      MakePlanningContext(&storage, &symbol_table, query, dba);
  auto query_parts = CollectQueryParts(symbol_table, storage, query);
  auto single_query_parts = query_parts.query_parts.at(0).single_query_parts;
  return TPlanner(single_query_parts, planning_context);
}

class FakeDbAccessor {
 public:
  int64_t VerticesCount(storage::Label label) const {
    auto found = label_index_.find(label);
    if (found != label_index_.end()) return found->second;
    return 0;
  }

  int64_t VerticesCount(storage::Label label,
                        storage::Property property) const {
    for (auto &index : label_property_index_) {
      if (std::get<0>(index) == label && std::get<1>(index) == property) {
        return std::get<2>(index);
      }
    }
    return 0;
  }

  bool LabelPropertyIndexExists(storage::Label label,
                                storage::Property property) const {
    for (auto &index : label_property_index_) {
      if (std::get<0>(index) == label && std::get<1>(index) == property) {
        return true;
      }
    }
    return false;
  }

  void SetIndexCount(storage::Label label, int64_t count) {
    label_index_[label] = count;
  }

  void SetIndexCount(storage::Label label, storage::Property property,
                     int64_t count) {
    for (auto &index : label_property_index_) {
      if (std::get<0>(index) == label && std::get<1>(index) == property) {
        std::get<2>(index) = count;
        return;
      }
    }
    label_property_index_.emplace_back(label, property, count);
  }

  storage::Label Label(const std::string &name) {
    auto found = labels_.find(name);
    if (found != labels_.end()) return found->second;
    return labels_.emplace(name, storage::Label(labels_.size())).first->second;
  }

  storage::EdgeType EdgeType(const std::string &name) {
    auto found = edge_types_.find(name);
    if (found != edge_types_.end()) return found->second;
    return edge_types_.emplace(name, storage::EdgeType(edge_types_.size()))
        .first->second;
  }

  storage::Property Property(const std::string &name) {
    auto found = properties_.find(name);
    if (found != properties_.end()) return found->second;
    return properties_.emplace(name, storage::Property(properties_.size()))
        .first->second;
  }

  std::string PropertyName(storage::Property property) const {
    for (const auto &kv : properties_) {
      if (kv.second == property) return kv.first;
    }
    LOG(FATAL) << "Unable to find property name";
  }

 private:
  std::unordered_map<std::string, storage::Label> labels_;
  std::unordered_map<std::string, storage::EdgeType> edge_types_;
  std::unordered_map<std::string, storage::Property> properties_;

  std::unordered_map<storage::Label, int64_t> label_index_;
  std::vector<std::tuple<storage::Label, storage::Property, int64_t>>
      label_property_index_;
};

}  // namespace query::plan
