#include "Dataflow/WPDS/Backend/PreparedBackend.h"
#include "Dataflow/WPDS/Core/GenKillTransformer.h"
#include "WPDS/CA.h"
#include "WPDS/SaturationProcess.h"
#include "WPDS/WPDS.h"
#include "WPDS/key_source.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace wpds::backend {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

GenKillTransformer *legacyWeight(const GenKillValue &value) {
  switch (value.getKind()) {
  case GenKillValue::Kind::One:
    return GenKillTransformer::one();
  case GenKillValue::Kind::Zero:
    return GenKillTransformer::zero();
  case GenKillValue::Kind::Bottom:
    return GenKillTransformer::bottom();
  case GenKillValue::Kind::Ordinary:
    return GenKillTransformer::makeGenKillTransformer(
        value.getKill(), value.getGen(), value.getFlow());
  }
  return nullptr;
}

class LegacyPreparedBackend final : public PreparedBackend {
public:
  LegacyPreparedBackend(const Model &model, WPDSQueryKind queryKind)
      : queryKind(queryKind), semiring(GenKillTransformer::one(), true),
        pds(semiring, queryKind == WPDSQueryKind::PostStar
                          ? ::wpds::Query::poststar()
                          : ::wpds::Query::prestar()) {
    auto start = Clock::now();
    static std::atomic<std::uint64_t> nextSession{0};
    const std::string prefix =
        "lotus_wpds_" + std::to_string(++nextSession) + "_";

    controlKeys.reserve(model.controlStateNames().size());
    for (const std::string &name : model.controlStateNames()) {
      controlKeys.push_back(new_str2key((prefix + "p_" + name).c_str()));
    }
    stackKeys.resize(model.stackSymbolNames().size(), WPDS_EPSILON);
    for (StackSymbolId id = 1; id < model.stackSymbolNames().size(); ++id) {
      stackKeys[id] =
          new_str2key((prefix + "g_" + model.stackSymbolName(id)).c_str());
    }

    for (const Rule &rule : model.rules()) {
      GenKillTransformer *weight = legacyWeight(rule.weight);
      switch (rule.kind) {
      case RuleKind::Pop:
        pds.add_rule(controlKeys[rule.fromState], stackKeys[rule.fromStack],
                     controlKeys[rule.toState], weight);
        ++stats.popRuleCount;
        break;
      case RuleKind::Replace:
        pds.add_rule(controlKeys[rule.fromState], stackKeys[rule.fromStack],
                     controlKeys[rule.toState], stackKeys[rule.toStack1],
                     weight);
        ++stats.replaceRuleCount;
        break;
      case RuleKind::Push:
        pds.add_rule(controlKeys[rule.fromState], stackKeys[rule.fromStack],
                     controlKeys[rule.toState], stackKeys[rule.toStack1],
                     stackKeys[rule.toStack2], weight);
        ++stats.pushRuleCount;
        break;
      }
    }

    stats.selectedBackend = WPDSBackendKind::Legacy;
    stats.effectiveBackend = WPDSBackendKind::Legacy;
    stats.query = queryKind;
    stats.controlStateCount = model.controlStateNames().size();
    stats.stackSymbolCount = model.stackSymbolNames().size() - 1;
    stats.preparationCount = 1;
    stats.preparationMilliseconds = milliseconds(start, Clock::now());
  }

  bool solve(const Query &query, QueryResult &result,
             std::string &error) override {
    if (query.operation != queryKind) {
      error = "prepared WPDS session query direction mismatch";
      return false;
    }
    if (query.initialState >= controlKeys.size()) {
      error = "WPDS query refers to an unknown control state";
      return false;
    }
    if (query.roots.empty()) {
      error = "WPDS query must contain at least one root symbol";
      return false;
    }
    for (StackSymbolId root : query.roots) {
      if (root == Epsilon || root >= stackKeys.size()) {
        error = "WPDS query refers to an unknown or epsilon root symbol";
        return false;
      }
    }

    auto solveStart = Clock::now();
    CA<GenKillTransformer> input(semiring);
    const wpds_key_t initial = controlKeys[query.initialState];
    const wpds_key_t accept = new_str2key(
        ("lotus_wpds_accept_" + std::to_string(++querySerial)).c_str());
    input.add_initial_state(initial);
    input.add_final_state(accept);
    for (StackSymbolId root : query.roots) {
      input.add(initial, stackKeys[root], accept, legacyWeight(query.seed));
    }

    lastResult = std::make_unique<CA<GenKillTransformer>>(input);
    SaturationProcess<GenKillTransformer> saturation(
        pds, *lastResult, semiring,
        queryKind == WPDSQueryKind::PostStar ? ::wpds::Query::poststar()
                                             : ::wpds::Query::prestar());
    if (queryKind == WPDSQueryKind::PostStar) {
      saturation.poststar();
    } else {
      saturation.prestar();
    }
    stats.solvingMilliseconds += milliseconds(solveStart, Clock::now());

    auto decodeStart = Clock::now();
    result.observations.clear();
    const wpds_key_t queryInitial = lastResult->initial_state();
    for (StackSymbolId id = 1; id < stackKeys.size(); ++id) {
      CA<GenKillTransformer> language(semiring);
      const wpds_key_t final = new_str2key(
          ("lotus_wpds_observe_" + std::to_string(++querySerial)).c_str());
      language.add_initial_state(queryInitial);
      language.add_final_state(final);
      language.add(queryInitial, stackKeys[id], final,
                   GenKillTransformer::one());
      if (query.observation == WPDSObservationKind::StackPrefix) {
        for (StackSymbolId suffix = 1; suffix < stackKeys.size(); ++suffix) {
          language.add(final, stackKeys[suffix], final,
                       GenKillTransformer::one());
        }
      }
      ::ref_ptr<GenKillTransformer> summary =
          lastResult->reglang_query(language);
      Observation observation;
      if (summary.get_ptr() != nullptr &&
          !summary->equal(GenKillTransformer::zero())) {
        observation.reachable = true;
        observation.summary = summary->getValue();
      }
      result.observations.emplace(id, std::move(observation));
    }
    stats.decodingMilliseconds += milliseconds(decodeStart, Clock::now());
    ++stats.queryCount;
    return true;
  }

  const WPDSBackendStatistics &statistics() const override { return stats; }

  const CA<GenKillTransformer> *legacyResultAutomaton() const override {
    return lastResult.get();
  }

  unsigned long legacyKeyForSymbol(StackSymbolId id) const override {
    return id < stackKeys.size() ? stackKeys[id] : WPDS_EPSILON;
  }

private:
  WPDSQueryKind queryKind;
  Semiring<GenKillTransformer> semiring;
  WPDS<GenKillTransformer> pds;
  std::vector<wpds_key_t> controlKeys;
  std::vector<wpds_key_t> stackKeys;
  std::unique_ptr<CA<GenKillTransformer>> lastResult;
  WPDSBackendStatistics stats;
  std::uint64_t querySerial = 0;
};

} // namespace

std::unique_ptr<PreparedBackend> prepareLegacyBackend(const Model &model,
                                                      WPDSQueryKind queryKind) {
  return std::make_unique<LegacyPreparedBackend>(model, queryKind);
}

} // namespace wpds::backend
