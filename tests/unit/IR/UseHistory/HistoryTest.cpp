#include "IR/UseHistory/UseHistory.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

using namespace lotus::usehistory;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      throw std::runtime_error(std::string(__FILE__) + ":" +                    \
                               std::to_string(__LINE__) + ": " #condition);     \
  } while (false)

template <typename Exception, typename F> void expectThrow(F fn) {
  bool threw = false;
  try { fn(); } catch (const Exception &) { threw = true; }
  CHECK(threw);
}

std::size_t count(const Graph &g, NodeKind kind, ValueID v = InvalidID) {
  return std::count_if(g.nodes().begin(), g.nodes().end(), [&](const Node &n) {
    return n.kind == kind && (v == InvalidID || n.value == v);
  });
}

void straightLine() {
  Program p;
  auto b = p.addBlock("entry"), v = p.addValue("x");
  p.addOperation(b, "define x", {}, {v});
  auto u1 = p.addOperation(b, "first use", {v});
  auto u2 = p.addOperation(b, "second use", {v});
  auto u3 = p.addOperation(b, "return x", {v});
  auto g = Graph::build(p);
  CHECK(g.verify());
  CHECK(count(g, NodeKind::Phi) == 0);
  CHECK(count(g, NodeKind::Psi) == 3);
  CHECK(g.use(u1, v)->before == g.definition(v));
  CHECK(g.use(u2, v)->before == g.use(u1, v)->after);
  CHECK(g.use(u3, v)->before == g.use(u2, v)->after);
}

void repeatedOperands() {
  Program p;
  auto b = p.addBlock("entry"), v = p.addValue("x");
  auto result = p.addValue("sum");
  p.addOperation(b, "argument", {}, {v});
  auto add = p.addOperation(b, "sum = add x, x", {v, v}, {result});
  auto ret = p.addOperation(b, "return sum", {result});
  auto g = Graph::build(p);
  CHECK(g.usesAt(add).size() == 1);
  CHECK(count(g, NodeKind::Psi, v) == 1);
  CHECK(g.use(ret, result)->before == g.definition(result));
  CHECK(!g.isReachable(g.definition(v), g.definition(result)));
}

void paperExample() {
  Program p;
  auto entry = p.addBlock("entry"), nonnull = p.addBlock("nonnull");
  auto join = p.addBlock("join"), z = p.addValue("z");
  p.addOperation(entry, "z = malloc()", {}, {z});
  auto check = p.addOperation(entry, "z == null", {z});
  p.addEdge(entry, join, "true: null");
  p.addEdge(entry, nonnull, "false: nonnull");
  auto deref = p.addOperation(nonnull, "*z", {z});
  auto free1 = p.addOperation(nonnull, "free(z) #1", {z});
  p.addEdge(nonnull, join);
  auto free2 = p.addOperation(join, "free(z) #2", {z});
  auto g = Graph::build(p);
  CHECK(count(g, NodeKind::Definition) == 1);
  CHECK(count(g, NodeKind::Psi) == 4);
  CHECK(count(g, NodeKind::Phi) == 1);
  CHECK(g.use(deref, z)->before == g.use(check, z)->after);
  CHECK(g.use(free1, z)->before == g.use(deref, z)->after);
  const auto &phi = g.node(g.use(free2, z)->before);
  CHECK(phi.kind == NodeKind::Phi);
  std::set<VersionID> inputs;
  for (auto in : phi.incoming) inputs.insert(in.version);
  CHECK(inputs == std::set<VersionID>({g.use(check, z)->after,
                                     g.use(free1, z)->after}));
  auto path = g.findPath(g.use(free1, z)->after, g.use(free2, z)->after);
  CHECK(path.size() == 3);
  CHECK(path[1] == phi.id);
  // The null-check is NOT automatically a sanitizer. It also reaches null.
  CHECK(g.isReachable(g.use(check, z)->after, g.use(free2, z)->after));
}

void diamond() {
  Program p;
  auto e = p.addBlock("entry"), l = p.addBlock("left");
  auto r = p.addBlock("right"), j = p.addBlock("join");
  auto v = p.addValue("x");
  p.addOperation(e, "def", {}, {v});
  p.addEdge(e, l); p.addEdge(e, r); p.addEdge(l, j); p.addEdge(r, j);
  auto lu = p.addOperation(l, "left use", {v});
  auto ru = p.addOperation(r, "right use", {v});
  auto ju = p.addOperation(j, "join use", {v});
  auto g = Graph::build(p);
  CHECK(g.use(lu, v)->before == g.definition(v));
  CHECK(g.use(ru, v)->before == g.definition(v));
  CHECK(g.node(g.use(ju, v)->before).kind == NodeKind::Phi);
  CHECK(!g.isReachable(g.use(lu, v)->after, g.use(ru, v)->after));
}

void prunedPhi() {
  Program p;
  auto e = p.addBlock("e"), l = p.addBlock("l"), j = p.addBlock("j");
  auto x = p.addValue("x"), y = p.addValue("y");
  p.addOperation(e, "arguments", {}, {x, y});
  p.addEdge(e, l); p.addEdge(e, j); p.addEdge(l, j);
  p.addOperation(l, "dead last use of x", {x});
  auto yUse = p.addOperation(j, "use y", {y});
  auto g = Graph::build(p);
  CHECK(count(g, NodeKind::Phi) == 0);
  CHECK(g.use(yUse, y)->before == g.definition(y));
}

void loop() {
  Program p;
  auto e = p.addBlock("entry"), h = p.addBlock("header");
  auto b = p.addBlock("body"), x = p.addBlock("exit");
  auto v = p.addValue("p");
  p.addOperation(e, "arg", {}, {v});
  p.addEdge(e, h); p.addEdge(h, b); p.addEdge(h, x); p.addEdge(b, h);
  auto bu = p.addOperation(b, "loop use", {v});
  auto xu = p.addOperation(x, "exit use", {v});
  auto g = Graph::build(p);
  CHECK(count(g, NodeKind::Phi) == 1);
  VersionID phi = g.use(bu, v)->before;
  CHECK(g.node(phi).kind == NodeKind::Phi);
  CHECK(g.use(xu, v)->before == phi);
  CHECK(g.isReachable(g.use(bu, v)->after, phi));
  CHECK(g.isReachable(phi, g.use(bu, v)->after));
}

void definitionResetsHistory() {
  Program p;
  auto e = p.addBlock("entry"), b = p.addBlock("loop");
  auto v = p.addValue("fresh allocation");
  p.addEdge(e, b); p.addEdge(b, b);
  p.addOperation(b, "allocate", {}, {v});
  auto u = p.addOperation(b, "free", {v});
  auto g = Graph::build(p);
  CHECK(count(g, NodeKind::Phi) == 0);
  CHECK(g.use(u, v)->before == g.definition(v));
  CHECK(!g.isReachable(g.use(u, v)->after, g.definition(v)));
}

void edgeUses() {
  Program p;
  auto e = p.addBlock("entry"), l = p.addBlock("left"), r = p.addBlock("right");
  auto v = p.addValue("x");
  p.addOperation(e, "arg", {}, {v});
  auto el = p.addEdge(e, l, "left"), er = p.addEdge(e, r, "right");
  auto phiOperand = p.addEdgeOperation(el, "original phi operand", {v});
  auto left = p.addOperation(l, "use x in left", {v});
  auto right = p.addOperation(r, "use x in right", {v});
  auto g = Graph::build(p);
  CHECK(g.use(left, v)->before == g.use(phiOperand, v)->after);
  CHECK(g.use(right, v)->before == g.definition(v));
  CHECK(g.node(g.use(phiOperand, v)->after).region == g.edgeRegion(el));
  CHECK(g.regions()[g.edgeRegion(er)].edge == er);
}

void parallelEdges() {
  Program p;
  auto e = p.addBlock("entry"), j = p.addBlock("join");
  auto v = p.addValue("x");
  p.addOperation(e, "arg", {}, {v});
  auto a = p.addEdge(e, j, "case 0"), b = p.addEdge(e, j, "default");
  auto u = p.addEdgeOperation(a, "edge-only use", {v});
  auto end = p.addOperation(j, "final use", {v});
  auto g = Graph::build(p);
  const Node &phi = g.node(g.use(end, v)->before);
  CHECK(phi.kind == NodeKind::Phi);
  CHECK(phi.incoming.size() == 2);
  CHECK(phi.incoming[0].predecessor == g.edgeRegion(a));
  CHECK(phi.incoming[1].predecessor == g.edgeRegion(b));
  CHECK(phi.incoming[0].version == g.use(u, v)->after);
  CHECK(phi.incoming[1].version == g.definition(v));
}

void invokeDefinition() {
  Program p;
  auto e = p.addBlock("entry"), n = p.addBlock("normal"), u = p.addBlock("unwind");
  auto v = p.addValue("invoke result"), arg = p.addValue("arg");
  p.addOperation(e, "argument", {}, {arg});
  auto call = p.addOperation(e, "invoke", {arg});
  auto en = p.addEdge(e, n, "normal"); p.addEdge(e, u, "unwind");
  p.addEdgeOperation(en, "invoke returns", {}, {v});
  auto phi = p.addEdgeOperation(en, "phi incoming value", {v});
  auto normal = p.addOperation(n, "use returned value", {v});
  auto unwind = p.addOperation(u, "cleanup argument", {arg});
  auto g = Graph::build(p);
  CHECK(g.use(phi, v)->before == g.definition(v));
  CHECK(g.use(normal, v)->before == g.use(phi, v)->after);
  CHECK(g.use(unwind, arg)->before == g.use(call, arg)->after);
  p.addOperation(u, "illegal use of invoke result", {v});
  expectThrow<std::invalid_argument>([&] { Graph::build(p); });
}

void unreachable() {
  Program p;
  auto e = p.addBlock("entry"), dead = p.addBlock("dead"), j = p.addBlock("join");
  auto v = p.addValue("x"), ghost = p.addValue("ghost");
  p.addOperation(e, "arg", {}, {v});
  p.addEdge(e, j); p.addEdge(dead, j); p.addEdge(dead, dead);
  auto du = p.addOperation(dead, "unreachable undefined use", {ghost, v});
  auto ju = p.addOperation(j, "use", {v});
  auto g = Graph::build(p);
  CHECK(!g.regions()[dead].reachable);
  CHECK(!g.use(du, ghost));
  CHECK(g.definition(ghost) == InvalidID);
  CHECK(g.use(ju, v)->before == g.definition(v));
  CHECK(count(g, NodeKind::Phi) == 0);
}

void traps() {
  Program p;
  auto e = p.addBlock("entry"), v = p.addValue("x");
  p.addOperation(e, "arg", {}, {v});
  auto sanitizer = p.addOperation(e, "client-specified sanitizer", {v});
  auto sink = p.addOperation(e, "sink", {v});
  auto g = Graph::build(p);
  auto src = g.definition(v), dst = g.use(sink, v)->after;
  auto trap = g.use(sanitizer, v)->after;
  CHECK(g.findPath(src, dst).size() == 3);
  CHECK(!g.isReachable(src, dst, {trap}));
  CHECK(!g.isReachable(src, dst, {src}));
  CHECK(!g.isReachable(src, dst, {dst}));
  CHECK(g.findPath(src, src) == std::vector<VersionID>{src});
  CHECK(!g.isReachable(src, src, {src}));
  expectThrow<std::out_of_range>([&] { g.findPath(src, InvalidID); });
  expectThrow<std::out_of_range>([&] { g.findPath(src, dst, {InvalidID}); });
}

void bypassTrap() {
  Program p;
  auto e = p.addBlock("entry"), l = p.addBlock("clean"), j = p.addBlock("join");
  auto v = p.addValue("x");
  p.addOperation(e, "arg", {}, {v});
  p.addEdge(e, l); p.addEdge(e, j); p.addEdge(l, j);
  auto clean = p.addOperation(l, "sanitize", {v});
  auto sink = p.addOperation(j, "sink", {v});
  auto g = Graph::build(p);
  CHECK(g.isReachable(g.definition(v), g.use(sink, v)->after,
                       {g.use(clean, v)->after}));
}

void invalidInput() {
  Program p;
  auto e = p.addBlock("entry"), v = p.addValue("x");
  p.addOperation(e, "before definition", {v});
  p.addOperation(e, "definition", {}, {v});
  expectThrow<std::invalid_argument>([&] { Graph::build(p); });
  Program dup;
  auto b = dup.addBlock("entry"), x = dup.addValue("x");
  dup.addOperation(b, "def1", {}, {x});
  dup.addOperation(b, "def2", {}, {x});
  expectThrow<std::invalid_argument>([&] { Graph::build(dup); });
  expectThrow<std::out_of_range>([&] { dup.addEdge(b, InvalidID); });
  expectThrow<std::out_of_range>([&] { dup.addOperation(b, "bad", {InvalidID}); });
  expectThrow<std::invalid_argument>([&] {
    dup.addOperation(b, "duplicate in site", {}, {x, x});
  });
  Program loopEntry;
  b = loopEntry.addBlock("entry"); loopEntry.addEdge(b, b);
  expectThrow<std::invalid_argument>([&] { Graph::build(loopEntry); });
  Program branch;
  auto root = branch.addBlock("root"), left = branch.addBlock("left");
  auto join = branch.addBlock("join"), local = branch.addValue("local");
  branch.addEdge(root, left); branch.addEdge(root, join); branch.addEdge(left, join);
  branch.addOperation(left, "local definition", {}, {local});
  branch.addOperation(join, "not dominated", {local});
  expectThrow<std::invalid_argument>([&] { Graph::build(branch); });
}

void emptyAndUnused() {
  auto empty = Graph::build(Program{});
  CHECK(empty.nodes().empty()); CHECK(empty.verify());
  Program p;
  auto e = p.addBlock("entry"), unused = p.addValue("unused");
  p.addOperation(e, "unused definition", {}, {unused});
  auto g = Graph::build(p);
  CHECK(g.nodes().size() == 1);
  CHECK(g.definition(unused) != InvalidID);
  CHECK(g.use(InvalidID, unused) == nullptr);
}

void deterministicOutput() {
  Program p;
  auto e = p.addBlock("entry \"quoted\""), x = p.addBlock("loop");
  auto v = p.addValue("x\\with\nnewline");
  p.addOperation(e, "arg", {}, {v});
  p.addEdge(e, x, "normal"); p.addEdge(x, x, "back");
  p.addOperation(x, "use\"\\\n", {v});
  auto a = Graph::build(p), b = Graph::build(p);
  std::ostringstream at, bt, ad, bd;
  a.print(at); b.print(bt); a.printDOT(ad); b.printDOT(bd);
  CHECK(at.str() == bt.str()); CHECK(ad.str() == bd.str());
  CHECK(ad.str().find("digraph UseHistory") != std::string::npos);
  CHECK(ad.str().find("\\\"") != std::string::npos);
  CHECK(ad.str().find("\\\\") != std::string::npos);
}

// Independent semantic oracle: bounded words in the graph's history equations
// must equal bounded use traces obtained by directly walking the expanded CFG.
// This checks content of histories, not just node counts or graph structure.
using Word = std::vector<SiteID>;
using Language = std::set<Word>;

void compareHistories(const Graph &g, std::size_t maxLength) {
  std::vector<Language> languages(g.nodes().size());
  for (const Node &n : g.nodes())
    if (n.kind == NodeKind::Definition) languages[n.id].insert(Word{});
  bool changed = true;
  while (changed) {
    changed = false;
    for (const Node &n : g.nodes()) {
      Language additions;
      for (auto in : n.incoming)
        for (Word word : languages[in.version]) {
          if (n.kind == NodeKind::Psi) word.push_back(n.site);
          if (word.size() <= maxLength) additions.insert(std::move(word));
        }
      for (const Word &word : additions)
        changed |= languages[n.id].insert(word).second;
    }
  }
  for (ValueID v = 0; v < g.program().values().size(); ++v) {
    auto def = g.definition(v);
    if (def == InvalidID) continue;
    const Node &d = g.node(def);
    const auto &ops = g.regions()[d.region].operations;
    auto at = std::find(ops.begin(), ops.end(), d.site);
    CHECK(at != ops.end());
    using State = std::tuple<RegionID, std::size_t, Word>;
    std::vector<State> work{{d.region, std::size_t(at - ops.begin()) + 1, {}}};
    std::set<State> seen;
    std::vector<Language> expected(g.program().operations().size());
    for (std::size_t cursor = 0; cursor < work.size(); ++cursor) {
      State state = work[cursor];
      if (!seen.insert(state).second) continue;
      auto region = std::get<0>(state);
      auto pos = std::get<1>(state);
      Word history = std::get<2>(state);
      const Region &r = g.regions()[region];
      if (pos == r.operations.size()) {
        for (RegionID succ : r.successors) work.push_back({succ, 0, history});
        continue;
      }
      const Operation &op = g.program().operations()[r.operations[pos]];
      if (std::find(op.uses.begin(), op.uses.end(), v) != op.uses.end()) {
        expected[op.id].insert(history);
        if (history.size() == maxLength) continue;
        history.push_back(op.id);
      }
      if (std::find(op.definitions.begin(), op.definitions.end(), v) !=
          op.definitions.end())
        continue; // a new dynamic definition resets the history
      work.push_back({region, pos + 1, history});
    }
    for (const Operation &op : g.program().operations())
      if (const UseVersion *use = g.use(op.id, v)) {
        if (languages[use->before] != expected[op.id]) {
          std::ostringstream dump;
          g.print(dump);
          throw std::runtime_error("history mismatch for value " +
                                   std::to_string(v) + " site " +
                                   std::to_string(op.id) + "\n" + dump.str());
        }
      }
  }
}

void irreducible() {
  Program p;
  auto e = p.addBlock("entry"), a = p.addBlock("a"), b = p.addBlock("b");
  auto x = p.addBlock("exit"), v = p.addValue("arg");
  p.addOperation(e, "arg", {}, {v});
  p.addEdge(e, a); p.addEdge(e, b); p.addEdge(a, b); p.addEdge(b, a);
  p.addEdge(a, x); p.addEdge(b, x);
  p.addOperation(a, "use a", {v}); p.addOperation(b, "use b", {v});
  p.addOperation(x, "exit use", {v});
  auto g = Graph::build(p);
  CHECK(g.verify());
  compareHistories(g, 4);
}

void loopPhiOperands() {
  Program p;
  auto e = p.addBlock("entry"), h = p.addBlock("header"), b = p.addBlock("body");
  auto initial = p.addValue("initial"), phi = p.addValue("induction phi");
  auto next = p.addValue("next");
  p.addOperation(e, "arg", {}, {initial});
  auto eh = p.addEdge(e, h); p.addEdge(h, b); auto bh = p.addEdge(b, h);
  p.addEdgeOperation(eh, "incoming initial", {initial});
  p.addOperation(h, "phi result", {}, {phi});
  p.addOperation(h, "condition", {phi});
  p.addOperation(b, "next = phi + 1", {phi}, {next});
  p.addEdgeOperation(bh, "incoming next", {next});
  auto g = Graph::build(p);
  CHECK(g.verify());
  // These are different SSA roots, not one conflated source-language variable.
  CHECK(count(g, NodeKind::Phi) == 0);
  compareHistories(g, 3);
}

void randomizedHistories() {
  std::mt19937 random(0x535355);
  for (unsigned iteration = 0; iteration < 120; ++iteration) {
    Program p;
    unsigned blocks = 3 + random() % 5;
    for (unsigned i = 0; i < blocks; ++i) p.addBlock("b" + std::to_string(i));
    auto a = p.addValue("a"), b = p.addValue("b");
    p.addOperation(0, "arguments", {}, {a, b});
    // Always make at least one path reachable; extra edges can be irreducible,
    // self-loops, critical edges, or parallel edges.
    p.addEdge(0, 1);
    for (unsigned i = 0; i < blocks; ++i) {
      for (unsigned k = 0; k < random() % 3; ++k) {
        auto edge = p.addEdge(i, 1 + random() % (blocks - 1), "random edge");
        if (random() % 3 == 0) p.addEdgeOperation(edge, "edge use", {a});
      }
      unsigned uses = random() % 3;
      for (unsigned k = 0; k < uses; ++k)
        p.addOperation(i, "use", random() % 2 ? std::vector<ValueID>{a, b}
                                               : std::vector<ValueID>{a});
    }
    auto g = Graph::build(p);
    compareHistories(g, 3);
  }
}

void deepCFG() {
  Program p;
  constexpr unsigned blocks = 20000;
  auto v = p.addValue("x");
  for (unsigned i = 0; i < blocks; ++i) p.addBlock("b");
  p.addOperation(0, "arg", {}, {v});
  for (unsigned i = 1; i < blocks; ++i) p.addEdge(i - 1, i);
  auto use = p.addOperation(blocks - 1, "last use", {v});
  auto g = Graph::build(p);
  CHECK(g.use(use, v)->before == g.definition(v));
  CHECK(count(g, NodeKind::Phi) == 0);
  CHECK(g.regions().size() == 2 * blocks - 1);
}

int main(int argc, char **argv) {
  const std::map<std::string, std::function<void()>> tests = {
      {"straight-line", straightLine}, {"repeated-operands", repeatedOperands},
      {"paper-example", paperExample}, {"diamond", diamond},
      {"pruned-phi", prunedPhi}, {"loop", loop},
      {"definition-reset", definitionResetsHistory}, {"edge-uses", edgeUses},
      {"parallel-edges", parallelEdges}, {"invoke-definition", invokeDefinition},
      {"unreachable", unreachable}, {"traps", traps}, {"bypass-trap", bypassTrap},
      {"invalid-input", invalidInput}, {"empty-unused", emptyAndUnused},
      {"deterministic-output", deterministicOutput}, {"irreducible", irreducible},
      {"loop-phi-operands", loopPhiOperands}, {"randomized-histories", randomizedHistories},
      {"deep-cfg", deepCFG}};
  try {
    if (argc == 2) {
      auto it = tests.find(argv[1]);
      if (it == tests.end()) throw std::runtime_error("unknown test");
      it->second();
      std::cout << "PASS " << it->first << '\n';
    } else if (argc == 1) {
      for (const auto &test : tests) {
        test.second();
        std::cout << "PASS " << test.first << '\n';
      }
    } else {
      throw std::runtime_error("usage: lotus-usehistory-core-test [test-name]");
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
