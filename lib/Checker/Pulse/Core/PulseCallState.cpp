#include "Checker/Pulse/Core/PulseCallState.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"

namespace pulse {

bool CallState::incorporateNewEqs(const PulseFormula &new_eqs) {
  if (astate_) {
    PulseFormula merged =
        PulseFormula::merge(astate_->getPathFormula(), new_eqs);
    if (!merged.isConsistent()) {
      return false;
    }
    astate_->setPathFormula(std::make_unique<PulseFormula>(std::move(merged)));
    astate_->canonicalize();
  }

  if (!astate_) {
    return true;
  }

  Substitution normalized_subst;
  for (const auto &kv : subst_.getMap()) {
    normalized_subst.add(astate_->getCanonical(kv.first),
                         astate_->getCanonical(kv.second));
  }
  subst_ = std::move(normalized_subst);

  std::map<AbstractValue, std::pair<AbstractValue, LazyHeapPath>>
      normalized_rev_subst;
  for (const auto &kv : rev_subst_) {
    normalized_rev_subst[astate_->getCanonical(kv.first)] =
        std::make_pair(astate_->getCanonical(kv.second.first), kv.second.second);
  }
  rev_subst_ = std::move(normalized_rev_subst);

  std::map<AbstractValue, ValueHistory> normalized_hist_map;
  for (const auto &kv : hist_map_) {
    normalized_hist_map[astate_->getCanonical(kv.first)] = kv.second;
  }
  hist_map_ = std::move(normalized_hist_map);

  std::set<AbstractValue> normalized_visited;
  for (AbstractValue v : visited_) {
    normalized_visited.insert(astate_->getCanonical(v));
  }
  visited_ = std::move(normalized_visited);

  std::map<AbstractValue, std::set<AbstractValue>> normalized_aliases;
  for (const auto &kv : aliases_) {
    AbstractValue canon_key = astate_->getCanonical(kv.first);
    auto &targets = normalized_aliases[canon_key];
    for (AbstractValue alias : kv.second) {
      targets.insert(astate_->getCanonical(alias));
    }
  }
  aliases_ = std::move(normalized_aliases);

  return true;
}

} // namespace pulse
