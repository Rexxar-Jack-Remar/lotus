#include "Alias/InclusionBased/BootstrapAA/Engine.h"

#include <random>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

using namespace lotus::bootstrap;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(std::string(__FILE__) + ":" +                   \
                               std::to_string(__LINE__) + ": " #x);            \
  } while (false)

namespace {
PointsToSet pts(std::initializer_list<Id> objects) {
  PointsToSet result;
  for (Id object : objects)
    result.insert(object);
  return result;
}
struct Fixture {
  Program p;
  Id main;
  Fixture() {
    main = p.addFunction("main");
    p.entry = main;
    p.addBlock(main);
  }
  Id value(std::string name = "v", Id function = INVALID) {
    Value v;
    v.name = std::move(name);
    v.function = function == INVALID ? main : function;
    return p.addValue(std::move(v));
  }
  Id object(std::string name, PointsToSet initial = PointsToSet::top(),
            bool singleton = true, ObjectKind kind = ObjectKind::Global) {
    Object o;
    o.name = std::move(name);
    o.initial = initial;
    o.singleton = singleton;
    o.kind = kind;
    return p.addObject(std::move(o));
  }
  Id addr(Id o) { return p.addConstant(PointsToSet(o)); }
  Id inst(Opcode op, Id dst = INVALID, std::vector<Id> args = {},
          Id f = INVALID, Id b = 0) {
    Instruction i;
    i.opcode = op;
    i.result = dst;
    i.operands = std::move(args);
    return p.append(f == INVALID ? main : f, b, i);
  }
  Id call(Id f, Id dst, std::vector<Id> args, Id caller = INVALID, Id b = 0) {
    Instruction i;
    i.opcode = Opcode::Call;
    i.result = dst;
    i.callee = f;
    i.operands = std::move(args);
    return p.append(caller == INVALID ? main : caller, b, i);
  }
  Id functionObject(Id f) {
    Object o;
    o.kind = ObjectKind::Function;
    o.function = f;
    o.name = p.functions[f].name;
    return p.addObject(std::move(o));
  }
};
Options refined() {
  Options o;
  o.andersen_threshold = 1;
  return o;
}
void sparsePointsToSets() {
  PointsToSet set;
  CHECK(set.insert(2));
  CHECK(set.insert(130));
  CHECK(set.insert(4097));
  CHECK(!set.insert(130));
  CHECK(set.size() == 3);
  CHECK(set.contains(2));
  CHECK(set.contains(130));
  CHECK(set.contains(4097));
  CHECK(!set.contains(4096));
  std::vector<Id> objects(set.begin(), set.end());
  CHECK(objects == std::vector<Id>({2, 130, 4097}));

  PointsToSet joined;
  joined.insert(64);
  joined.insert(4097);
  CHECK(set.join(joined));
  CHECK(set == pts({2, 64, 130, 4097}));
  CHECK(set.intersects(pts({64})));
  CHECK(!set.intersects(pts({65})));
  CHECK(set.join(PointsToSet::top()));
  CHECK(set.isTop());
  CHECK(set.objects().begin() != set.objects().end());
  CHECK(*set.objects().begin() == UNKNOWN);
}
void hierarchyMetadata() {
  Fixture f;
  Id leaf = f.object("leaf");
  Id cell = f.object("cell", pts({leaf}));
  Id leafAddress = f.addr(leaf), cellAddress = f.addr(cell);
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  const auto &hierarchy = aa.hierarchy();
  const Id higher = hierarchy.value_components[cellAddress];
  const Id lower = hierarchy.value_components[leafAddress];
  CHECK(hierarchy.isHigher(higher, lower));
  CHECK(hierarchy.depth[lower] > hierarchy.depth[higher]);
  CHECK(aa.statistics().hierarchy_nodes == hierarchy.successors.size());
  CHECK(aa.statistics().hierarchy_edges != 0);
  CHECK(!aa.statistics().steensgaard_partition_sizes.empty());
}
void strongUpdates() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b");
  Id slot = f.object("slot", pts({a}));
  Id ps = f.addr(slot), pb = f.addr(b), x = f.value(), y = f.value();
  Id first = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Store, INVALID, {ps, pb});
  Id second = f.inst(Opcode::Load, y, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({a}));
  CHECK(aa.pointsTo(y, second, {}, Point::After).points_to == pts({b}));
  CHECK(aa.pointsTo(x, second).points_to == pts({a}));
  CHECK(!aa.mayAlias(x, pb, second));
}
void branchesAndKills() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id slot = f.object("slot", pts({NULL_OBJECT}));
  Id ps = f.addr(slot), pa = f.addr(a), pb = f.addr(b), pc = f.addr(c);
  Id left = f.p.addBlock(f.main), right = f.p.addBlock(f.main),
     merge = f.p.addBlock(f.main);
  f.p.functions[f.main].blocks[0].successors = {{left, {}}, {right, {}}};
  f.inst(Opcode::Store, INVALID, {ps, pa}, f.main, left);
  f.inst(Opcode::Store, INVALID, {ps, pb}, f.main, right);
  f.p.functions[f.main].blocks[left].successors = {{merge, {}}};
  f.p.functions[f.main].blocks[right].successors = {{merge, {}}};
  Id x = f.value(), y = f.value();
  Id first = f.inst(Opcode::Load, x, {ps}, f.main, merge);
  f.inst(Opcode::Store, INVALID, {ps, pc}, f.main, merge);
  Id last = f.inst(Opcode::Load, y, {ps}, f.main, merge);
  f.inst(Opcode::Return, INVALID, {}, f.main, merge);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({a, b}));
  CHECK(aa.pointsTo(y, last, {}, Point::After).points_to == pts({c}));
}
void contextsAndReturns() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id identity = f.p.addFunction("identity");
  f.p.addBlock(identity);
  Id parameter = f.value("p", identity);
  f.p.functions[identity].parameters = {parameter};
  Id ret = f.inst(Opcode::Return, INVALID, {parameter}, identity);
  Id x = f.value(), y = f.value();
  Id ca = f.call(identity, x, {pa}), cb = f.call(identity, y, {pb});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a}));
  CHECK(aa.pointsTo(y, end).points_to == pts({b}));
  CHECK(aa.pointsTo(parameter, ret, {ca}).points_to == pts({a}));
  CHECK(aa.pointsTo(parameter, ret, {cb}).points_to == pts({b}));
  CHECK(aa.pointsToAllContexts(parameter, ret).points_to == pts({a, b}));
  CHECK(aa.pointsTo(parameter, ret).status == QueryStatus::Unreachable);
  auto count = aa.statistics().contexts;
  aa.pointsTo(x, end);
  CHECK(count == aa.statistics().contexts);
}
void byReferenceAndMemoryContext() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id setter = f.p.addFunction("setter");
  f.p.addBlock(setter);
  Id dst = f.value("dst", setter), src = f.value("src", setter);
  f.p.functions[setter].parameters = {dst, src};
  f.inst(Opcode::Store, INVALID, {dst, src}, setter);
  f.inst(Opcode::Return, INVALID, {}, setter);
  Id getter = f.p.addFunction("getter");
  f.p.addBlock(getter);
  Id tmp = f.value("tmp", getter);
  f.inst(Opcode::Load, tmp, {ps}, getter);
  f.inst(Opcode::Return, INVALID, {tmp}, getter);
  Id x = f.value(), y = f.value();
  f.call(getter, x, {});
  f.call(setter, INVALID, {ps, pb});
  f.call(getter, y, {});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a}));
  CHECK(aa.pointsTo(y, end).points_to == pts({b}));
  CHECK(!aa.mayAlias(y, pa, end));
}
void conditionalCalleeWrite() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id maybe = f.p.addFunction("maybe");
  f.p.addBlock(maybe);
  Id write = f.p.addBlock(maybe), skip = f.p.addBlock(maybe);
  f.p.functions[maybe].blocks[0].successors = {{write, {}}, {skip, {}}};
  f.inst(Opcode::Store, INVALID, {ps, pb}, maybe, write);
  f.inst(Opcode::Return, INVALID, {}, maybe, write);
  f.inst(Opcode::Return, INVALID, {}, maybe, skip);
  f.call(maybe, INVALID, {});
  Id x = f.value(), load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({a, b}));
}
void recursion() {
  Fixture f;
  Id a = f.object("a"), pa = f.addr(a);
  Id rec = f.p.addFunction("rec");
  f.p.addBlock(rec);
  Id base = f.p.addBlock(rec), step = f.p.addBlock(rec);
  Id p = f.value("p", rec), r = f.value("r", rec);
  f.p.functions[rec].parameters = {p};
  f.p.functions[rec].blocks[0].successors = {{base, {}}, {step, {}}};
  Id ret = f.inst(Opcode::Return, INVALID, {p}, rec, base);
  Id nested = f.call(rec, r, {p}, rec, step);
  f.inst(Opcode::Return, INVALID, {r}, rec, step);
  Id x = f.value(), outer = f.call(rec, x, {pa});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a}));
  Context path(100, nested);
  path.insert(path.begin(), outer);
  CHECK(aa.pointsTo(p, ret, path).points_to == pts({a}));
  CHECK(aa.statistics().contexts < 40);
  CHECK(aa.statistics().recursive_call_graph_sccs != 0);
  CHECK(aa.statistics().scc_reschedules != 0);
}
void mutualRecursion() {
  Fixture f;
  Id a = f.object("a"), pa = f.addr(a);
  Id one = f.p.addFunction("one"), two = f.p.addFunction("two");
  f.p.addBlock(one);
  f.p.addBlock(two);
  Id base = f.p.addBlock(two), step = f.p.addBlock(two);
  f.p.functions[two].blocks[0].successors = {{base, {}}, {step, {}}};
  Id x = f.value("x", one), y = f.value("y", two), z = f.value();
  f.call(two, x, {}, one);
  f.inst(Opcode::Return, INVALID, {x}, one);
  f.inst(Opcode::Return, INVALID, {pa}, two, base);
  f.call(one, y, {}, two, step);
  f.inst(Opcode::Return, INVALID, {y}, two, step);
  f.call(one, z, {});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(z, end).points_to == pts({a}));
}
void noReturnIsNotIdentity() {
  Fixture f;
  Id a = f.object("a"), pa = f.addr(a);
  Id rec = f.p.addFunction("never");
  f.p.addBlock(rec);
  f.call(rec, INVALID, {}, rec);
  f.inst(Opcode::Return, INVALID, {}, rec);
  Id call = f.call(rec, INVALID, {}), end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(pa, call).reachable());
  CHECK(aa.pointsTo(pa, end).status == QueryStatus::Unreachable);
}
void indirectCalls() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id fa = f.p.addFunction("a_fn"), fb = f.p.addFunction("b_fn");
  f.p.addBlock(fa);
  f.p.addBlock(fb);
  f.inst(Opcode::Return, INVALID, {pa}, fa);
  f.inst(Opcode::Return, INVALID, {pb}, fb);
  Id af = f.addr(f.functionObject(fa)), bf = f.addr(f.functionObject(fb));
  Id fp = f.value(), x = f.value(), y = f.value();
  f.inst(Opcode::Join, fp, {af, bf});
  Instruction i;
  i.opcode = Opcode::Call;
  i.indirect_target = fp;
  i.result = x;
  f.p.append(f.main, 0, i);
  i.indirect_target = af;
  i.result = y;
  f.p.append(f.main, 0, i);
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, end).points_to == pts({a, b}));
  CHECK(aa.pointsTo(y, end).points_to == pts({a}));
}
void externalAndUnknownStores() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id ext = f.p.addFunction("external", true);
  Id x = f.value(), y = f.value(), z = f.value();
  f.call(ext, INVALID, {});
  Id l1 = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Store, INVALID, {ps, pb});
  Id l2 = f.inst(Opcode::Load, y, {ps});
  Id unknown = f.p.addConstant(PointsToSet::top());
  f.inst(Opcode::Store, INVALID, {unknown, unknown});
  Id l3 = f.inst(Opcode::Load, z, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, l1, {}, Point::After).points_to.isTop());
  CHECK(aa.pointsTo(y, l2, {}, Point::After).points_to == pts({b}));
  CHECK(aa.pointsTo(z, l3, {}, Point::After).points_to.isTop());
  CHECK(aa.mayAlias(unknown, pb, l3));
}
void readonlyExternal() {
  Fixture f;
  Id a = f.object("a"), slot = f.object("slot", pts({a})), ps = f.addr(slot);
  Id ext = f.p.addFunction("readonly", true);
  f.p.functions[ext].writes_memory = false;
  f.call(ext, INVALID, {});
  Id x = f.value(), load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({a}));
}
void weakSummaryObjects() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id heap = f.object("heap", pts({NULL_OBJECT}), false, ObjectKind::Heap);
  Id ph = f.value(), x = f.value();
  Instruction alloc;
  alloc.opcode = Opcode::Allocate;
  alloc.object = heap;
  alloc.result = ph;
  f.p.append(f.main, 0, alloc);
  f.inst(Opcode::Store, INVALID, {ph, pa});
  f.inst(Opcode::Store, INVALID, {ph, pb});
  Id load = f.inst(Opcode::Load, x, {ph});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to ==
        pts({NULL_OBJECT, a, b}));
}
void multiTargetWeakUpdate() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id s = f.object("s", pts({a})), t = f.object("t", pts({b}));
  Id ps = f.addr(s), pt = f.addr(t), pc = f.addr(c), both = f.value(),
     x = f.value();
  f.inst(Opcode::Join, both, {ps, pt});
  f.inst(Opcode::Store, INVALID, {both, pc});
  Id load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({a, c}));
}
void partialWriteIsWeak() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a}));
  f.p.objects[slot].bytes = 8;
  Id ps = f.addr(slot), x = f.value();
  Instruction store;
  store.opcode = Opcode::Store;
  store.operands = {ps, pb};
  store.write_bytes = 4;
  f.p.append(f.main, 0, store);
  Id load = f.inst(Opcode::Load, x, {ps});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  // Mixed pointer bits are not restricted to either the old or the new object.
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to.isTop());
}
void phiParallelLoop() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b);
  Id p = f.value(), q = f.value();
  Id loop = f.p.addBlock(f.main), end = f.p.addBlock(f.main);
  f.p.functions[f.main].blocks[0].successors = {{loop, {{p, pa}, {q, pb}}}};
  f.p.functions[f.main].blocks[loop].successors = {{loop, {{p, q}, {q, p}}},
                                                   {end, {}}};
  Id site = f.inst(Opcode::Nop, INVALID, {}, f.main, loop);
  f.inst(Opcode::Return, INVALID, {}, f.main, end);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(p, site).points_to == pts({a, b}));
  CHECK(aa.pointsTo(q, site).points_to == pts({a, b}));
}
void cyclicMemory() {
  Fixture f;
  Id holder = f.object("holder", pts({NULL_OBJECT})), ph = f.addr(holder),
     x = f.value();
  f.inst(Opcode::Store, INVALID, {ph, ph});
  Id load = f.inst(Opcode::Load, x, {ph});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, load, {}, Point::After).points_to == pts({holder}));
}
void overlapAndCoverage() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pa = f.addr(a), pb = f.addr(b),
     both = f.value();
  f.inst(Opcode::Join, both, {pa, pb});
  Id end = f.inst(Opcode::Return);
  Options parallel = refined();
  parallel.parallelism = 2;
  Analysis aa(f.p, parallel);
  unsigned occurrences = 0;
  for (const auto &cluster : aa.clusters())
    for (Id value : cluster)
      if (value == both)
        ++occurrences;
  CHECK(occurrences >= 2);
  const auto parallelResult = aa.pointsTo(both, end);
  CHECK(parallelResult.points_to == pts({a, b}));
  CHECK(aa.statistics().parallel_cluster_tasks >= 2);

  Options serial = parallel;
  serial.parallel_clusters = false;
  Analysis reference(f.p, serial);
  CHECK(reference.pointsTo(both, end).points_to == parallelResult.points_to);
}
void adaptiveAndersenThreshold() {
  Fixture f;
  Id first = f.object("first"), second = f.object("second");
  std::vector<Id> firstGroup, secondGroup;
  for (unsigned index = 0; index < 8; ++index)
    firstGroup.push_back(f.addr(first));
  for (unsigned index = 0; index < 5; ++index)
    secondGroup.push_back(f.addr(second));
  Id end = f.inst(Opcode::Return);

  Options adaptive = refined();
  adaptive.adaptive_andersen_threshold = true;
  Analysis adjusted(f.p, adaptive);
  CHECK(adjusted.pointsTo(firstGroup.front(), end).points_to == pts({first}));
  CHECK(adjusted.statistics().andersen_runs != 0);
  CHECK(adjusted.statistics().adaptive_refinement_rejections != 0);
  CHECK(adjusted.statistics().adaptive_refinement_skips != 0);
  CHECK(adjusted.statistics().effective_andersen_threshold >
        adaptive.andersen_threshold);

  Options fixed = adaptive;
  fixed.adaptive_andersen_threshold = false;
  Analysis reference(f.p, fixed);
  CHECK(reference.pointsTo(secondGroup.front(), end).points_to ==
        pts({second}));
  CHECK(reference.statistics().adaptive_refinement_skips == 0);

  Options guarded = adaptive;
  guarded.max_andersen_partition_size = 0;
  guarded.max_andersen_work = 4;
  Analysis costGuarded(f.p, guarded);
  CHECK(costGuarded.pointsTo(firstGroup.front(), end).points_to ==
        pts({first}));
  CHECK(costGuarded.statistics().adaptive_cost_skips == 2);
  CHECK(costGuarded.statistics().andersen_runs == 0);
}
void parallelPrecompute() {
  Fixture f;
  std::vector<std::pair<Id, Id>> pointers;
  for (unsigned index = 0; index < 6; ++index) {
    const Id object = f.object("object");
    pointers.emplace_back(f.addr(object), object);
  }
  const Id end = f.inst(Opcode::Return);

  Options parallel = refined();
  parallel.parallelism = 3;
  Analysis analysis(f.p, parallel);
  analysis.precomputeAll();
  CHECK(analysis.statistics().evaluated_clusters == analysis.clusters().size());
  CHECK(analysis.statistics().parallel_cluster_tasks >= 2);

  Options serial = parallel;
  serial.parallel_clusters = false;
  Analysis reference(f.p, serial);
  reference.precomputeAll();
  for (const auto &[pointer, object] : pointers) {
    CHECK(analysis.pointsTo(pointer, end).points_to == pts({object}));
    CHECK(reference.pointsTo(pointer, end).points_to ==
          analysis.pointsTo(pointer, end).points_to);
  }
}
void limitsAndValidation() {
  Fixture f;
  Id a = f.object("a"), pa = f.addr(a), end = f.inst(Opcode::Return);
  Options zero = refined();
  zero.max_contexts = 0;
  Analysis aa(f.p, zero);
  CHECK(aa.pointsTo(pa, end).status == QueryStatus::ResourceLimit);
  CHECK(aa.pointsTo(pa, end).points_to.isTop());
  zero.max_contexts = 10;
  zero.max_steps = 0;
  Analysis steps(f.p, zero);
  CHECK(steps.pointsTo(pa, end).status == QueryStatus::ResourceLimit);
  bool threw = false;
  try {
    aa.pointsTo(INVALID, end);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    aa.pointsTo(pa, end, {end});
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}
void sizedLoadsAndStores() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), pb = f.addr(b);
  Id slot = f.object("slot", pts({a}));
  f.p.objects[slot].bytes = 8;
  Id ps = f.addr(slot), x = f.value(), y = f.value();
  Instruction store;
  store.opcode = Opcode::Store;
  store.operands = {ps, pb};
  store.write_bytes = 8;
  f.p.append(f.main, 0, store);
  Instruction load;
  load.opcode = Opcode::Load;
  load.operands = {ps};
  load.result = x;
  load.read_bytes = 8;
  Id first = f.p.append(f.main, 0, load);
  load.result = y;
  load.read_bytes = 4;
  Id second = f.p.append(f.main, 0, load);
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({b}));
  CHECK(aa.pointsTo(y, second, {}, Point::After).points_to.isTop());
}
void nullableAllocationAndHavocResult() {
  Fixture f;
  Id heap = f.object("heap", PointsToSet::top(), false, ObjectKind::Heap);
  Id p = f.value(), old = f.value();
  Instruction allocation;
  allocation.opcode = Opcode::Allocate;
  allocation.object = heap;
  allocation.result = p;
  allocation.may_be_null = true;
  Id site = f.p.append(f.main, 0, allocation);
  Id havoc = f.inst(Opcode::Havoc, old, {p});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(p, site, {}, Point::After).points_to ==
        pts({heap, NULL_OBJECT}));
  CHECK(aa.pointsTo(old, havoc, {}, Point::After).points_to.isTop());
}
void implicitByvalAllocation() {
  Fixture f;
  Id cell = f.object("caller-cell", pts({NULL_OBJECT})), pc = f.addr(cell);
  Id copy = f.p.addFunction("copy");
  f.p.addBlock(copy);
  Id formal = f.value("formal", copy);
  f.p.functions[copy].parameters = {formal};
  Id hidden = f.object("byval", PointsToSet::top(), false, ObjectKind::Stack);
  Instruction allocation;
  allocation.opcode = Opcode::Allocate;
  allocation.object = hidden;
  allocation.result = formal;
  f.p.append(copy, 0, allocation);
  f.inst(Opcode::Return, INVALID, {formal}, copy);
  Id r = f.value();
  f.call(copy, r, {pc});
  Id end = f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(r, end).points_to == pts({hidden}));
  CHECK(!aa.mayAlias(r, pc, end));
}
void secondLevelDependencies() {
  Fixture f;
  Id a = f.object("a"), b = f.object("b"), c = f.object("c");
  Id s = f.object("s", pts({a})), t = f.object("t", pts({b}));
  Id link = f.object("link", pts({s}));
  Id ps = f.addr(s), pt = f.addr(t), pl = f.addr(link), pc = f.addr(c);
  Id old_address = f.value(), x = f.value(), y = f.value();
  f.inst(Opcode::Load, old_address, {pl});
  f.inst(Opcode::Store, INVALID, {pl, pt});
  f.inst(Opcode::Store, INVALID, {old_address, pc});
  Id first = f.inst(Opcode::Load, x, {ps}),
     second = f.inst(Opcode::Load, y, {pt});
  f.inst(Opcode::Return);
  Analysis aa(f.p, refined());
  CHECK(aa.pointsTo(x, first, {}, Point::After).points_to == pts({c}));
  CHECK(aa.pointsTo(y, second, {}, Point::After).points_to == pts({b}));
}
void differentialSlices() {
  // Deterministic metamorphic test: bootstrapped/sliced and monolithic solvers
  // must agree on 200 generated straight-line programs with aliasing stores,
  // loads, pointer joins, and context-sensitive helper calls.
  std::mt19937 random(0xB007);
  for (unsigned trial = 0; trial < 200; ++trial) {
    Fixture f;
    std::vector<Id> objects, pointers, slots;
    for (unsigned i = 0; i < 4; ++i)
      objects.push_back(f.object("obj"));
    for (Id object : objects)
      pointers.push_back(f.addr(object));
    for (unsigned i = 0; i < 4; ++i)
      slots.push_back(f.addr(f.object("cell", pts({objects[i]}))));
    Id helper = f.p.addFunction("id");
    f.p.addBlock(helper);
    Id arg = f.value("arg", helper);
    f.p.functions[helper].parameters = {arg};
    f.inst(Opcode::Return, INVALID, {arg}, helper);
    for (unsigned step = 0; step < 16; ++step) {
      switch (random() % 4) {
      case 0:
        f.inst(Opcode::Store, INVALID,
               {slots[random() % slots.size()],
                pointers[random() % pointers.size()]});
        break;
      case 1: {
        Id v = f.value();
        f.inst(Opcode::Load, v, {slots[random() % slots.size()]});
        pointers.push_back(v);
        break;
      }
      case 2: {
        Id v = f.value();
        f.inst(Opcode::Join, v,
               {pointers[random() % pointers.size()],
                pointers[random() % pointers.size()]});
        pointers.push_back(v);
        break;
      }
      default: {
        Id v = f.value();
        f.call(helper, v, {pointers[random() % pointers.size()]});
        pointers.push_back(v);
        break;
      }
      }
    }
    Id end = f.inst(Opcode::Return);
    Options mono;
    mono.enable_clustering = false;
    mono.enable_slicing = false;
    Options sliced = refined();
    if (trial % 2 == 0)
      sliced.andersen_threshold = 60;
    Options serial = sliced;
    serial.parallel_clusters = false;
    Analysis reference(f.p, mono), bootstrapped(f.p, sliced),
        serialized(f.p, serial);
    for (Id pointer : pointers) {
      auto expected = reference.pointsTo(pointer, end),
           actual = bootstrapped.pointsTo(pointer, end),
           serialResult = serialized.pointsTo(pointer, end);
      CHECK(expected.status == actual.status);
      CHECK(expected.points_to == actual.points_to);
      CHECK(serialResult.status == actual.status);
      CHECK(serialResult.points_to == actual.points_to);
    }
  }
}
} // namespace

TEST(BootstrapEngineTest, StrongUpdates) { strongUpdates(); }
TEST(BootstrapEngineTest, SparsePointsToSets) { sparsePointsToSets(); }
TEST(BootstrapEngineTest, HierarchyMetadata) { hierarchyMetadata(); }
TEST(BootstrapEngineTest, BranchesAndKills) { branchesAndKills(); }
TEST(BootstrapEngineTest, ContextsAndReturns) { contextsAndReturns(); }
TEST(BootstrapEngineTest, ByReferenceAndMemoryContext) {
  byReferenceAndMemoryContext();
}
TEST(BootstrapEngineTest, ConditionalCalleeWrite) { conditionalCalleeWrite(); }
TEST(BootstrapEngineTest, Recursion) { recursion(); }
TEST(BootstrapEngineTest, MutualRecursion) { mutualRecursion(); }
TEST(BootstrapEngineTest, NonreturningRecursion) { noReturnIsNotIdentity(); }
TEST(BootstrapEngineTest, IndirectCalls) { indirectCalls(); }
TEST(BootstrapEngineTest, ExternalAndUnknownStores) {
  externalAndUnknownStores();
}
TEST(BootstrapEngineTest, ReadonlyExternal) { readonlyExternal(); }
TEST(BootstrapEngineTest, SummaryHeapObjectsUseWeakUpdates) {
  weakSummaryObjects();
}
TEST(BootstrapEngineTest, MultiTargetStoreUsesWeakUpdate) {
  multiTargetWeakUpdate();
}
TEST(BootstrapEngineTest, PartialWriteIsWeak) { partialWriteIsWeak(); }
TEST(BootstrapEngineTest, ParallelLoopPhis) { phiParallelLoop(); }
TEST(BootstrapEngineTest, CyclicMemory) { cyclicMemory(); }
TEST(BootstrapEngineTest, OverlappingCover) { overlapAndCoverage(); }
TEST(BootstrapEngineTest, AdaptiveAndersenThreshold) {
  adaptiveAndersenThreshold();
}
TEST(BootstrapEngineTest, ParallelPrecompute) { parallelPrecompute(); }
TEST(BootstrapEngineTest, LimitsAndValidation) { limitsAndValidation(); }
TEST(BootstrapEngineTest, SizedLoadsAndStores) { sizedLoadsAndStores(); }
TEST(BootstrapEngineTest, NullableAllocationAndHavocResult) {
  nullableAllocationAndHavocResult();
}
TEST(BootstrapEngineTest, ImplicitByvalAllocation) {
  implicitByvalAllocation();
}
TEST(BootstrapEngineTest, SecondLevelDependencies) {
  secondLevelDependencies();
}
TEST(BootstrapEngineTest, DifferentialSlicing) { differentialSlices(); }
