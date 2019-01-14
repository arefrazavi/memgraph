#pragma once

#include "database/graph_db_accessor.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/parameters.hpp"
#include "query/plan/profile.hpp"

namespace query {

struct EvaluationContext {
  int64_t timestamp{-1};
  Parameters parameters;
  /// All properties indexable via PropertyIx
  std::vector<storage::Property> properties;
  /// All labels indexable via LabelIx
  std::vector<storage::Label> labels;
};

inline std::vector<storage::Property> NamesToProperties(
    const std::vector<std::string> &property_names,
    database::GraphDbAccessor *dba) {
  std::vector<storage::Property> properties;
  properties.reserve(property_names.size());
  for (const auto &name : property_names) {
    properties.push_back(dba->Property(name));
  }
  return properties;
}

inline std::vector<storage::Label> NamesToLabels(
    const std::vector<std::string> &label_names,
    database::GraphDbAccessor *dba) {
  std::vector<storage::Label> labels;
  labels.reserve(label_names.size());
  for (const auto &name : label_names) {
    labels.push_back(dba->Label(name));
  }
  return labels;
}

class Context {
 public:
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Context(Context &&) = default;
  Context &operator=(Context &&) = default;

  explicit Context(database::GraphDbAccessor &db_accessor)
      : db_accessor_(db_accessor) {}

  database::GraphDbAccessor &db_accessor_;
  SymbolTable symbol_table_;
  EvaluationContext evaluation_context_;
  bool is_profile_query_{false};
  plan::ProfilingStats stats_;
  plan::ProfilingStats *stats_root_{nullptr};
};

// TODO: Move this to somewhere in query/frontend. Currently, frontend includes
// this and therefore implicitly includes the whole database because of the
// includes at the top of this file.
struct ParsingContext {
  bool is_query_cached = false;
};

}  // namespace query
