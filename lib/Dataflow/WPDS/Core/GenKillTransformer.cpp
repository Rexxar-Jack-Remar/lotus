#include "Dataflow/WPDS/Core/GenKillTransformer.h"

namespace wpds {

GenKillTransformer::GenKillTransformer() : count(0), value() {}

GenKillTransformer::GenKillTransformer(const DataFlowFacts &kill,
                                       const DataFlowFacts &gen)
    : count(0), value(kill, gen) {}

GenKillTransformer::GenKillTransformer(
    const DataFlowFacts &kill, const DataFlowFacts &gen,
    const std::map<Value *, DataFlowFacts> &flow)
    : count(0), value(kill, gen, flow) {}

GenKillTransformer::GenKillTransformer(const GenKillValue &value)
    : count(0), value(value) {}

GenKillTransformer::GenKillTransformer(const GenKillValue &value, int)
    : count(1), value(value) {}

GenKillTransformer *
GenKillTransformer::makeGenKillTransformer(const DataFlowFacts &kill,
                                           const DataFlowFacts &gen) {
  return makeGenKillTransformer(kill, gen, {});
}

GenKillTransformer *GenKillTransformer::makeGenKillTransformer(
    const DataFlowFacts &kill, const DataFlowFacts &gen,
    const std::map<Value *, DataFlowFacts> &flow) {
  GenKillValue normalized = GenKillValue::normalized(kill, gen, flow);
  switch (normalized.getKind()) {
  case GenKillValue::Kind::One:
    return one();
  case GenKillValue::Kind::Bottom:
    return bottom();
  case GenKillValue::Kind::Zero:
    return zero();
  case GenKillValue::Kind::Ordinary:
    return new GenKillTransformer(normalized);
  }
  return nullptr;
}

GenKillTransformer *GenKillTransformer::one() {
  static GenKillTransformer *ONE =
      new GenKillTransformer(GenKillValue::one(), 1);
  return ONE;
}

GenKillTransformer *GenKillTransformer::zero() {
  static GenKillTransformer *ZERO =
      new GenKillTransformer(GenKillValue::zero(), 1);
  return ZERO;
}

GenKillTransformer *GenKillTransformer::bottom() {
  static GenKillTransformer *BOTTOM =
      new GenKillTransformer(GenKillValue::bottom(), 1);
  return BOTTOM;
}

static GenKillTransformer *materialize(const GenKillValue &result,
                                       GenKillTransformer *lhs,
                                       GenKillTransformer *rhs) {
  if (lhs != nullptr && result.equal(lhs->getValue())) {
    return lhs;
  }
  if (rhs != nullptr && result.equal(rhs->getValue())) {
    return rhs;
  }
  switch (result.getKind()) {
  case GenKillValue::Kind::One:
    return GenKillTransformer::one();
  case GenKillValue::Kind::Zero:
    return GenKillTransformer::zero();
  case GenKillValue::Kind::Bottom:
    return GenKillTransformer::bottom();
  case GenKillValue::Kind::Ordinary:
    return GenKillTransformer::makeGenKillTransformer(
        result.getKill(), result.getGen(), result.getFlow());
  }
  return nullptr;
}

GenKillTransformer *GenKillTransformer::extend(GenKillTransformer *other) {
  return materialize(value.extend(other->value), this, other);
}

GenKillTransformer *GenKillTransformer::combine(GenKillTransformer *other) {
  return materialize(value.combine(other->value), this, other);
}

GenKillTransformer *GenKillTransformer::diff(GenKillTransformer *other) {
  return equal(other) ? zero() : this;
}

GenKillTransformer *GenKillTransformer::quasiOne() const { return one(); }

bool GenKillTransformer::equal(GenKillTransformer *other) const {
  return other != nullptr && value.equal(other->value);
}

DataFlowFacts GenKillTransformer::apply(const DataFlowFacts &input) {
  return value.apply(input);
}

const DataFlowFacts &GenKillTransformer::getKill() const {
  return value.getKill();
}

const DataFlowFacts &GenKillTransformer::getGen() const {
  return value.getGen();
}

const std::map<Value *, DataFlowFacts> &GenKillTransformer::getFlow() const {
  return value.getFlow();
}

const GenKillValue &GenKillTransformer::getValue() const { return value; }

std::ostream &GenKillTransformer::print(std::ostream &os) const {
  return value.print(os);
}

} // namespace wpds
