#include "Dataflow/WPDS/Backend/PreparedBackend.h"
#include "wali/Common.hpp"
#include "wali/Key.hpp"
#include "wali/SemElem.hpp"
#include "wali/wfa/State.hpp"
#include "wali/wfa/WFA.hpp"
#include "wali/wfa/WeightMaker.hpp"
#include "wali/wpds/fwpds/FWPDS.hpp"
#include "wali/wpds/fwpds/SWPDS.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <tuple>
#include <vector>

namespace wpds::backend {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::recursive_mutex &waliRuntimeMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

int compareFacts(const DataFlowFacts &left, const DataFlowFacts &right) {
  if (left.isUniverse() != right.isUniverse()) {
    return left.isUniverse() ? 1 : -1;
  }
  if (left.getFacts() < right.getFacts()) {
    return -1;
  }
  if (right.getFacts() < left.getFacts()) {
    return 1;
  }
  return 0;
}

int compareValues(const GenKillValue &left, const GenKillValue &right) {
  if (left.getKind() != right.getKind()) {
    return static_cast<int>(left.getKind()) < static_cast<int>(right.getKind())
               ? -1
               : 1;
  }
  if (int comparison = compareFacts(left.getKill(), right.getKill())) {
    return comparison;
  }
  if (int comparison = compareFacts(left.getGen(), right.getGen())) {
    return comparison;
  }
  auto leftFlow = left.getFlow().begin();
  auto rightFlow = right.getFlow().begin();
  while (leftFlow != left.getFlow().end() &&
         rightFlow != right.getFlow().end()) {
    if (std::less<Value *>{}(leftFlow->first, rightFlow->first)) {
      return -1;
    }
    if (std::less<Value *>{}(rightFlow->first, leftFlow->first)) {
      return 1;
    }
    if (int comparison = compareFacts(leftFlow->second, rightFlow->second)) {
      return comparison;
    }
    ++leftFlow;
    ++rightFlow;
  }
  if (leftFlow != left.getFlow().end()) {
    return 1;
  }
  if (rightFlow != right.getFlow().end()) {
    return -1;
  }
  return 0;
}

std::size_t hashFacts(const DataFlowFacts &facts) {
  std::size_t hash = facts.isUniverse() ? 0x9e3779b9U : 0x85ebca6bU;
  for (Value *fact : facts.getFacts()) {
    hash ^=
        std::hash<Value *>{}(fact) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
  }
  return hash;
}

std::size_t hashValue(const GenKillValue &value) {
  std::size_t hash = static_cast<std::size_t>(value.getKind());
  auto mix = [&hash](std::size_t part) {
    hash ^= part + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
  };
  mix(hashFacts(value.getKill()));
  mix(hashFacts(value.getGen()));
  for (const auto &entry : value.getFlow()) {
    mix(std::hash<Value *>{}(entry.first));
    mix(hashFacts(entry.second));
  }
  return hash;
}

class WaliGenKillWeight final : public wali::SemElem {
public:
  explicit WaliGenKillWeight(GenKillValue value) : value(std::move(value)) {}

  wali::sem_elem_t one() const override {
    return wali::sem_elem_t(new WaliGenKillWeight(GenKillValue::one()));
  }

  wali::sem_elem_t zero() const override {
    return wali::sem_elem_t(new WaliGenKillWeight(GenKillValue::zero()));
  }

  wali::sem_elem_t extend(wali::SemElem *other) override {
    auto *rhs = dynamic_cast<WaliGenKillWeight *>(other);
    assert(rhs != nullptr && "mixed WALi weight domains");
    return wali::sem_elem_t(new WaliGenKillWeight(value.extend(rhs->value)));
  }

  wali::sem_elem_t combine(wali::SemElem *other) override {
    auto *rhs = dynamic_cast<WaliGenKillWeight *>(other);
    assert(rhs != nullptr && "mixed WALi weight domains");
    return wali::sem_elem_t(new WaliGenKillWeight(value.combine(rhs->value)));
  }

  wali::sem_elem_t diff(wali::SemElem *other) override {
    auto *rhs = dynamic_cast<WaliGenKillWeight *>(other);
    assert(rhs != nullptr && "mixed WALi weight domains");
    return wali::sem_elem_t(new WaliGenKillWeight(value.diff(rhs->value)));
  }

  wali::sem_elem_t quasi_one() const override {
    return wali::sem_elem_t(new WaliGenKillWeight(value.quasiOne()));
  }

  bool equal(wali::SemElem *other) const override {
    auto *rhs = dynamic_cast<WaliGenKillWeight *>(other);
    return rhs != nullptr && value.equal(rhs->value);
  }

  bool containerLessThan(const wali::SemElem *other) const override {
    auto *rhs = dynamic_cast<const WaliGenKillWeight *>(other);
    assert(rhs != nullptr && "mixed WALi weight domains");
    return compareValues(value, rhs->value) < 0;
  }

  std::size_t hash() const override { return hashValue(value); }

  std::ostream &print(std::ostream &stream) const override {
    return value.print(stream);
  }

  const GenKillValue &getValue() const { return value; }

private:
  GenKillValue value;
};

wali::sem_elem_t waliWeight(const GenKillValue &value) {
  return wali::sem_elem_t(new WaliGenKillWeight(value));
}

class WaliPreparedBackend final : public PreparedBackend {
public:
  WaliPreparedBackend(const Model &model, const WPDSBackendOptions &options,
                      WPDSQueryKind queryKind,
                      const std::vector<StackSymbolId> &preprocessEntries,
                      std::string &error)
      : options(options), queryKind(queryKind) {
    std::lock_guard<std::recursive_mutex> lock(waliRuntimeMutex());
    auto start = Clock::now();
    static std::atomic<std::uint64_t> nextSession{0};
    prefix = "lotus_wpds_wali_" + std::to_string(++nextSession) + "_";

    if (model.controlStateNames().size() != 1) {
      error = "WALi SWPDS/FWPDS adapter currently requires a single-control-"
              "state Lotus model";
      return;
    }
    if (model.rules().empty()) {
      error = "WALi cannot prepare a WPDS model with no rules";
      return;
    }
    acceptKey = wali::getKey(prefix + "accept");
    observationInitialKey = wali::getKey(prefix + "observe_initial");
    observationFinalKey = wali::getKey(prefix + "observe_final");

    controlKeys.reserve(model.controlStateNames().size());
    for (const std::string &name : model.controlStateNames()) {
      controlKeys.push_back(wali::getKey(prefix + "p_" + name));
    }
    stackKeys.resize(model.stackSymbolNames().size(), wali::WALI_EPSILON);
    for (StackSymbolId id = 1; id < model.stackSymbolNames().size(); ++id) {
      stackKeys[id] = wali::getKey(prefix + "g_" + model.stackSymbolName(id));
    }

    if (options.backend == WPDSBackendKind::WaliSWPDS) {
      pds = std::make_unique<wali::wpds::fwpds::SWPDS>();
    } else {
      pds = std::make_unique<wali::wpds::fwpds::FWPDS>();
    }

    for (const Rule &rule : model.rules()) {
      wali::sem_elem_t weight = waliWeight(rule.weight);
      switch (rule.kind) {
      case RuleKind::Pop:
        pds->add_rule(controlKeys[rule.fromState], stackKeys[rule.fromStack],
                      controlKeys[rule.toState], weight);
        ++stats.popRuleCount;
        break;
      case RuleKind::Replace:
        pds->add_rule(controlKeys[rule.fromState], stackKeys[rule.fromStack],
                      controlKeys[rule.toState], stackKeys[rule.toStack1],
                      weight);
        ++stats.replaceRuleCount;
        break;
      case RuleKind::Push:
        pds->add_rule(controlKeys[rule.fromState], stackKeys[rule.fromStack],
                      controlKeys[rule.toState], stackKeys[rule.toStack1],
                      stackKeys[rule.toStack2], weight);
        ++stats.pushRuleCount;
        break;
      }
    }

    if (options.backend == WPDSBackendKind::WaliSWPDS) {
      if (preprocessEntries.empty()) {
        error = "WALi SWPDS preparation requires at least one program entry";
        pds.reset();
        return;
      }
      auto *swpds = static_cast<wali::wpds::fwpds::SWPDS *>(pds.get());
      for (StackSymbolId entry : preprocessEntries) {
        if (entry == Epsilon || entry >= stackKeys.size()) {
          error = "WALi SWPDS preparation received an invalid entry symbol";
          pds.reset();
          return;
        }
        swpds->addEntryPoint(stackKeys[entry]);
      }
      swpds->preprocess();
    }

    stats.selectedBackend = options.backend;
    stats.effectiveBackend = options.backend;
    stats.query = queryKind;
    stats.controlStateCount = model.controlStateNames().size();
    stats.stackSymbolCount = model.stackSymbolNames().size() - 1;
    stats.preparationCount = 1;
    stats.preparationMilliseconds = milliseconds(start, Clock::now());
    valid = true;
  }

  ~WaliPreparedBackend() override {
    std::lock_guard<std::recursive_mutex> lock(waliRuntimeMutex());
    lastOutput.reset();
    pds.reset();
  }

  bool isValid() const { return valid; }

  bool solve(const Query &query, QueryResult &result,
             std::string &error) override {
    std::lock_guard<std::recursive_mutex> lock(waliRuntimeMutex());
    if (!valid || !pds) {
      error = "WALi WPDS session is not prepared";
      return false;
    }
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
    if (options.backend == WPDSBackendKind::WaliSWPDS &&
        query.operation == WPDSQueryKind::PreStar) {
      auto *swpds = static_cast<wali::wpds::fwpds::SWPDS *>(pds.get());
      for (StackSymbolId root : query.roots) {
        if (swpds->multiple_proc(stackKeys[root])) {
          error = "WALi SWPDS prestar does not support a query symbol that "
                  "belongs to multiple procedures";
          return false;
        }
      }
    }

    auto solveStart = Clock::now();
    wali::wfa::WFA input;
    const wali::Key initial = controlKeys[query.initialState];
    for (StackSymbolId root : query.roots) {
      input.addTrans(initial, stackKeys[root], acceptKey,
                     waliWeight(query.seed));
    }
    input.setInitialState(initial);
    input.addFinalState(acceptKey);

    lastOutput = std::make_unique<wali::wfa::WFA>();
    if (query.operation == WPDSQueryKind::PostStar) {
      pds->poststar(input, *lastOutput);
    } else {
      pds->prestar(input, *lastOutput);
    }
    stats.solvingMilliseconds += milliseconds(solveStart, Clock::now());

    auto decodeStart = Clock::now();
    result.observations.clear();
    if (query.observation == WPDSObservationKind::StackPrefix) {
      lastOutput->path_summary_iterative_original();
    }
    for (StackSymbolId id = 1; id < stackKeys.size(); ++id) {
      result.observations.emplace(id, query.observation ==
                                              WPDSObservationKind::ExactStack
                                          ? decodeExact(id)
                                          : decodePrefix(initial, id));
    }
    stats.decodingMilliseconds += milliseconds(decodeStart, Clock::now());
    ++stats.queryCount;
    return true;
  }

  const WPDSBackendStatistics &statistics() const override { return stats; }

private:
  Observation decodePrefix(wali::Key initial, StackSymbolId symbol) {
    Observation observation;
    wali::sem_elem_t combined = waliWeight(GenKillValue::zero());
    wali::wfa::TransSet transitions =
        lastOutput->match(initial, stackKeys[symbol]);
    for (wali::wfa::ITrans *transition : transitions) {
      wali::wfa::State *suffix = lastOutput->getState(transition->to());
      if (suffix == nullptr || !suffix->weight().is_valid()) {
        continue;
      }
      wali::sem_elem_t path =
          queryKind == WPDSQueryKind::PostStar
              ? suffix->weight()->extend(transition->weight())
              : transition->weight()->extend(suffix->weight());
      combined = combined->combine(path);
    }
    auto *weight = dynamic_cast<WaliGenKillWeight *>(combined.get_ptr());
    if (weight != nullptr &&
        weight->getValue().getKind() != GenKillValue::Kind::Zero) {
      observation.reachable = true;
      observation.summary = weight->getValue();
    }
    return observation;
  }

  Observation decodeExact(StackSymbolId symbol) {
    Observation observation;
    wali::wfa::WFA language;
    language.addTrans(observationInitialKey, stackKeys[symbol],
                      observationFinalKey, waliWeight(GenKillValue::one()));
    language.setInitialState(observationInitialKey);
    language.addFinalState(observationFinalKey);

    wali::wfa::KeepLeft keepWeights;
    wali::wfa::WFA intersection;
    lastOutput->intersect_cross(keepWeights, language, intersection);
    if (intersection.numStates() == 0) {
      return observation;
    }
    intersection.path_summary_iterative_original();
    wali::wfa::State *initial =
        intersection.getState(intersection.getInitialState());
    if (initial == nullptr || !initial->weight().is_valid()) {
      return observation;
    }
    auto *weight =
        dynamic_cast<WaliGenKillWeight *>(initial->weight().get_ptr());
    if (weight != nullptr &&
        weight->getValue().getKind() != GenKillValue::Kind::Zero) {
      observation.reachable = true;
      observation.summary = weight->getValue();
    }
    return observation;
  }

  WPDSBackendOptions options;
  WPDSQueryKind queryKind;
  std::string prefix;
  std::vector<wali::Key> controlKeys;
  std::vector<wali::Key> stackKeys;
  wali::Key acceptKey = wali::WALI_EPSILON;
  wali::Key observationInitialKey = wali::WALI_EPSILON;
  wali::Key observationFinalKey = wali::WALI_EPSILON;
  std::unique_ptr<wali::wpds::fwpds::FWPDS> pds;
  std::unique_ptr<wali::wfa::WFA> lastOutput;
  WPDSBackendStatistics stats;
  bool valid = false;
};

} // namespace

std::unique_ptr<PreparedBackend>
prepareWaliBackend(const Model &model, const WPDSBackendOptions &options,
                   WPDSQueryKind queryKind,
                   const std::vector<StackSymbolId> &preprocessEntries,
                   std::string &error) {
  auto prepared = std::make_unique<WaliPreparedBackend>(
      model, options, queryKind, preprocessEntries, error);
  if (!prepared->isValid()) {
    return nullptr;
  }
  return prepared;
}

} // namespace wpds::backend
