#include "Alias/InclusionBased/BootstrapAA/Engine.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace lotus {
namespace bootstrap {

namespace {
Id nextId(std::size_t size, const char *kind) {
  if (size >= INVALID)
    throw std::length_error(std::string("BootstrapAA ") + kind +
                            " ID space exhausted");
  return static_cast<Id>(size);
}
} // namespace

PointsToSet::const_iterator::const_iterator(const PointsToSet *set,
                                            std::size_t word,
                                            std::uint64_t remaining)
    : m_set(set), m_word(word), m_remaining(remaining) {}

Id PointsToSet::const_iterator::operator*() const {
  const auto bit = static_cast<unsigned>(__builtin_ctzll(m_remaining));
  return static_cast<Id>(m_set->m_words[m_word].index * WORD_BITS + bit);
}

PointsToSet::const_iterator &PointsToSet::const_iterator::operator++() {
  m_remaining &= m_remaining - 1;
  if (m_remaining != 0)
    return *this;
  ++m_word;
  if (m_word < m_set->m_words.size())
    m_remaining = m_set->m_words[m_word].bits;
  return *this;
}

bool PointsToSet::const_iterator::operator==(
    const const_iterator &other) const {
  return m_set == other.m_set && m_word == other.m_word &&
         m_remaining == other.m_remaining;
}

PointsToSet::const_iterator PointsToSet::ObjectRange::begin() const {
  return m_set->begin();
}

PointsToSet::const_iterator PointsToSet::ObjectRange::end() const {
  return m_set->end();
}

PointsToSet::const_iterator PointsToSet::begin() const {
  return empty() ? end() : const_iterator(this, 0, m_words.front().bits);
}

PointsToSet::const_iterator PointsToSet::end() const {
  return const_iterator(this, m_words.size(), 0);
}

bool PointsToSet::isTop() const {
  return m_words.size() == 1 && m_words.front().index == 0 &&
         m_words.front().bits == 1;
}

std::size_t PointsToSet::size() const {
  std::size_t result = 0;
  for (const Word &word : m_words)
    result += static_cast<std::size_t>(__builtin_popcountll(word.bits));
  return result;
}

bool PointsToSet::contains(Id object) const {
  if (isTop())
    return true;
  if (object == INVALID)
    return false;
  const Id wordIndex = object / WORD_BITS;
  const auto found = std::lower_bound(
      m_words.begin(), m_words.end(), wordIndex,
      [](const Word &word, Id index) { return word.index < index; });
  return found != m_words.end() && found->index == wordIndex &&
         (found->bits & (std::uint64_t{1} << (object % WORD_BITS))) != 0;
}

bool PointsToSet::insert(Id object) {
  if (object == INVALID)
    throw std::invalid_argument("BootstrapAA: invalid points-to object ID");
  if (isTop() || contains(object))
    return false;
  if (object == UNKNOWN)
    m_words.clear();
  const Id wordIndex = object / WORD_BITS;
  auto found = std::lower_bound(
      m_words.begin(), m_words.end(), wordIndex,
      [](const Word &word, Id index) { return word.index < index; });
  if (found == m_words.end() || found->index != wordIndex)
    found = m_words.insert(found, Word{wordIndex, 0});
  found->bits |= std::uint64_t{1} << (object % WORD_BITS);
  return true;
}

bool PointsToSet::join(const PointsToSet &other) {
  if (isTop() || other.empty())
    return false;
  if (other.isTop()) {
    m_words = {{0, 1}};
    return true;
  }
  std::vector<Word> merged;
  merged.reserve(m_words.size() + other.m_words.size());
  bool changed = false;
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < m_words.size() || right < other.m_words.size()) {
    if (right == other.m_words.size() ||
        (left < m_words.size() &&
         m_words[left].index < other.m_words[right].index)) {
      merged.push_back(m_words[left++]);
    } else if (left == m_words.size() ||
               other.m_words[right].index < m_words[left].index) {
      merged.push_back(other.m_words[right++]);
      changed = true;
    } else {
      const std::uint64_t bits = m_words[left].bits | other.m_words[right].bits;
      changed |= bits != m_words[left].bits;
      merged.push_back({m_words[left].index, bits});
      ++left;
      ++right;
    }
  }
  if (changed)
    m_words = std::move(merged);
  return changed;
}

bool PointsToSet::intersects(const PointsToSet &other) const {
  if (empty() || other.empty())
    return false;
  if (isTop() || other.isTop())
    return true;
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < m_words.size() && right < other.m_words.size()) {
    if (m_words[left].index < other.m_words[right].index) {
      ++left;
    } else if (other.m_words[right].index < m_words[left].index) {
      ++right;
    } else {
      if ((m_words[left].bits & other.m_words[right].bits) != 0)
        return true;
      ++left;
      ++right;
    }
  }
  return false;
}

bool PointsToSet::operator==(const PointsToSet &rhs) const {
  return m_words == rhs.m_words;
}

bool PointsToSet::operator<(const PointsToSet &rhs) const {
  auto left = begin();
  auto right = rhs.begin();
  while (left != end() && right != rhs.end()) {
    if (*left != *right)
      return *left < *right;
    ++left;
    ++right;
  }
  return left == end() && right != rhs.end();
}

bool SteensgaardHierarchy::isHigher(Id higher, Id lower) const {
  if (higher >= successors.size() || lower >= successors.size() ||
      higher == lower)
    return false;
  std::vector<bool> visited(successors.size(), false);
  std::vector<Id> work{higher};
  while (!work.empty()) {
    const Id current = work.back();
    work.pop_back();
    if (!visited[current]) {
      visited[current] = true;
      for (Id successor : successors[current]) {
        if (successor == lower)
          return true;
        work.push_back(successor);
      }
    }
  }
  return false;
}

Program::Program() {
  Object unknown;
  unknown.name = "<unknown>";
  unknown.kind = ObjectKind::Unknown;
  objects.push_back(unknown);
  Object null_object;
  null_object.name = "<null>";
  null_object.kind = ObjectKind::Null;
  objects.push_back(null_object);
}
Id Program::addObject(Object object) {
  const Id id = nextId(objects.size(), "object");
  objects.push_back(std::move(object));
  return id;
}
Id Program::addValue(Value value) {
  const Id id = nextId(values.size(), "value");
  values.push_back(std::move(value));
  return id;
}
Id Program::addConstant(PointsToSet points_to, std::string name) {
  Value value;
  value.name = std::move(name);
  value.constant = true;
  value.initial = std::move(points_to);
  return addValue(std::move(value));
}
Id Program::addFunction(std::string name, bool external) {
  const Id id = nextId(functions.size(), "function");
  Function function;
  function.name = std::move(name);
  function.external = external;
  functions.push_back(std::move(function));
  return id;
}
Id Program::addBlock(Id function) {
  auto &blocks = functions.at(function).blocks;
  const Id id = nextId(blocks.size(), "block");
  blocks.emplace_back();
  return id;
}
Id Program::append(Id function, Id block, Instruction instruction) {
  if (m_next_site == INVALID)
    throw std::length_error("BootstrapAA instruction ID space exhausted");
  instruction.id = m_next_site++;
  functions.at(function).blocks.at(block).instructions.push_back(instruction);
  return instruction.id;
}
void Program::validate() const {
  if (entry >= functions.size() || functions[entry].external)
    throw std::invalid_argument(
        "BootstrapAA requires a defined entry function");
  auto check_value = [&](Id value) {
    if (value != INVALID && value >= values.size())
      throw std::invalid_argument("invalid value ID");
  };
  auto check_set = [&](const PointsToSet &set) {
    for (Id object : set.objects())
      if (object >= objects.size())
        throw std::invalid_argument("invalid object ID in points-to set");
  };
  for (const Value &value : values) {
    check_set(value.initial);
    if (value.function != INVALID && value.function >= functions.size())
      throw std::invalid_argument("invalid value owner");
  }
  for (const Object &object : objects) {
    check_set(object.initial);
    if (object.kind == ObjectKind::Function &&
        object.function >= functions.size())
      throw std::invalid_argument("invalid function object");
    if (object.singleton && object.kind != ObjectKind::Global &&
        object.kind != ObjectKind::Stack)
      throw std::invalid_argument(
          "only scalar globals/stacks may be singleton cells");
  }
  std::set<Id> sites;
  for (const Function &function : functions) {
    if (!function.external && function.blocks.empty())
      throw std::invalid_argument("defined function has no entry block");
    for (Id parameter : function.parameters)
      check_value(parameter);
    for (const Block &block : function.blocks) {
      for (const Edge &edge : block.successors) {
        if (edge.target >= function.blocks.size())
          throw std::invalid_argument("invalid CFG edge");
        for (auto phi : edge.phi) {
          check_value(phi.first);
          check_value(phi.second);
          if (phi.first == INVALID)
            throw std::invalid_argument("PHI has no destination");
        }
      }
      for (const Instruction &instruction : block.instructions) {
        if (instruction.id == INVALID || !sites.insert(instruction.id).second)
          throw std::invalid_argument(
              "instruction IDs must be unique (use append)");
        check_value(instruction.result);
        check_value(instruction.indirect_target);
        for (Id operand : instruction.operands)
          check_value(operand);
        if (instruction.callee != INVALID &&
            instruction.callee >= functions.size())
          throw std::invalid_argument("invalid callee ID");
        if ((instruction.opcode == Opcode::Address ||
             instruction.opcode == Opcode::Allocate) &&
            instruction.object >= objects.size())
          throw std::invalid_argument("invalid allocation object");
        if (((instruction.opcode == Opcode::Copy ||
              instruction.opcode == Opcode::Load) &&
             instruction.operands.size() != 1) ||
            (instruction.opcode == Opcode::Store &&
             instruction.operands.size() != 2) ||
            (instruction.opcode == Opcode::Havoc &&
             instruction.operands.size() > 1) ||
            (instruction.opcode == Opcode::Return &&
             instruction.operands.size() > 1))
          throw std::invalid_argument("incorrect operand count");
        switch (instruction.opcode) {
        case Opcode::Address:
        case Opcode::Allocate:
        case Opcode::Copy:
        case Opcode::Join:
        case Opcode::Load:
        case Opcode::Unknown:
          if (instruction.result == INVALID)
            throw std::invalid_argument("pointer definition has no result");
          break;
        default:
          break;
        }
      }
    }
  }
}

namespace {
using Sets = std::vector<PointsToSet>;
bool isMemory(const Object &object) {
  return object.kind == ObjectKind::Global ||
         object.kind == ObjectKind::Stack || object.kind == ObjectKind::Heap;
}
template <typename Fn> void forInstructions(const Program &program, Fn fn) {
  for (Id f = 0; f < program.functions.size(); ++f)
    for (const Block &block : program.functions[f].blocks)
      for (const Instruction &instruction : block.instructions)
        fn(f, instruction);
}
PointsToSet valueOf(const Program &program, const Sets &values, Id value) {
  if (value == INVALID)
    return PointsToSet::top();
  return program.values[value].constant ? program.values[value].initial
                                        : values[value];
}
struct Targets {
  std::set<Id> functions;
  bool opaque = false;
};
Targets callees(const Program &program, const Instruction &instruction,
                const PointsToSet &target) {
  Targets result;
  result.opaque = instruction.opaque_alternative;
  if (instruction.callee != INVALID) {
    result.functions.insert(instruction.callee);
  } else if (instruction.indirect_target == INVALID || target.isTop()) {
    for (Id f = 0; f < program.functions.size(); ++f)
      result.functions.insert(f);
    result.opaque = true;
  } else {
    for (Id object : target.objects()) {
      if (program.objects[object].kind == ObjectKind::Function)
        result.functions.insert(program.objects[object].function);
      else if (object != NULL_OBJECT)
        result.opaque = true;
    }
  }
  return result;
}

// Unary unification graph. We do not assume the points-to graph is acyclic;
// merge() explicitly handles recursive shapes and self edges.
class Unification {
  struct Node {
    Id parent;
    unsigned rank = 0;
    Id target = INVALID;
    bool top = false;
  };
  std::vector<Node> m_nodes;
  std::size_t m_version = 0;

public:
  Id node(bool top = false) {
    Id id = nextId(m_nodes.size(), "unification node");
    m_nodes.push_back({id, 0, INVALID, top});
    ++m_version;
    return id;
  }
  std::size_t size() const { return m_nodes.size(); }
  Id existingTarget(Id id) {
    id = find(id);
    return m_nodes[id].target == INVALID ? INVALID : find(m_nodes[id].target);
  }
  Id find(Id id) {
    Id root = id;
    while (m_nodes[root].parent != root)
      root = m_nodes[root].parent;
    while (id != root) {
      Id next = m_nodes[id].parent;
      m_nodes[id].parent = root;
      id = next;
    }
    return root;
  }
  bool top(Id id) { return m_nodes[find(id)].top; }
  Id target(Id id) {
    id = find(id);
    if (m_nodes[id].top)
      return id;
    if (m_nodes[id].target == INVALID) {
      Id fresh = node();
      m_nodes[id].target = fresh;
    }
    return find(m_nodes[id].target);
  }
  void merge(Id lhs, Id rhs) {
    std::deque<std::pair<Id, Id>> work{{lhs, rhs}};
    while (!work.empty()) {
      auto pair = work.front();
      work.pop_front();
      Id a = find(pair.first), b = find(pair.second);
      if (a == b)
        continue;
      if (m_nodes[a].rank < m_nodes[b].rank)
        std::swap(a, b);
      Id ta = m_nodes[a].target, tb = m_nodes[b].target;
      m_nodes[b].parent = a;
      if (m_nodes[a].rank == m_nodes[b].rank)
        ++m_nodes[a].rank;
      m_nodes[a].top |= m_nodes[b].top;
      ++m_version;
      if (m_nodes[a].top) {
        m_nodes[a].target = a;
        if (ta != INVALID)
          work.emplace_back(a, ta);
        if (tb != INVALID)
          work.emplace_back(a, tb);
      } else {
        m_nodes[a].target = ta == INVALID ? tb : ta;
        if (ta != INVALID && tb != INVALID)
          work.emplace_back(ta, tb);
      }
    }
  }
  std::size_t version() const { return m_version; }
};
struct CoarseResult {
  Sets upper;
  std::vector<std::vector<Id>> partitions;
  SteensgaardHierarchy hierarchy;
};
CoarseResult steensgaard(const Program &program) {
  Unification graph;
  std::vector<Id> object_nodes, value_nodes;
  for (Id o = 0; o < program.objects.size(); ++o)
    object_nodes.push_back(graph.node(o == UNKNOWN));
  for (std::size_t v = 0; v < program.values.size(); ++v)
    value_nodes.push_back(graph.node());
  Id top = object_nodes[UNKNOWN];
  auto value_target = [&](Id value) {
    return value == INVALID ? top : graph.target(value_nodes[value]);
  };
  auto seed = [&](Id node, const PointsToSet &set) {
    for (Id object : set.objects())
      graph.merge(graph.target(node), object_nodes[object]);
  };
  for (Id o = 0; o < program.objects.size(); ++o)
    seed(object_nodes[o], program.objects[o].initial);
  for (Id v = 0; v < program.values.size(); ++v)
    if (program.values[v].constant)
      seed(value_nodes[v], program.values[v].initial);
  for (Id parameter : program.functions[program.entry].parameters)
    if (parameter != INVALID)
      graph.merge(value_target(parameter), top);
  auto havoc = [&]() {
    for (Id o = 0; o < program.objects.size(); ++o)
      if (isMemory(program.objects[o]))
        graph.merge(graph.target(object_nodes[o]), top);
  };
  auto accessMatches = [&](Id address, std::uint64_t bytes) {
    if (bytes == 0)
      return true;
    address = graph.find(address);
    if (graph.top(address))
      return false;
    bool found = false;
    for (Id object = 2; object < program.objects.size(); ++object) {
      if (!isMemory(program.objects[object]) ||
          graph.find(object_nodes[object]) != address)
        continue;
      found = true;
      if (program.objects[object].bytes == 0 ||
          program.objects[object].bytes != bytes)
        return false;
    }
    return found;
  };
  std::size_t old;
  do {
    old = graph.version();
    forInstructions(program, [&](Id, const Instruction &i) {
      switch (i.opcode) {
      case Opcode::Address:
      case Opcode::Allocate:
        graph.merge(value_target(i.result), object_nodes[i.object]);
        if (i.may_be_null)
          graph.merge(value_target(i.result), object_nodes[NULL_OBJECT]);
        for (Id operand : i.operands)
          graph.merge(value_target(i.result), value_target(operand));
        break;
      case Opcode::Unknown:
        graph.merge(value_target(i.result), top);
        break;
      case Opcode::Copy:
      case Opcode::Join:
        for (Id operand : i.operands)
          graph.merge(value_target(i.result), value_target(operand));
        break;
      case Opcode::Load:
        graph.merge(value_target(i.result),
                    accessMatches(value_target(i.operands[0]), i.read_bytes)
                        ? graph.target(value_target(i.operands[0]))
                        : top);
        break;
      case Opcode::Store: {
        Id address = value_target(i.operands[0]);
        if (graph.top(address)) {
          for (Id o = 0; o < program.objects.size(); ++o)
            if (isMemory(program.objects[o]))
              graph.merge(graph.target(object_nodes[o]),
                          i.write_bytes ? top : value_target(i.operands[1]));
        } else {
          graph.merge(graph.target(address),
                      accessMatches(address, i.write_bytes)
                          ? value_target(i.operands[1])
                          : top);
        }
        break;
      }
      case Opcode::Havoc:
        if (i.result != INVALID)
          graph.merge(value_target(i.result), top);
        if (i.operands.empty() || graph.top(value_target(i.operands[0])))
          havoc();
        else
          graph.merge(graph.target(value_target(i.operands[0])), top);
        break;
      case Opcode::Call: {
        // Coarse call graph: all functions for indirect calls. Refinement below
        // and the context-sensitive solver resolve targets from points-to sets.
        Targets targets = callees(program, i, PointsToSet::top());
        if (targets.opaque) {
          if (i.result != INVALID)
            graph.merge(value_target(i.result), top);
          if (i.writes_memory)
            havoc();
        }
        for (Id f : targets.functions) {
          const Function &callee = program.functions[f];
          if (callee.external) {
            if (i.result != INVALID)
              graph.merge(value_target(i.result), top);
            if (i.writes_memory && callee.writes_memory)
              havoc();
            continue;
          }
          for (std::size_t a = 0; a < callee.parameters.size(); ++a)
            if (callee.parameters[a] != INVALID)
              graph.merge(value_target(callee.parameters[a]),
                          value_target(a < i.operands.size() ? i.operands[a]
                                                             : INVALID));
          if (i.result != INVALID)
            for (const Block &block : callee.blocks)
              for (const Instruction &ret : block.instructions)
                if (ret.opcode == Opcode::Return && !ret.operands.empty())
                  graph.merge(value_target(i.result),
                              value_target(ret.operands[0]));
        }
        break;
      }
      default:
        break;
      }
    });
    for (const Function &function : program.functions)
      for (const Block &block : function.blocks)
        for (const Edge &edge : block.successors)
          for (auto phi : edge.phi)
            graph.merge(value_target(phi.first), value_target(phi.second));
  } while (old != graph.version());

  CoarseResult result;
  // Retain the Steensgaard points-to hierarchy instead of discarding it after
  // partitioning. Unification roots are graph nodes; cycles are collapsed to
  // expose the DAG and depth order required by the paper's Algorithm 1.
  for (Id value = 0; value < program.values.size(); ++value)
    (void)value_target(value);
  std::map<Id, Id> rootNumber;
  for (Id node = 0; node < graph.size(); ++node) {
    const Id root = graph.find(node);
    if (!rootNumber.count(root))
      rootNumber.emplace(root, nextId(rootNumber.size(), "hierarchy node"));
  }
  std::vector<std::vector<Id>> rawSuccessors(rootNumber.size());
  std::vector<std::vector<Id>> rawPredecessors(rootNumber.size());
  for (const auto &[root, from] : rootNumber) {
    const Id target = graph.existingTarget(root);
    if (target == INVALID)
      continue;
    const Id to = rootNumber.at(graph.find(target));
    if (std::find(rawSuccessors[from].begin(), rawSuccessors[from].end(), to) ==
        rawSuccessors[from].end()) {
      rawSuccessors[from].push_back(to);
      rawPredecessors[to].push_back(from);
    }
  }

  std::vector<bool> visited(rawSuccessors.size(), false);
  std::vector<Id> postorder;
  for (Id start = 0; start < rawSuccessors.size(); ++start) {
    if (visited[start])
      continue;
    std::vector<std::pair<Id, std::size_t>> dfs{{start, 0}};
    visited[start] = true;
    while (!dfs.empty()) {
      auto &[node, next] = dfs.back();
      if (next < rawSuccessors[node].size()) {
        const Id successor = rawSuccessors[node][next++];
        if (!visited[successor]) {
          visited[successor] = true;
          dfs.emplace_back(successor, 0);
        }
        continue;
      }
      postorder.push_back(node);
      dfs.pop_back();
    }
  }
  std::vector<Id> component(rawSuccessors.size(), INVALID);
  Id componentCount = 0;
  for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
    if (component[*it] != INVALID)
      continue;
    std::vector<Id> work{*it};
    component[*it] = componentCount;
    while (!work.empty()) {
      const Id node = work.back();
      work.pop_back();
      for (Id predecessor : rawPredecessors[node])
        if (component[predecessor] == INVALID) {
          component[predecessor] = componentCount;
          work.push_back(predecessor);
        }
    }
    ++componentCount;
  }

  auto &hierarchy = result.hierarchy;
  hierarchy.successors.resize(componentCount);
  hierarchy.predecessors.resize(componentCount);
  hierarchy.values.resize(componentCount);
  hierarchy.depth.assign(componentCount, 0);
  hierarchy.cyclic.assign(componentCount, false);
  std::vector<std::size_t> componentSize(componentCount, 0);
  for (Id node = 0; node < rawSuccessors.size(); ++node) {
    const Id from = component[node];
    ++componentSize[from];
    for (Id rawTarget : rawSuccessors[node]) {
      const Id to = component[rawTarget];
      if (from == to) {
        hierarchy.cyclic[from] = true;
      } else if (std::find(hierarchy.successors[from].begin(),
                           hierarchy.successors[from].end(),
                           to) == hierarchy.successors[from].end()) {
        hierarchy.successors[from].push_back(to);
        hierarchy.predecessors[to].push_back(from);
      }
    }
  }
  for (Id node = 0; node < componentCount; ++node)
    hierarchy.cyclic[node] = hierarchy.cyclic[node] || componentSize[node] > 1;

  std::vector<std::size_t> indegree(componentCount, 0);
  std::deque<Id> hierarchyWork;
  for (Id node = 0; node < componentCount; ++node) {
    indegree[node] = hierarchy.predecessors[node].size();
    if (indegree[node] == 0)
      hierarchyWork.push_back(node);
  }
  while (!hierarchyWork.empty()) {
    const Id node = hierarchyWork.front();
    hierarchyWork.pop_front();
    for (Id successor : hierarchy.successors[node]) {
      hierarchy.depth[successor] =
          std::max(hierarchy.depth[successor], hierarchy.depth[node] + 1);
      if (--indegree[successor] == 0)
        hierarchyWork.push_back(successor);
    }
  }

  hierarchy.value_components.resize(program.values.size());
  for (Id value = 0; value < program.values.size(); ++value) {
    const Id raw = rootNumber.at(graph.find(value_target(value)));
    hierarchy.value_components[value] = component[raw];
    hierarchy.values[component[raw]].push_back(value);
  }
  hierarchy.object_components.resize(program.objects.size());
  for (Id object = 0; object < program.objects.size(); ++object) {
    const Id raw = rootNumber.at(graph.find(object_nodes[object]));
    hierarchy.object_components[object] = component[raw];
  }
  hierarchy.top_component =
      component[rootNumber.at(graph.find(object_nodes[UNKNOWN]))];

  result.upper.resize(program.values.size());
  bool universal = false;
  for (Id v = 0; v < program.values.size(); ++v) {
    Id target = graph.find(value_target(v));
    if (graph.top(target)) {
      result.upper[v] = PointsToSet::top();
      universal = true;
    } else {
      for (Id o = 0; o < program.objects.size(); ++o)
        if (hierarchy.value_components[v] == hierarchy.object_components[o])
          result.upper[v].insert(o);
    }
  }
  // Top may alias every pointer; a *disjoint* alias cover must connect all
  // groups.
  if (universal) {
    std::vector<Id> all(program.values.size());
    std::iota(all.begin(), all.end(), 0);
    result.partitions.push_back(std::move(all));
  } else {
    for (const auto &values : hierarchy.values)
      if (!values.empty())
        result.partitions.push_back(values);
  }
  return result;
}

struct Slice {
  std::vector<bool> values, objects;
};
bool touches(const Program &program, const Slice &slice,
             const PointsToSet &targets) {
  for (Id object = 2; object < program.objects.size(); ++object)
    if (slice.objects[object] && targets.contains(object))
      return true;
  return false;
}
Slice relevant(const Program &program, const Sets &upper,
               const SteensgaardHierarchy &hierarchy,
               const std::vector<Id> &seeds, bool enabled) {
  Slice result{std::vector<bool>(program.values.size(), !enabled),
               std::vector<bool>(program.objects.size(), !enabled)};
  if (!enabled)
    return result;
  bool changed = false;
  std::vector<bool> relevantHigher(hierarchy.successors.size(), false);
  auto markHigher = [&](Id value) {
    const Id component = hierarchy.value_components[value];
    std::vector<Id> work(hierarchy.predecessors[component].begin(),
                         hierarchy.predecessors[component].end());
    if (hierarchy.cyclic[component])
      work.push_back(component);
    while (!work.empty()) {
      const Id current = work.back();
      work.pop_back();
      if (relevantHigher[current])
        continue;
      relevantHigher[current] = true;
      work.insert(work.end(), hierarchy.predecessors[current].begin(),
                  hierarchy.predecessors[current].end());
    }
  };
  auto need = [&](Id value) {
    if (value != INVALID && !result.values[value]) {
      result.values[value] = true;
      markHigher(value);
      changed = true;
    }
  };
  auto memory = [&](const PointsToSet &set) {
    for (Id o = 2; o < program.objects.size(); ++o)
      if (isMemory(program.objects[o]) && set.contains(o) &&
          !result.objects[o]) {
        result.objects[o] = true;
        changed = true;
      }
  };
  for (Id value : seeds)
    need(value);
  auto hierarchyAddressRelevant = [&](Id address) {
    return address != INVALID &&
           relevantHigher[hierarchy.value_components[address]];
  };
  do {
    changed = false;
    forInstructions(program, [&](Id, const Instruction &i) {
      bool defines = i.result != INVALID && result.values[i.result];
      if (i.opcode == Opcode::Allocate && defines)
        for (Id operand : i.operands)
          need(operand);
      if ((i.opcode == Opcode::Copy || i.opcode == Opcode::Join) && defines)
        for (Id operand : i.operands)
          need(operand);
      if (i.opcode == Opcode::Load && defines) {
        need(i.operands[0]);
        memory(valueOf(program, upper, i.operands[0]));
      }
      if (i.opcode == Opcode::Store) {
        const Id address = i.operands[0];
        if (hierarchyAddressRelevant(address) ||
            touches(program, result, valueOf(program, upper, address))) {
          need(address);
          need(i.operands[1]);
          memory(valueOf(program, upper, address));
        }
      }
      if (i.opcode == Opcode::Havoc && !i.operands.empty() &&
          (defines || hierarchyAddressRelevant(i.operands[0]) ||
           touches(program, result, valueOf(program, upper, i.operands[0])))) {
        need(i.operands[0]);
        memory(valueOf(program, upper, i.operands[0]));
      }
      if (i.opcode == Opcode::Call) {
        // Keep call/return structure even in empty slices, including call
        // targets.
        need(i.indirect_target);
        Targets targets =
            callees(program, i, valueOf(program, upper, i.indirect_target));
        for (Id f : targets.functions) {
          const Function &callee = program.functions[f];
          for (std::size_t a = 0; a < callee.parameters.size(); ++a)
            if (callee.parameters[a] != INVALID &&
                result.values[callee.parameters[a]])
              need(a < i.operands.size() ? i.operands[a] : INVALID);
          if (defines)
            for (const Block &block : callee.blocks)
              for (const Instruction &ret : block.instructions)
                if (ret.opcode == Opcode::Return && !ret.operands.empty())
                  need(ret.operands[0]);
        }
      }
    });
    for (const Function &function : program.functions)
      for (const Block &block : function.blocks)
        for (const Edge &edge : block.successors)
          for (auto phi : edge.phi)
            if (result.values[phi.first])
              need(phi.second);
  } while (changed);
  return result;
}

Sets inclusion(const Program &program, const Slice &slice) {
  Sets values(program.values.size()), memory(program.objects.size());
  Sets returns(program.functions.size());
  for (Id o = 0; o < program.objects.size(); ++o)
    if (slice.objects[o])
      memory[o] = program.objects[o].initial;
  for (Id parameter : program.functions[program.entry].parameters)
    if (parameter != INVALID && slice.values[parameter])
      values[parameter] = PointsToSet::top();
  bool changed;
  auto eval = [&](Id value) { return valueOf(program, values, value); };
  auto store = [&](const PointsToSet &address, const PointsToSet &rhs,
                   std::uint64_t bytes = 0) {
    for (Id o = 2; o < program.objects.size(); ++o)
      if (slice.objects[o] && isMemory(program.objects[o]) &&
          address.contains(o)) {
        const Object &info = program.objects[o];
        const bool incompatible =
            bytes != 0 && (info.bytes == 0 || info.bytes != bytes);
        changed |= memory[o].join(incompatible ? PointsToSet::top() : rhs);
      }
  };
  do {
    changed = false;
    forInstructions(program, [&](Id f, const Instruction &i) {
      bool defines = i.result != INVALID && slice.values[i.result];
      PointsToSet rhs;
      switch (i.opcode) {
      case Opcode::Address:
      case Opcode::Allocate:
        rhs.insert(i.object);
        if (i.may_be_null)
          rhs.insert(NULL_OBJECT);
        for (Id operand : i.operands)
          rhs.join(eval(operand));
        break;
      case Opcode::Unknown:
        rhs = PointsToSet::top();
        break;
      case Opcode::Copy:
      case Opcode::Join:
        if (defines)
          for (Id operand : i.operands)
            rhs.join(eval(operand));
        break;
      case Opcode::Load:
        if (defines) {
          PointsToSet address = eval(i.operands[0]);
          if (address.isTop())
            rhs = PointsToSet::top();
          else
            for (Id object : address.objects()) {
              const Object &info = program.objects[object];
              if (!isMemory(info) ||
                  (i.read_bytes != 0 &&
                   (info.bytes == 0 || info.bytes != i.read_bytes)))
                rhs.join(PointsToSet::top());
              else
                rhs.join(memory[object]);
            }
        }
        break;
      case Opcode::Store:
        store(eval(i.operands[0]), eval(i.operands[1]), i.write_bytes);
        break;
      case Opcode::Havoc:
        rhs = PointsToSet::top();
        store(i.operands.empty() ? PointsToSet::top() : eval(i.operands[0]),
              PointsToSet::top());
        break;
      case Opcode::Return:
        if (!i.operands.empty())
          changed |= returns[f].join(eval(i.operands[0]));
        break;
      case Opcode::Call: {
        Targets targets = callees(program, i, eval(i.indirect_target));
        if (targets.opaque) {
          rhs = PointsToSet::top();
          if (i.writes_memory)
            store(PointsToSet::top(), PointsToSet::top());
        }
        for (Id callee_id : targets.functions) {
          const Function &callee = program.functions[callee_id];
          if (callee.external) {
            rhs = PointsToSet::top();
            if (i.writes_memory && callee.writes_memory)
              store(PointsToSet::top(), PointsToSet::top());
          } else {
            for (std::size_t a = 0; a < callee.parameters.size(); ++a) {
              Id parameter = callee.parameters[a];
              if (parameter != INVALID && slice.values[parameter])
                changed |= values[parameter].join(
                    eval(a < i.operands.size() ? i.operands[a] : INVALID));
            }
            rhs.join(returns[callee_id]);
          }
        }
        break;
      }
      default:
        break;
      }
      if (defines)
        changed |= values[i.result].join(rhs);
    });
    for (const Function &function : program.functions)
      for (const Block &block : function.blocks)
        for (const Edge &edge : block.successors)
          for (auto phi : edge.phi)
            if (slice.values[phi.first])
              changed |= values[phi.first].join(eval(phi.second));
  } while (changed);
  for (Id v = 0; v < program.values.size(); ++v)
    if (program.values[v].constant)
      values[v] = program.values[v].initial;
  return values;
}

struct State {
  bool reachable = false;
  Sets values, memory;
  State(std::size_t nv, std::size_t no) : values(nv), memory(no) {}
  bool join(const State &other) {
    if (!other.reachable)
      return false;
    bool changed = !reachable;
    reachable = true;
    for (std::size_t i = 0; i < values.size(); ++i)
      changed |= values[i].join(other.values[i]);
    for (std::size_t i = 0; i < memory.size(); ++i)
      changed |= memory[i].join(other.memory[i]);
    return changed;
  }
};
struct Summary {
  bool reachable = false;
  PointsToSet returned;
  Sets memory;
  explicit Summary(std::size_t size) : memory(size) {}
  bool join(const Summary &other) {
    if (!other.reachable)
      return false;
    bool changed = !reachable;
    reachable = true;
    changed |= returned.join(other.returned);
    for (std::size_t i = 0; i < memory.size(); ++i)
      changed |= memory[i].join(other.memory[i]);
    return changed;
  }
};
struct Key {
  Id function;
  Sets parameters, memory;
  bool operator<(const Key &other) const {
    return std::tie(function, parameters, memory) <
           std::tie(other.function, other.parameters, other.memory);
  }
};
struct LimitReached {};
struct Record {
  Key key;
  State entry;
  Summary summary;
  std::set<std::size_t> callers;
  std::map<Id, std::set<std::size_t>> calls;
  std::map<Id, std::pair<State, State>> snapshots;
  Record(Key k, State e)
      : key(std::move(k)), entry(std::move(e)), summary(entry.memory.size()) {}
};

// Input-state tabulation: each distinct (callee, relevant actuals, memory) has
// one summary. Missing summaries are bottom, NOT identity. Dependencies drive
// a least fixed point, including recursive calls without a call-depth cutoff.
class Solver {
  const Program &m_program;
  const Sets &m_upper;
  const Slice m_slice;
  const Options &m_options;
  Statistics &m_stats;
  std::vector<std::unique_ptr<Record>> m_records;
  std::map<Key, std::size_t> m_keys;
  std::deque<std::size_t> m_work;
  std::set<std::size_t> m_queued;
  std::size_t m_steps = 0;
  bool m_failed = false;
  State bottom() const {
    return State(m_program.values.size(), m_program.objects.size());
  }
  void tick() {
    ++m_stats.steps;
    if (++m_steps > m_options.max_steps)
      throw LimitReached{};
  }
  void schedule(std::size_t context) {
    if (m_queued.insert(context).second)
      m_work.push_back(context);
  }
  std::size_t context(Key key) {
    auto it = m_keys.find(key);
    if (it != m_keys.end()) {
      ++m_stats.context_cache_hits;
      return it->second;
    }
    if (m_records.size() >= m_options.max_contexts)
      throw LimitReached{};
    State entry = bottom();
    entry.reachable = true;
    entry.memory = key.memory;
    const auto &parameters = m_program.functions[key.function].parameters;
    for (std::size_t a = 0; a < parameters.size(); ++a)
      if (parameters[a] != INVALID && m_slice.values[parameters[a]])
        entry.values[parameters[a]] = key.parameters[a];
    std::size_t id = m_records.size();
    m_keys.emplace(key, id);
    m_records.emplace_back(
        std::make_unique<Record>(std::move(key), std::move(entry)));
    ++m_stats.contexts;
    ++m_stats.context_cache_misses;
    m_stats.maximum_contexts_per_cluster =
        std::max(m_stats.maximum_contexts_per_cluster, m_records.size());
    schedule(id);
    return id;
  }
  PointsToSet eval(const State &state, Id value) const {
    return valueOf(m_program, state.values, value);
  }
  void write(State &state, const PointsToSet &address, const PointsToSet &rhs,
             std::uint64_t bytes, bool allow_strong) const {
    for (Id object = 2; object < m_program.objects.size(); ++object) {
      const Object &info = m_program.objects[object];
      if (!m_slice.objects[object] || !isMemory(info) ||
          !address.contains(object))
        continue;
      bool strong = allow_strong && !address.isTop() && address.size() == 1 &&
                    info.singleton &&
                    (bytes == 0 || (info.bytes != 0 && bytes == info.bytes));
      // Partial or type-punned writes can synthesize arbitrary pointer bits.
      if (bytes != 0 && (info.bytes == 0 || info.bytes != bytes))
        state.memory[object] = PointsToSet::top();
      else if (strong)
        state.memory[object] = rhs;
      else
        state.memory[object].join(rhs);
    }
  }
  State external(const Instruction &instruction, State state,
                 bool writes) const {
    if (writes && instruction.writes_memory)
      write(state, PointsToSet::top(), PointsToSet::top(), 0, false);
    if (instruction.result != INVALID && m_slice.values[instruction.result])
      state.values[instruction.result] = PointsToSet::top();
    return state;
  }
  State call(std::size_t caller, const Instruction &instruction,
             const State &state) {
    State result = bottom();
    Targets targets = callees(m_program, instruction,
                              eval(state, instruction.indirect_target));
    std::set<std::size_t> edges;
    if (targets.opaque)
      result.join(external(instruction, state, true));
    for (Id f : targets.functions) {
      const Function &function = m_program.functions[f];
      if (function.external) {
        result.join(external(instruction, state, function.writes_memory));
        continue;
      }
      Key key{f, Sets(function.parameters.size()), state.memory};
      for (std::size_t a = 0; a < function.parameters.size(); ++a)
        if (function.parameters[a] != INVALID &&
            m_slice.values[function.parameters[a]])
          key.parameters[a] = eval(state, a < instruction.operands.size()
                                              ? instruction.operands[a]
                                              : INVALID);
      std::size_t callee = context(std::move(key));
      edges.insert(callee);
      m_records[callee]->callers.insert(caller);
      const Summary &summary = m_records[callee]->summary;
      if (!summary.reachable)
        continue;
      State returned = state;
      returned.memory = summary.memory;
      if (instruction.result != INVALID && m_slice.values[instruction.result])
        returned.values[instruction.result] = summary.returned;
      result.join(returned);
    }
    // Replace, rather than union with edges from provisional CFG iterations.
    m_records[caller]->calls[instruction.id] = std::move(edges);
    return result;
  }
  State transfer(std::size_t id, const Instruction &i, State state) {
    tick();
    bool defines = i.result != INVALID && m_slice.values[i.result];
    PointsToSet rhs;
    switch (i.opcode) {
    case Opcode::Address:
      rhs.insert(i.object);
      break;
    case Opcode::Allocate:
      rhs.insert(i.object);
      if (i.may_be_null)
        rhs.insert(NULL_OBJECT);
      for (Id operand : i.operands)
        rhs.join(eval(state, operand));
      if (m_slice.objects[i.object]) {
        if (m_program.objects[i.object].singleton)
          state.memory[i.object] = m_program.objects[i.object].initial;
        else
          state.memory[i.object].join(m_program.objects[i.object].initial);
      }
      break;
    case Opcode::Unknown:
      rhs = PointsToSet::top();
      break;
    case Opcode::Copy:
    case Opcode::Join:
      if (defines)
        for (Id operand : i.operands)
          rhs.join(eval(state, operand));
      break;
    case Opcode::Load:
      if (defines) {
        PointsToSet address = eval(state, i.operands[0]);
        if (address.isTop())
          rhs = PointsToSet::top();
        else
          for (Id object : address.objects()) {
            const Object &info = m_program.objects[object];
            if (!isMemory(info) ||
                (i.read_bytes != 0 &&
                 (info.bytes == 0 || info.bytes != i.read_bytes)))
              rhs.join(PointsToSet::top());
            else
              rhs.join(state.memory[object]);
          }
      }
      break;
    case Opcode::Store:
      if (touches(m_program, m_slice,
                  valueOf(m_program, m_upper, i.operands[0])))
        write(state, eval(state, i.operands[0]), eval(state, i.operands[1]),
              i.write_bytes, true);
      return state;
    case Opcode::Havoc:
      if (defines)
        state.values[i.result] = PointsToSet::top();
      if (i.operands.empty() ||
          touches(m_program, m_slice,
                  valueOf(m_program, m_upper, i.operands[0])))
        write(state,
              i.operands.empty() ? PointsToSet::top()
                                 : eval(state, i.operands[0]),
              PointsToSet::top(), 0, false);
      return state;
    case Opcode::Call:
      return call(id, i, state);
    default:
      return state;
    }
    if (defines)
      state.values[i.result] = std::move(rhs);
    return state;
  }
  Summary evaluate(std::size_t id) {
    tick();
    Record &record = *m_records[id];
    record.snapshots.clear();
    record.calls.clear();
    const Function &function = m_program.functions[record.key.function];
    std::vector<State> inputs(function.blocks.size(), bottom());
    inputs[0] = record.entry;
    std::deque<Id> work{0};
    std::vector<bool> queued(function.blocks.size(), false);
    queued[0] = true;
    Summary output(m_program.objects.size());
    while (!work.empty()) {
      Id b = work.front();
      work.pop_front();
      queued[b] = false;
      State state = inputs[b];
      for (const Instruction &i : function.blocks[b].instructions) {
        if (!state.reachable)
          break;
        State before = state;
        if (i.opcode == Opcode::Return) {
          Summary exit(m_program.objects.size());
          exit.reachable = true;
          exit.memory = state.memory;
          if (!i.operands.empty())
            exit.returned = eval(state, i.operands[0]);
          output.join(exit);
          record.snapshots.insert_or_assign(i.id,
                                            std::make_pair(before, state));
          state.reachable = false;
          break;
        }
        state = transfer(id, i, std::move(state));
        record.snapshots.insert_or_assign(
            i.id, std::make_pair(std::move(before), state));
      }
      if (!state.reachable)
        continue;
      for (const Edge &edge : function.blocks[b].successors) {
        State next = state;
        for (auto phi : edge.phi)
          if (m_slice.values[phi.first])
            next.values[phi.first] = eval(state, phi.second);
        if (inputs[edge.target].join(next) && !queued[edge.target]) {
          queued[edge.target] = true;
          work.push_back(edge.target);
        }
      }
    }
    return output;
  }

public:
  Solver(const Program &program, const Sets &upper, Slice slice,
         const Options &options, Statistics &stats)
      : m_program(program), m_upper(upper), m_slice(std::move(slice)),
        m_options(options), m_stats(stats) {
    m_stats.slice_values +=
        std::count(m_slice.values.begin(), m_slice.values.end(), true);
    m_stats.slice_objects +=
        std::count(m_slice.objects.begin(), m_slice.objects.end(), true);
    try {
      Key root{program.entry,
               Sets(program.functions[program.entry].parameters.size()),
               Sets(program.objects.size())};
      const auto &parameters = program.functions[program.entry].parameters;
      for (std::size_t a = 0; a < parameters.size(); ++a)
        if (parameters[a] != INVALID && m_slice.values[parameters[a]])
          root.parameters[a] = PointsToSet::top();
      // Stack and heap storage is initialized at its allocation instruction.
      for (Id o = 2; o < program.objects.size(); ++o)
        if (m_slice.objects[o] && program.objects[o].kind == ObjectKind::Global)
          root.memory[o] = program.objects[o].initial;
      context(std::move(root));
      while (!m_work.empty()) {
        std::size_t id = m_work.front();
        m_work.pop_front();
        m_queued.erase(id);
        Summary next = evaluate(id);
        if (m_records[id]->summary.join(next)) {
          ++m_stats.summary_updates;
          for (std::size_t caller : m_records[id]->callers)
            schedule(caller);
        }
      }
    } catch (const LimitReached &) {
      m_failed = true;
    }
  }
  QueryResult query(Id value, Id site, const Context *context_path,
                    Point point) const {
    if (m_failed)
      return {PointsToSet::top(), QueryStatus::ResourceLimit};
    std::set<std::size_t> active{0};
    if (context_path) {
      for (Id callsite : *context_path) {
        std::set<std::size_t> next;
        for (std::size_t id : active) {
          auto found = m_records[id]->calls.find(callsite);
          if (found != m_records[id]->calls.end())
            next.insert(found->second.begin(), found->second.end());
        }
        active = std::move(next);
      }
    } else {
      std::deque<std::size_t> work{0};
      while (!work.empty()) {
        std::size_t id = work.front();
        work.pop_front();
        for (const auto &calls : m_records[id]->calls)
          for (std::size_t callee : calls.second)
            if (active.insert(callee).second)
              work.push_back(callee);
      }
    }
    QueryResult result;
    for (std::size_t id : active) {
      auto found = m_records[id]->snapshots.find(site);
      if (found == m_records[id]->snapshots.end())
        continue;
      const State &state =
          point == Point::Before ? found->second.first : found->second.second;
      if (!state.reachable)
        continue;
      result.status = QueryStatus::Complete;
      PointsToSet set = eval(state, value);
      // A query of an undefined/out-of-scope SSA value must never prove
      // NoAlias.
      result.points_to.join(set.empty() ? PointsToSet::top() : set);
    }
    return result;
  }
};
} // namespace

struct Analysis::Impl {
  Program program;
  Options options;
  Statistics stats;
  Sets upper;
  SteensgaardHierarchy hierarchy;
  std::vector<std::vector<Id>> cover;
  std::vector<std::unique_ptr<Solver>> solvers;
  std::map<Id, std::pair<Id, bool>> sites;
  Impl(const Program &p, Options o) : program(p), options(o) {
    const auto preprocessingStart = std::chrono::steady_clock::now();
    program.validate();
    forInstructions(program, [&](Id f, const Instruction &i) {
      sites.emplace(i.id, std::make_pair(f, i.opcode == Opcode::Call));
    });
    CoarseResult coarse = steensgaard(program);
    upper = coarse.upper;
    hierarchy = std::move(coarse.hierarchy);
    stats.steensgaard_partitions = coarse.partitions.size();
    for (const auto &partition : coarse.partitions) {
      stats.steensgaard_partition_sizes.push_back(partition.size());
      stats.steensgaard_largest_partition =
          std::max(stats.steensgaard_largest_partition, partition.size());
    }
    stats.hierarchy_nodes = hierarchy.successors.size();
    for (Id node = 0; node < hierarchy.successors.size(); ++node) {
      stats.hierarchy_edges += hierarchy.successors[node].size();
      stats.hierarchy_max_depth =
          std::max(stats.hierarchy_max_depth, hierarchy.depth[node]);
      stats.hierarchy_cyclic_components += hierarchy.cyclic[node] ? 1 : 0;
    }
    if (!options.enable_clustering) {
      cover.emplace_back(program.values.size());
      std::iota(cover.back().begin(), cover.back().end(), 0);
    } else {
      for (const auto &partition : coarse.partitions) {
        if (partition.size() <= options.andersen_threshold) {
          cover.push_back(partition);
          continue;
        }
        ++stats.andersen_runs;
        Slice slice =
            relevant(program, coarse.upper, hierarchy, partition, true);
        Sets refined = inclusion(program, slice);
        std::map<Id, std::vector<Id>> inverse;
        for (Id v : partition) {
          upper[v] = refined[v];
          if (refined[v].empty()) {
            cover.push_back({v});
          } else if (refined[v].isTop()) {
            // Top appears in *every* inverse points-to set, including UNKNOWN.
            for (Id object = 0; object < program.objects.size(); ++object)
              inverse[object].push_back(v);
          } else {
            for (Id object : refined[v].objects())
              inverse[object].push_back(v);
          }
        }
        for (auto &cluster : inverse)
          if (!cluster.second.empty())
            cover.push_back(std::move(cluster.second));
      }
    }
    // Canonicalize only; never split or truncate overlapping clusters by size.
    std::sort(cover.begin(), cover.end());
    cover.erase(std::unique(cover.begin(), cover.end()), cover.end());
    stats.clusters = cover.size();
    std::vector<std::size_t> memberships(program.values.size(), 0);
    for (const auto &cluster : cover) {
      stats.cluster_sizes.push_back(cluster.size());
      stats.largest_cluster = std::max(stats.largest_cluster, cluster.size());
      stats.cover_memberships += cluster.size();
      for (Id value : cluster)
        ++memberships[value];
    }
    for (std::size_t count : memberships) {
      stats.overlapping_values += count > 1 ? 1 : 0;
      stats.maximum_cover_memberships =
          std::max(stats.maximum_cover_memberships, count);
    }
    solvers.resize(cover.size());
    stats.cluster_solve_milliseconds.assign(cover.size(), 0.0);
    stats.preprocessing_milliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - preprocessingStart)
            .count();
  }
  QueryResult query(Id value, Id site, const Context *context_path,
                    Point point) {
    if (value >= program.values.size() || !sites.count(site))
      throw std::invalid_argument("unknown query value or instruction");
    if (context_path) {
      for (Id callsite : *context_path) {
        auto it = sites.find(callsite);
        if (it == sites.end() || !it->second.second)
          throw std::invalid_argument("context contains a non-call site");
      }
    }
    QueryResult result;
    for (std::size_t c = 0; c < cover.size(); ++c) {
      if (!std::binary_search(cover[c].begin(), cover[c].end(), value))
        continue;
      if (!solvers[c]) {
        ++stats.evaluated_clusters;
        const auto started = std::chrono::steady_clock::now();
        solvers[c] =
            std::make_unique<Solver>(program, upper,
                                     relevant(program, upper, hierarchy,
                                              cover[c], options.enable_slicing),
                                     options, stats);
        stats.cluster_solve_milliseconds[c] =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
      }
      QueryResult part = solvers[c]->query(value, site, context_path, point);
      if (part.status == QueryStatus::ResourceLimit) {
        ++stats.fallback_queries;
        return part;
      }
      if (part.reachable()) {
        result.status = QueryStatus::Complete;
        result.points_to.join(part.points_to);
      }
    }
    return result;
  }
};
Analysis::Analysis(const Program &program, Options options)
    : m_impl(std::make_unique<Impl>(program, options)) {}
Analysis::~Analysis() = default;
Analysis::Analysis(Analysis &&) noexcept = default;
Analysis &Analysis::operator=(Analysis &&) noexcept = default;
QueryResult Analysis::pointsTo(Id value, Id site, const Context &context,
                               Point point) {
  return m_impl->query(value, site, &context, point);
}
QueryResult Analysis::pointsToAllContexts(Id value, Id site, Point point) {
  return m_impl->query(value, site, nullptr, point);
}
bool Analysis::mayAlias(Id lhs, Id rhs, Id site, const Context &context) {
  QueryResult a = pointsTo(lhs, site, context),
              b = pointsTo(rhs, site, context);
  if (!a.reachable() || !b.reachable())
    return true;
  return a.points_to.intersects(b.points_to);
}
const Statistics &Analysis::statistics() const { return m_impl->stats; }
const std::vector<std::vector<Id>> &Analysis::clusters() const {
  return m_impl->cover;
}
const SteensgaardHierarchy &Analysis::hierarchy() const {
  return m_impl->hierarchy;
}

} // namespace bootstrap
} // namespace lotus
