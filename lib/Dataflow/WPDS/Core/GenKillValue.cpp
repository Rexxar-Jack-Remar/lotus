#include "Dataflow/WPDS/Core/GenKillValue.h"

namespace wpds {

GenKillValue::GenKillValue()
    : kind(Kind::Ordinary), kill(DataFlowFacts::EmptySet()),
      gen(DataFlowFacts::EmptySet()) {}

GenKillValue::GenKillValue(const DataFlowFacts &kill, const DataFlowFacts &gen,
                           const std::map<Value *, DataFlowFacts> &flow)
    : kind(Kind::Ordinary), kill(DataFlowFacts::Diff(kill, gen)), gen(gen),
      flow(flow) {}

GenKillValue::GenKillValue(Kind kind, const DataFlowFacts &kill,
                           const DataFlowFacts &gen,
                           const std::map<Value *, DataFlowFacts> &flow)
    : kind(kind), kill(kill), gen(gen), flow(flow) {}

GenKillValue GenKillValue::one() {
  return GenKillValue(Kind::One, DataFlowFacts::EmptySet(),
                      DataFlowFacts::EmptySet(), {});
}

GenKillValue GenKillValue::zero() {
  return GenKillValue(Kind::Zero, DataFlowFacts::UniverseSet(),
                      DataFlowFacts::EmptySet(), {});
}

GenKillValue GenKillValue::bottom() {
  return GenKillValue(Kind::Bottom, DataFlowFacts::EmptySet(),
                      DataFlowFacts::UniverseSet(), {});
}

GenKillValue
GenKillValue::normalized(const DataFlowFacts &kill, const DataFlowFacts &gen,
                         const std::map<Value *, DataFlowFacts> &flow) {
  DataFlowFacts normalizedKill = DataFlowFacts::Diff(kill, gen);
  std::map<Value *, DataFlowFacts> normalizedFlow;
  for (const auto &entry : flow) {
    DataFlowFacts targets = DataFlowFacts::Diff(entry.second, gen);
    if (!normalizedKill.containsFact(entry.first)) {
      targets.removeFact(entry.first);
    }
    if (!targets.isEmpty()) {
      normalizedFlow.emplace(entry.first, std::move(targets));
    }
  }
  const bool flowEmpty = normalizedFlow.empty();

  if (DataFlowFacts::Eq(normalizedKill, DataFlowFacts::EmptySet()) &&
      DataFlowFacts::Eq(gen, DataFlowFacts::UniverseSet()) && flowEmpty) {
    return bottom();
  }
  if (DataFlowFacts::Eq(normalizedKill, DataFlowFacts::EmptySet()) &&
      DataFlowFacts::Eq(gen, DataFlowFacts::EmptySet()) && flowEmpty) {
    return one();
  }
  return GenKillValue(normalizedKill, gen, normalizedFlow);
}

std::set<Value *> GenKillValue::collectRelevantValues(const GenKillValue &lhs,
                                                      const GenKillValue &rhs) {
  std::set<Value *> values;
  auto collectFacts = [&values](const DataFlowFacts &facts) {
    values.insert(facts.getFacts().begin(), facts.getFacts().end());
  };
  auto collectFlow = [&values, &collectFacts](
                         const std::map<Value *, DataFlowFacts> &mapping) {
    for (const auto &entry : mapping) {
      values.insert(entry.first);
      collectFacts(entry.second);
    }
  };

  collectFacts(lhs.kill);
  collectFacts(lhs.gen);
  collectFacts(rhs.kill);
  collectFacts(rhs.gen);
  collectFlow(lhs.flow);
  collectFlow(rhs.flow);
  return values;
}

GenKillValue GenKillValue::extend(const GenKillValue &other) const {
  if (kind == Kind::Zero || other.kind == Kind::Zero) {
    return zero();
  }
  if (kind == Kind::One) {
    return other;
  }
  if (other.kind == Kind::One) {
    return *this;
  }

  DataFlowFacts composedGen = other.apply(apply(DataFlowFacts::EmptySet()));
  const bool preservesUnknown = !kill.isUniverse() && !other.kill.isUniverse();
  DataFlowFacts composedKill = preservesUnknown ? DataFlowFacts::EmptySet()
                                                : DataFlowFacts::UniverseSet();
  std::map<Value *, DataFlowFacts> composedFlow;

  for (Value *value : collectRelevantValues(*this, other)) {
    DataFlowFacts input;
    input.addFact(value);
    DataFlowFacts output = other.apply(apply(input));
    DataFlowFacts basis = DataFlowFacts::Diff(output, composedGen);
    const bool preservesValue = basis.containsFact(value);
    if (preservesUnknown) {
      if (!preservesValue) {
        composedKill.addFact(value);
      }
    } else if (preservesValue) {
      composedKill.removeFact(value);
    }
    basis.removeFact(value);
    if (!basis.isEmpty()) {
      composedFlow[value] = basis;
    }
  }
  return normalized(composedKill, composedGen, composedFlow);
}

GenKillValue GenKillValue::combine(const GenKillValue &other) const {
  if (kind == Kind::Zero) {
    return other;
  }
  if (other.kind == Kind::Zero) {
    return *this;
  }

  DataFlowFacts joinedGen = DataFlowFacts::Union(gen, other.gen);
  const bool preservesUnknown = !kill.isUniverse() || !other.kill.isUniverse();
  DataFlowFacts joinedKill = preservesUnknown ? DataFlowFacts::EmptySet()
                                              : DataFlowFacts::UniverseSet();
  std::map<Value *, DataFlowFacts> joinedFlow;

  for (Value *value : collectRelevantValues(*this, other)) {
    DataFlowFacts input;
    input.addFact(value);
    DataFlowFacts output =
        DataFlowFacts::Union(apply(input), other.apply(input));
    DataFlowFacts basis = DataFlowFacts::Diff(output, joinedGen);
    const bool preservesValue = basis.containsFact(value);
    if (preservesUnknown) {
      if (!preservesValue) {
        joinedKill.addFact(value);
      }
    } else if (preservesValue) {
      joinedKill.removeFact(value);
    }
    basis.removeFact(value);
    if (!basis.isEmpty()) {
      joinedFlow[value] = basis;
    }
  }
  return normalized(joinedKill, joinedGen, joinedFlow);
}

GenKillValue GenKillValue::diff(const GenKillValue &other) const {
  return equal(other) ? zero() : *this;
}

GenKillValue GenKillValue::quasiOne() const { return one(); }

bool GenKillValue::equal(const GenKillValue &other) const {
  if (kind != Kind::Ordinary || other.kind != Kind::Ordinary) {
    return kind == other.kind;
  }
  return DataFlowFacts::Eq(kill, other.kill) &&
         DataFlowFacts::Eq(gen, other.gen) && flow == other.flow;
}

bool GenKillValue::semanticallyEqual(const GenKillValue &other) const {
  if (kind == Kind::Zero || other.kind == Kind::Zero) {
    return kind == other.kind;
  }
  if (kind == Kind::Bottom || other.kind == Kind::Bottom) {
    return kind == other.kind;
  }
  if (!DataFlowFacts::Eq(apply(DataFlowFacts::EmptySet()),
                         other.apply(DataFlowFacts::EmptySet()))) {
    return false;
  }
  for (Value *fact : collectRelevantValues(*this, other)) {
    DataFlowFacts input;
    input.addFact(fact);
    if (!DataFlowFacts::Eq(apply(input), other.apply(input))) {
      return false;
    }
  }
  return true;
}

DataFlowFacts GenKillValue::apply(const DataFlowFacts &input) const {
  DataFlowFacts survivors = DataFlowFacts::Diff(input, kill);
  DataFlowFacts flowOut;
  for (Value *value : input.getFacts()) {
    auto it = flow.find(value);
    if (it != flow.end()) {
      flowOut = DataFlowFacts::Union(flowOut, it->second);
    }
  }
  return DataFlowFacts::Union(DataFlowFacts::Union(survivors, flowOut), gen);
}

GenKillValue::Kind GenKillValue::getKind() const { return kind; }

const DataFlowFacts &GenKillValue::getKill() const { return kill; }

const DataFlowFacts &GenKillValue::getGen() const { return gen; }

const std::map<Value *, DataFlowFacts> &GenKillValue::getFlow() const {
  return flow;
}

std::ostream &GenKillValue::print(std::ostream &os) const {
  os << "GenKillTransformer{kill=";
  kill.print(os);
  os << ", gen=";
  gen.print(os);
  os << ", flow={";
  bool first = true;
  for (const auto &entry : flow) {
    if (!first) {
      os << ", ";
    }
    first = false;
    if (entry.first->hasName()) {
      os << entry.first->getName().str();
    } else {
      os << entry.first;
    }
    os << "->";
    entry.second.print(os);
  }
  return os << "}}";
}

} // namespace wpds
