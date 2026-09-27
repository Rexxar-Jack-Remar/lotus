#include "IR/UseHistory/UseHistory.h"

#include <algorithm>
#include <deque>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lotus {
namespace usehistory {
namespace {

ID nextID(std::size_t size) {
  if (size >= InvalidID)
    throw std::length_error("UseHistory: identifier space exhausted");
  return static_cast<ID>(size);
}

std::vector<ValueID> uniqueValues(const std::vector<ValueID> &values) {
  std::vector<ValueID> result;
  std::unordered_set<ValueID> seen;
  for (ValueID v : values)
    if (seen.insert(v).second)
      result.push_back(v);
  return result;
}

std::string escape(const std::string &text) {
  std::string result;
  for (unsigned char ch : text) {
    switch (ch) {
    case '"': result += "\\\""; break;
    case '\\': result += "\\\\"; break;
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default: result += ch >= 32 ? static_cast<char>(ch) : '?'; break;
    }
  }
  return result;
}

const char *kindName(NodeKind kind) {
  switch (kind) {
  case NodeKind::Definition: return "def";
  case NodeKind::Psi: return "psi";
  case NodeKind::Phi: return "phi";
  }
  return "?";
}

} // namespace

ValueID Program::addValue(std::string name) {
  ValueID id = nextID(Values.size());
  Values.push_back(std::move(name));
  return id;
}

BlockID Program::addBlock(std::string name) {
  BlockID id = nextID(Blocks.size());
  Blocks.push_back({std::move(name), {}});
  if (Entry == InvalidID)
    Entry = id;
  return id;
}

void Program::setEntry(BlockID block) {
  if (block >= Blocks.size())
    throw std::out_of_range("UseHistory: invalid entry block");
  Entry = block;
}

EdgeID Program::addEdge(BlockID from, BlockID to, std::string label) {
  if (from >= Blocks.size() || to >= Blocks.size())
    throw std::out_of_range("UseHistory: invalid CFG edge endpoint");
  EdgeID id = nextID(Edges.size());
  Edges.push_back({from, to, std::move(label), {}});
  return id;
}

SiteID Program::makeOperation(std::string label, std::vector<ValueID> uses,
                             std::vector<ValueID> definitions) {
  for (ValueID v : uses)
    if (v >= Values.size())
      throw std::out_of_range("UseHistory: invalid used value");
  for (ValueID v : definitions)
    if (v >= Values.size())
      throw std::out_of_range("UseHistory: invalid defined value");
  if (uniqueValues(definitions).size() != definitions.size())
    throw std::invalid_argument("UseHistory: duplicate definition in an operation");
  SiteID id = nextID(Operations.size());
  Operations.push_back({id, std::move(label), uniqueValues(uses),
                        std::move(definitions)});
  return id;
}

SiteID Program::addOperation(BlockID block, std::string label,
                            std::vector<ValueID> uses,
                            std::vector<ValueID> definitions) {
  if (block >= Blocks.size())
    throw std::out_of_range("UseHistory: invalid block");
  SiteID id = makeOperation(std::move(label), std::move(uses),
                           std::move(definitions));
  Blocks[block].operations.push_back(id);
  return id;
}

SiteID Program::addEdgeOperation(EdgeID edge, std::string label,
                                std::vector<ValueID> uses,
                                std::vector<ValueID> definitions) {
  if (edge >= Edges.size())
    throw std::out_of_range("UseHistory: invalid edge");
  SiteID id = makeOperation(std::move(label), std::move(uses),
                           std::move(definitions));
  Edges[edge].operations.push_back(id);
  return id;
}

VersionID Graph::addNode(NodeKind kind, ValueID value, RegionID region,
                        SiteID site) {
  VersionID id = nextID(Nodes.size());
  Nodes.push_back({id, kind, value, region, site, {}});
  return id;
}

bool Graph::dominates(RegionID a, RegionID b) const {
  return a < Regions.size() && b < Regions.size() &&
         Regions[a].reachable && Regions[b].reachable &&
         DomIn[a] <= DomIn[b] && DomOut[b] <= DomOut[a];
}

/// Pruned SSA construction applied to the history variable for each original
/// SSA value. A real definition and every psi are definitions of that history
/// variable. PHIs are placed in their live iterated dominance frontier.
class Constructor {
public:
  explicit Constructor(Graph &graph) : G(graph), P(graph.Input) {}
  void run() {
    expandEdges();
    if (G.Regions.empty()) {
      if (!P.values().empty())
        throw std::invalid_argument("UseHistory: values in an empty function");
      return;
    }
    computeDominators();
    collectDefinitionsAndLivenessSeeds();
    checkOriginalSSA();
    placePhis();
    rename();
    G.Users.resize(G.Nodes.size());
    for (const Node &node : G.Nodes)
      for (const Incoming &input : node.incoming)
        G.Users.at(input.version).push_back(node.id);
    // A repeated phi input needs only one reachability adjacency entry.
    for (auto &users : G.Users) {
      std::sort(users.begin(), users.end());
      users.erase(std::unique(users.begin(), users.end()), users.end());
    }
  }

private:
  Graph &G;
  const Program &P;
  std::vector<RegionID> RPO;
  std::vector<ID> RPOIndex;
  std::vector<std::vector<RegionID>> DomChildren, Frontier;
  std::vector<std::set<RegionID>> DefBlocks, LiveSeeds;
  std::vector<std::map<ValueID, VersionID>> Phis;
  std::vector<std::map<ValueID, VersionID>> Psis;
  std::vector<SiteID> OriginalDefs;

  void expandEdges() {
    nextID(P.blocks().size() + P.edges().size());
    G.Regions.resize(P.blocks().size() + P.edges().size());
    G.Uses.resize(P.operations().size());
    G.Definitions.assign(P.values().size(), InvalidID);
    G.SiteRegion.assign(P.operations().size(), InvalidID);
    G.SitePosition.assign(P.operations().size(), InvalidID);
    for (BlockID b = 0; b < P.blocks().size(); ++b) {
      G.Regions[b].block = b;
      G.Regions[b].operations = P.blocks()[b].operations;
    }
    for (EdgeID e = 0; e < P.edges().size(); ++e) {
      const Edge &edge = P.edges()[e];
      RegionID r = static_cast<RegionID>(P.blocks().size()) + e;
      G.Regions[r].edge = e;
      G.Regions[r].operations = edge.operations;
      G.Regions[edge.from].successors.push_back(r);
      G.Regions[r].predecessors.push_back(edge.from);
      G.Regions[r].successors.push_back(edge.to);
      G.Regions[edge.to].predecessors.push_back(r);
    }
    for (RegionID r = 0; r < G.Regions.size(); ++r)
      for (ID pos = 0; pos < G.Regions[r].operations.size(); ++pos) {
        SiteID s = G.Regions[r].operations[pos];
        G.SiteRegion[s] = r;
        G.SitePosition[s] = pos;
      }
    if (!G.Regions.empty() && P.entry() >= P.blocks().size())
      throw std::invalid_argument("UseHistory: missing entry block");
  }

  void computeDominators() {
    // Nonrecursive DFS: very deep generated CFGs must not exhaust the stack.
    std::vector<std::pair<RegionID, std::size_t>> stack;
    RegionID entry = P.entry();
    G.Regions[entry].reachable = true;
    stack.push_back({entry, 0});
    std::vector<RegionID> postorder;
    while (!stack.empty()) {
      auto &frame = stack.back();
      Region &r = G.Regions[frame.first];
      if (frame.second < r.successors.size()) {
        RegionID child = r.successors[frame.second++];
        if (!G.Regions[child].reachable) {
          G.Regions[child].reachable = true;
          stack.push_back({child, 0});
        }
      } else {
        postorder.push_back(frame.first);
        stack.pop_back();
      }
    }
    // Like an LLVM function entry, the external entry is not a loop header.
    for (RegionID pred : G.Regions[entry].predecessors)
      if (G.Regions[pred].reachable)
        throw std::invalid_argument("UseHistory: entry has a reachable predecessor; "
                                    "add a pre-entry block");
    RPO.assign(postorder.rbegin(), postorder.rend());
    RPOIndex.assign(G.Regions.size(), InvalidID);
    for (ID i = 0; i < RPO.size(); ++i)
      RPOIndex[RPO[i]] = i;
    G.IDom.assign(G.Regions.size(), InvalidID);
    G.IDom[entry] = entry;
    auto intersect = [&](RegionID a, RegionID b) {
      while (a != b) {
        while (RPOIndex[a] > RPOIndex[b]) a = G.IDom[a];
        while (RPOIndex[b] > RPOIndex[a]) b = G.IDom[b];
      }
      return a;
    };
    // Cooper-style immediate dominators. No claim of linear worst-case time.
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t i = 1; i < RPO.size(); ++i) {
        RegionID r = RPO[i], newIDom = InvalidID;
        for (RegionID pred : G.Regions[r].predecessors)
          if (G.IDom[pred] != InvalidID)
            newIDom = newIDom == InvalidID ? pred : intersect(pred, newIDom);
        if (G.IDom[r] != newIDom) {
          G.IDom[r] = newIDom;
          changed = true;
        }
      }
    }
    DomChildren.resize(G.Regions.size());
    for (RegionID r : RPO)
      if (r != entry)
        DomChildren[G.IDom[r]].push_back(r);
    G.DomIn.assign(G.Regions.size(), InvalidID);
    G.DomOut.assign(G.Regions.size(), InvalidID);
    ID clock = 0;
    stack.push_back({entry, 0});
    G.DomIn[entry] = clock++;
    while (!stack.empty()) {
      auto &f = stack.back();
      if (f.second < DomChildren[f.first].size()) {
        RegionID child = DomChildren[f.first][f.second++];
        G.DomIn[child] = clock++;
        stack.push_back({child, 0});
      } else {
        // Half-open DFS intervals need only one tick per region.
        G.DomOut[f.first] = clock;
        stack.pop_back();
      }
    }
    Frontier.resize(G.Regions.size());
    for (RegionID r : RPO) {
      std::size_t livePreds = 0;
      for (RegionID pred : G.Regions[r].predecessors)
        livePreds += G.Regions[pred].reachable;
      if (livePreds < 2)
        continue;
      for (RegionID pred : G.Regions[r].predecessors) {
        if (!G.Regions[pred].reachable) continue;
        for (RegionID runner = pred; runner != G.IDom[r];
             runner = G.IDom[runner])
          Frontier[runner].push_back(r);
      }
    }
    for (auto &df : Frontier) {
      std::sort(df.begin(), df.end());
      df.erase(std::unique(df.begin(), df.end()), df.end());
    }
  }

  void collectDefinitionsAndLivenessSeeds() {
    DefBlocks.resize(P.values().size());
    LiveSeeds.resize(P.values().size());
    OriginalDefs.assign(P.values().size(), InvalidID);
    Psis.resize(P.operations().size());
    Phis.resize(G.Regions.size());
    // Region order, not hash iteration order, controls all generated IDs.
    for (RegionID r = 0; r < G.Regions.size(); ++r) {
      if (!G.Regions[r].reachable) continue;
      std::unordered_set<ValueID> defined;
      for (SiteID s : G.Regions[r].operations) {
        const Operation &op = P.operations()[s];
        for (ValueID v : op.uses) {
          if (!defined.count(v)) LiveSeeds[v].insert(r);
          DefBlocks[v].insert(r); // psi defines the next history
          defined.insert(v);
        }
        for (ValueID v : op.definitions) {
          if (OriginalDefs[v] != InvalidID)
            throw std::invalid_argument("UseHistory: multiple SSA definitions of " +
                                        P.values()[v]);
          OriginalDefs[v] = s;
          DefBlocks[v].insert(r);
          defined.insert(v);
        }
      }
    }
  }

  void checkOriginalSSA() {
    for (RegionID r : RPO)
      for (SiteID s : G.Regions[r].operations)
        for (ValueID v : P.operations()[s].uses) {
          SiteID def = OriginalDefs[v];
          if (def == InvalidID || !G.dominates(G.SiteRegion[def], r) ||
              (G.SiteRegion[def] == r &&
               G.SitePosition[def] >= G.SitePosition[s]))
            throw std::invalid_argument("UseHistory: definition does not dominate use "
                                        "of " + P.values()[v] + " at " +
                                        P.operations()[s].label);
        }
    for (ValueID v = 0; v < P.values().size(); ++v) {
      SiteID s = OriginalDefs[v];
      if (s != InvalidID)
        G.Definitions[v] =
            G.addNode(NodeKind::Definition, v, G.SiteRegion[s], s);
    }
  }

  void placePhis() {
    for (ValueID v = 0; v < P.values().size(); ++v) {
      // Sparse backward liveness, killed by original definitions and psis.
      std::unordered_set<RegionID> live(LiveSeeds[v].begin(), LiveSeeds[v].end());
      std::vector<RegionID> work(LiveSeeds[v].begin(), LiveSeeds[v].end());
      for (std::size_t i = 0; i < work.size(); ++i)
        for (RegionID pred : G.Regions[work[i]].predecessors)
          if (G.Regions[pred].reachable && !DefBlocks[v].count(pred) &&
              live.insert(pred).second)
            work.push_back(pred);
      work.assign(DefBlocks[v].begin(), DefBlocks[v].end());
      std::unordered_set<RegionID> queued(work.begin(), work.end());
      for (std::size_t i = 0; i < work.size(); ++i)
        for (RegionID join : Frontier[work[i]]) {
          if (!live.count(join) || Phis[join].count(v)) continue;
          VersionID phi = G.addNode(NodeKind::Phi, v, join, InvalidID);
          Phis[join][v] = phi;
          if (queued.insert(join).second) work.push_back(join);
        }
    }
    // Preallocate psis; renaming fills operands, including backedge operands.
    for (RegionID r = 0; r < G.Regions.size(); ++r)
      if (G.Regions[r].reachable)
        for (SiteID s : G.Regions[r].operations)
          for (ValueID v : P.operations()[s].uses)
            Psis[s][v] = G.addNode(NodeKind::Psi, v, r, s);
  }

  void rename() {
    std::vector<std::vector<VersionID>> versions(P.values().size());
    struct Frame {
      RegionID region;
      std::size_t child = 0;
      bool entered = false;
      std::vector<ValueID> pushed;
    };
    std::vector<Frame> stack;
    stack.push_back({P.entry(), 0, false, {}});
    while (!stack.empty()) {
      Frame &frame = stack.back();
      RegionID r = frame.region;
      if (!frame.entered) {
        frame.entered = true;
        auto push = [&](ValueID v, VersionID n) {
          versions[v].push_back(n);
          frame.pushed.push_back(v);
        };
        for (const auto &phi : Phis[r]) push(phi.first, phi.second);
        for (SiteID s : G.Regions[r].operations) {
          const Operation &op = P.operations()[s];
          for (ValueID v : op.uses) {
            if (versions[v].empty())
              throw std::logic_error("UseHistory: missing history during renaming");
            VersionID before = versions[v].back(), after = Psis[s].at(v);
            G.Nodes[after].incoming.push_back({before, InvalidID});
            G.Uses[s].push_back({s, v, before, after});
            push(v, after);
          }
          for (ValueID v : op.definitions) push(v, G.Definitions[v]);
        }
        for (RegionID succ : G.Regions[r].successors)
          for (const auto &phi : Phis[succ]) {
            ValueID v = phi.first;
            if (versions[v].empty())
              throw std::logic_error("UseHistory: undefined phi incoming history");
            G.Nodes[phi.second].incoming.push_back({versions[v].back(), r});
          }
      }
      if (frame.child < DomChildren[r].size()) {
        RegionID child = DomChildren[r][frame.child++];
        stack.push_back({child, 0, false, {}});
      } else {
        for (auto it = frame.pushed.rbegin(); it != frame.pushed.rend(); ++it)
          versions[*it].pop_back();
        stack.pop_back();
      }
    }
    for (Node &node : G.Nodes)
      if (node.kind == NodeKind::Phi)
        std::sort(node.incoming.begin(), node.incoming.end(),
                  [](const Incoming &a, const Incoming &b) {
                    return a.predecessor < b.predecessor;
                  });
  }
};

Graph Graph::build(Program program) {
  Graph graph;
  graph.Input = std::move(program);
  Constructor(graph).run();
  std::string error;
  if (!graph.verify(&error))
    throw std::logic_error("UseHistory construction invariant: " + error);
  return graph;
}

const UseVersion *Graph::use(SiteID site, ValueID value) const {
  if (site >= Uses.size()) return nullptr;
  for (const UseVersion &u : Uses[site])
    if (u.value == value) return &u;
  return nullptr;
}

RegionID Graph::blockRegion(BlockID block) const {
  if (block >= Input.blocks().size())
    throw std::out_of_range("UseHistory: invalid block");
  return block;
}

RegionID Graph::edgeRegion(EdgeID edge) const {
  if (edge >= Input.edges().size())
    throw std::out_of_range("UseHistory: invalid edge");
  return static_cast<RegionID>(Input.blocks().size()) + edge;
}

std::vector<VersionID>
Graph::findPath(VersionID source, VersionID sink,
                const std::vector<VersionID> &traps) const {
  if (source >= Nodes.size() || sink >= Nodes.size())
    throw std::out_of_range("UseHistory: invalid reachability endpoint");
  std::vector<bool> blocked(Nodes.size(), false);
  for (VersionID t : traps) {
    if (t >= Nodes.size()) throw std::out_of_range("UseHistory: invalid trap");
    blocked[t] = true;
  }
  if (blocked[source] || blocked[sink]) return {};
  std::vector<VersionID> parent(Nodes.size(), InvalidID), queue{source};
  parent[source] = source;
  for (std::size_t i = 0; i < queue.size(); ++i) {
    VersionID current = queue[i];
    if (current == sink) {
      std::vector<VersionID> path{sink};
      while (path.back() != source) path.push_back(parent[path.back()]);
      std::reverse(path.begin(), path.end());
      return path;
    }
    for (VersionID next : Users[current])
      if (!blocked[next] && parent[next] == InvalidID) {
        parent[next] = current;
        queue.push_back(next);
      }
  }
  return {};
}

bool Graph::verify(std::string *error) const {
  if (error) error->clear();
  auto fail = [&](const std::string &message) {
    if (error) *error = message;
    return false;
  };
  if (Nodes.size() != Users.size() || Uses.size() != Input.operations().size() ||
      Definitions.size() != Input.values().size())
    return fail("inconsistent graph tables");
  std::vector<unsigned> psiBindings(Nodes.size(), 0);
  for (VersionID id = 0; id < Nodes.size(); ++id) {
    const Node &n = Nodes[id];
    if (n.id != id || n.value >= Input.values().size() ||
        n.region >= Regions.size() || !Regions[n.region].reachable)
      return fail("invalid node identity or location");
    if (n.kind != NodeKind::Phi &&
        (n.site >= Input.operations().size() || SiteRegion[n.site] != n.region))
      return fail("invalid definition/use site");
    if (n.kind == NodeKind::Definition &&
        (!n.incoming.empty() || Definitions[n.value] != id))
      return fail("definition must be the unique root");
    if (n.kind == NodeKind::Psi && n.incoming.size() != 1)
      return fail("psi must have one input");
    if (n.kind == NodeKind::Phi) {
      if (n.site != InvalidID) return fail("history phi has an original site");
      std::vector<RegionID> expected, actual;
      for (RegionID p : Regions[n.region].predecessors)
        if (Regions[p].reachable) expected.push_back(p);
      for (const Incoming &in : n.incoming) actual.push_back(in.predecessor);
      std::sort(expected.begin(), expected.end());
      std::sort(actual.begin(), actual.end());
      if (expected != actual || expected.size() < 2)
        return fail("phi does not cover the reachable predecessor edges");
    }
    for (const Incoming &in : n.incoming) {
      if (in.version >= Nodes.size() || Nodes[in.version].value != n.value)
        return fail("invalid input or cross-value history edge");
      const Node &src = Nodes[in.version];
      RegionID target = n.kind == NodeKind::Phi ? in.predecessor : n.region;
      if (!dominates(src.region, target))
        return fail("version does not dominate its input use");
      if (n.kind == NodeKind::Psi) {
        if (in.predecessor != InvalidID)
          return fail("psi has an edge predecessor");
        if (src.region == target && src.kind != NodeKind::Phi &&
            SitePosition[src.site] >= SitePosition[n.site])
          return fail("psi reads a future version");
      }
      const auto &users = Users[in.version];
      if (!std::binary_search(users.begin(), users.end(), id))
        return fail("missing reverse adjacency");
    }
    for (VersionID u : Users[id]) {
      if (u >= Nodes.size()) return fail("invalid reverse adjacency");
      const auto &incoming = Nodes[u].incoming;
      if (std::none_of(incoming.begin(), incoming.end(),
                       [&](const Incoming &x) { return x.version == id; }))
        return fail("spurious reverse adjacency");
    }
  }
  for (SiteID s = 0; s < Uses.size(); ++s) {
    if (!Regions[SiteRegion[s]].reachable) {
      if (!Uses[s].empty()) return fail("analyzed an unreachable use");
      continue;
    }
    const auto &expected = Input.operations()[s].uses;
    if (Uses[s].size() != expected.size()) return fail("missing use binding");
    for (ValueID v : expected) {
      const UseVersion *u = use(s, v);
      if (!u || u->site != s || u->before >= Nodes.size() ||
          u->after >= Nodes.size())
        return fail("invalid use binding");
      const Node &psi = Nodes[u->after];
      if (psi.kind != NodeKind::Psi || psi.site != s || psi.value != v ||
          psi.incoming[0].version != u->before)
        return fail("binding does not name its psi");
      ++psiBindings[u->after];
    }
  }
  for (const Node &n : Nodes)
    if (n.kind == NodeKind::Psi && psiBindings[n.id] != 1)
      return fail("psi has no unique use binding");
  return true;
}

void Graph::print(std::ostream &out) const {
  out << "UseHistory: " << Input.values().size() << " values, " << Nodes.size()
      << " versions\n";
  for (RegionID r = 0; r < Regions.size(); ++r) {
    const Region &region = Regions[r];
    out << "region r" << r << " ";
    if (region.block != InvalidID)
      out << "block \"" << escape(Input.blocks()[region.block].name) << "\"";
    else {
      const Edge &e = Input.edges()[region.edge];
      out << "edge e" << region.edge << " (b" << e.from << " -> b" << e.to
          << ") \"" << escape(e.label) << "\"";
    }
    if (!region.reachable) out << " [unreachable]";
    out << '\n';
  }
  for (const Node &n : Nodes) {
    out << "n" << n.id << " [\"" << escape(Input.values()[n.value]) << "\"] = "
        << kindName(n.kind) << "(";
    for (std::size_t i = 0; i < n.incoming.size(); ++i) {
      if (i) out << ", ";
      out << "n" << n.incoming[i].version;
      if (n.kind == NodeKind::Phi)
        out << " @r" << n.incoming[i].predecessor;
    }
    out << ") @r" << n.region;
    if (n.site != InvalidID)
      out << " s" << n.site << " \"" << escape(Input.operations()[n.site].label)
          << "\"";
    out << '\n';
  }
}

void Graph::printDOT(std::ostream &out) const {
  out << "digraph UseHistory {\n  rankdir=LR;\n";
  for (const Node &n : Nodes) {
    std::ostringstream label;
    label << 'n' << n.id << ' ' << Input.values()[n.value] << "\n"
          << kindName(n.kind) << " @r" << n.region;
    if (n.site != InvalidID) label << "\n" << Input.operations()[n.site].label;
    out << "  n" << n.id << " [shape="
        << (n.kind == NodeKind::Phi ? "diamond" : "box") << ",label=\""
        << escape(label.str()) << "\"];\n";
    for (const Incoming &in : n.incoming) {
      out << "  n" << in.version << " -> n" << n.id;
      if (n.kind == NodeKind::Phi) {
        out << " [label=\"r" << in.predecessor;
        const Region &pred = Regions[in.predecessor];
        if (pred.edge != InvalidID)
          out << ' ' << escape(Input.edges()[pred.edge].label);
        out << "\"]";
      }
      out << ";\n";
    }
  }
  out << "}\n";
}

} // namespace usehistory
} // namespace lotus
