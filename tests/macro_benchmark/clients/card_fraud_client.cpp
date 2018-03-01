#include <memory>
#include <random>
#include <vector>

#include "gflags/gflags.h"

#include "long_running_common.hpp"
#include "stats/stats.hpp"
#include "stats/stats_rpc_messages.hpp"

// TODO(mtomic): this sucks but I don't know a different way to make it work
#include "boost/archive/binary_iarchive.hpp"
#include "boost/archive/binary_oarchive.hpp"
#include "boost/serialization/export.hpp"
BOOST_CLASS_EXPORT(stats::StatsReq);
BOOST_CLASS_EXPORT(stats::StatsRes);
BOOST_CLASS_EXPORT(stats::BatchStatsReq);
BOOST_CLASS_EXPORT(stats::BatchStatsRes);

std::atomic<int> num_pos;
std::atomic<int> num_cards;
std::atomic<int> num_transactions;

class CardFraudClient : public TestClient {
 public:
  CardFraudClient(int id) : TestClient(), rg_(id) {}

 private:
  std::mt19937 rg_;

  auto GetFraudulentTransactions() {
    return Execute(
        "MATCH (t:Transaction {fraud_reported: true}) "
        "RETURN t.id as id",
        {});
  }

  auto GetCompromisedPos() {
    return Execute(
        "MATCH (t:Transaction {fraud_reported: true})-[:Using]->(:Card)"
        "<-[:Using]-(:Transaction)-[:At]->(p:Pos) "
        "WITH p.id as pos, count(t) as connected_frauds "
        "WHERE connected_frauds > 1 "
        "RETURN pos, connected_frauds ORDER BY connected_frauds DESC",
        {});
  }

  auto ResolvePos(int id) {
    return Execute(
        "MATCH (p:Pos {id: $id}) "
        "SET p.compromised = false "
        "WITH p MATCH (p)--(t:Transaction)--(c:Card) "
        "SET t.fraud_reported = false, c.compromised = false",
        {{"id", id}});
  }

  auto GetTransaction(int id) {
    return Execute("MATCH (t:Transaction {id: $id}) RETURN (t)", {{"id", id}});
  }

  auto TepsQuery() {
    auto result = Execute("MATCH (u)--(v) RETURN count(1)", {});
    DCHECK(result.records[0][0].ValueInt() == num_transactions * 2);
  }

  auto CompromisePos(int id) {
    return Execute(
        "MATCH (p:Pos {id: $id}) "
        "SET p.compromised = true "
        "WITH p MATCH (p)--(t:Transaction)--(c:Card) "
        "SET t.fraud_reported = false, c.compromised = true",
        {{"id", id}});
  }

  auto CreateTransaction(int pos_id, int card_id) {
    return Execute(
        "MATCH (p:Pos {id: $pos_id}), (c:Card {id: $card_id}) "
        "CREATE (t:Transaction {id: $tx_id, fraud_reported: false}) "
        "CREATE (c)<-[:Using]-(t)-[:At]->(p)",
        {{"pos_id", pos_id},
         {"card_id", card_id},
         {"tx_id", num_transactions++}});
  }

  auto CreateTransactionWithoutEdge(int pos_id, int card_id) {
    return Execute(
        "MATCH (p:Pos {id: $pos_id}), (c:Card {id: $card_id}) "
        "CREATE (t:Transaction {id: $tx_id, fraud_reported: false})",
        {{"pos_id", pos_id},
         {"card_id", card_id},
         {"tx_id", num_transactions++}});
  }

 public:
  virtual void Step() override {
    if (FLAGS_scenario == "read_only") {
      std::uniform_int_distribution<int> dist(0, 1);
      if (dist(rg_)) {
        GetFraudulentTransactions();
      } else {
        GetCompromisedPos();
      }
    } else if (FLAGS_scenario == "read_write") {
      std::uniform_int_distribution<int> dist(0, num_pos - 1);
      int pos_id = dist(rg_);
      CompromisePos(pos_id);
      GetFraudulentTransactions();
      ResolvePos(pos_id);
    } else if (FLAGS_scenario == "teps") {
      TepsQuery();
    } else if (FLAGS_scenario == "point_lookup") {
      std::uniform_int_distribution<int> dist(0, num_transactions - 1);
      int tx_id = dist(rg_);
      GetTransaction(tx_id);
    } else if (FLAGS_scenario == "create_tx") {
      std::uniform_int_distribution<int> dist_pos(0, num_pos - 1);
      std::uniform_int_distribution<int> dist_card(0, num_cards - 1);
      CreateTransaction(dist_pos(rg_), dist_card(rg_));
    } else if (FLAGS_scenario == "create_tx_without_edge") {
      std::uniform_int_distribution<int> dist_pos(0, num_pos - 1);
      std::uniform_int_distribution<int> dist_card(0, num_cards - 1);
      CreateTransactionWithoutEdge(dist_pos(rg_), dist_card(rg_));
    } else {
      LOG(FATAL) << "Should not get here: unknown scenario!";
    }
  }
};

int64_t NumNodesWithLabel(BoltClient &client, std::string label) {
  std::string query = fmt::format("MATCH (u :{}) RETURN COUNT(u)", label);
  auto result = ExecuteNTimesTillSuccess(client, query, {}, MAX_RETRIES);
  return result.records[0][0].ValueInt();
}

void CreateIndex(BoltClient &client, const std::string &label,
                 const std::string &property) {
  LOG(INFO) << fmt::format("Creating indexes for :{}({})...", label, property);
  ExecuteNTimesTillSuccess(
      client, fmt::format("CREATE INDEX ON :{}({});", label, property), {},
      MAX_RETRIES);
  try {
    LOG(INFO) << fmt::format("Trying to sync indexes...");
    ExecuteNTimesTillSuccess(client, "CALL db.awaitIndexes(14400);", {},
                             MAX_RETRIES);
  } catch (utils::BasicException &e) {
    LOG(WARNING) << "Index sync failed: " << e.what();
  }
}

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  stats::InitStatsLogging(
      fmt::format("client.long_running.{}.{}", FLAGS_group, FLAGS_scenario));

  BoltClient client(FLAGS_address, FLAGS_port, FLAGS_username, FLAGS_password);

  num_pos.store(NumNodesWithLabel(client, "Pos"));
  num_cards.store(NumNodesWithLabel(client, "Card"));
  num_transactions.store(NumNodesWithLabel(client, "Transaction"));

  CreateIndex(client, "Pos", "id");
  CreateIndex(client, "Card", "id");
  CreateIndex(client, "Transaction", "fraud_reported");
  CreateIndex(client, "Transaction", "id");
  LOG(INFO) << "Done building indexes.";

  client.Close();

  std::vector<std::unique_ptr<TestClient>> clients;
  for (int i = 0; i < FLAGS_num_workers; ++i) {
    clients.emplace_back(std::make_unique<CardFraudClient>(i));
  }

  RunMultithreadedTest(clients);

  stats::StopStatsLogging();

  return 0;
}