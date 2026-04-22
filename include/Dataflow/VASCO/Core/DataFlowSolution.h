#pragma once

#include <map>

namespace vasco {

template <typename N, typename A> class DataFlowSolution {
public:
  using MapType = std::map<N, A>;

  DataFlowSolution(MapType InValues, MapType OutValues)
      : InValues(std::move(InValues)), OutValues(std::move(OutValues)) {}

  const A &getValueBefore(const N &Node) const { return InValues.at(Node); }
  const A &getValueAfter(const N &Node) const { return OutValues.at(Node); }

  const MapType &getInValues() const { return InValues; }
  const MapType &getOutValues() const { return OutValues; }

private:
  MapType InValues;
  MapType OutValues;
};

} // namespace vasco
