/// @file PointsToGraph.cpp
/// @brief Points-to graph data structure and core operations
///
/// This file implements `PTGraph` and `PTResult`, the foundational data
/// structures for representing and querying **points-to relationships** in
/// LotusAA.
///
/// **Core Data Structures:**
///
/// 1. **PTResult**: Points-to set for a single pointer value
///    - Direct targets: `{(obj1, offset1), (obj2, offset2), ...}`
///    - Derived targets: `{ptr → offset}` (indirection through another pointer)
///    - Cached and memoized for performance
///
/// 2. **PTResultIterator**: Efficient traversal of points-to sets
///    - Recursively expands derived targets
///    - Handles cycles gracefully
///    - Caches results for repeated queries
///
/// 3. **PTGraph**: Per-function points-to graph
///    - Maps `Value* → PTResult*` (points-to sets)
///    - Owns all `MemObject`s for the function
///    - Provides utilities for memory operations and value tracking
///
/// **Key Operations:**
///
/// - `addPointsTo(ptr, obj, offset)`: Create direct points-to edge
/// - `derivePtsFrom(ptr, other_pts, offset)`: Create derived edge
/// - `loadPtrAt(ptr, inst, result)`: Load values from memory locations
/// - `trackPtrRightValue(val, result)`: Track value through PHI/Select/Load
///
/// **Memory Model Integration:**
/// ```
/// PTGraph
///   ├── pt_results: Value → PTResult
///   ├── mem_objs: MemObject set
///   └── Each MemObject contains ObjectLocators
///       └── Each ObjectLocator tracks stored values
/// ```
///
/// **Optimization Techniques:**
///
/// 1. **Memoization**: PTResults cached by Value*
/// 2. **Iterator Caching**: PTResultIterator results cached in PTResult
/// 3. **Load Categories**: Group equivalent loads for load-load matching
/// 4. **Access Path Depth Limiting**: Prune deep field accesses for scalability
///
/// **Configuration:**
/// - `lotus_restrict_pts_count`: Max points-to set size (default: 100)
/// - `lotus_restrict_obj_ap_depth`: Max access path depth for objects (default:
/// 5)
///
/// **Special Values:**
/// - `NullPTS`: Singleton for null pointer
/// - `DEFAULT_NON_POINTER_TYPE`: Int64 (placeholder for non-pointers)
/// - `DEFAULT_POINTER_TYPE`: Int8* (generic pointer type)
///
/// @see PTResult for points-to set representation
/// @see PTResultIterator for efficient set traversal
/// @see MemObject for memory object abstraction
/// @see ObjectLocator for field-level tracking

#include "Alias/LotusAA/MemoryModel/PointsToGraph.h"

#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Support/Config.h"

#include <set>
#include <tuple>

#include <llvm/Analysis/DominanceFrontier.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;
using namespace std;

static cl::opt<int> lotus_restrict_pts_count(
    "lotus-restrict-pts-count",
    cl::desc("Maximum number of locators a pointer may point to"),
    cl::init(3), cl::Hidden);

static cl::opt<int> lotus_restrict_obj_ap_depth(
    "lotus-restrict-obj-ap-depth",
    cl::desc("Maximum AP-depth of objects considered for callees"), cl::init(5),
    cl::Hidden);

// Static members
Type *PTGraph::DEFAULT_NON_POINTER_TYPE = nullptr;
Type *PTGraph::DEFAULT_POINTER_TYPE = nullptr;
const int PTGraph::VALUE_SEQ_UNDEF = -1;
const int PTGraph::VALUE_SEQ_INFINITE = -2;
const int PTGraph::FUNC_OBJ_UNREACHABLE = -1;

// PTResultIterator
PTResultIterator::PTResultIterator(PTResult *target, PTGraph *parent_graph)
    : parent_graph(parent_graph) {
  set<PTResult *> visited;
  visit(target, 0, parent_graph->getEmptyCond(), visited);

  // Optimize: cache results in target
  if (!target->is_optimized) {
    target->pt_list.clear();
    int count = 0;
    for (auto &item : res) {
      count++;
      if (lotus_restrict_pts_count != -1 && count > lotus_restrict_pts_count)
        break;
      target->pt_list.push_back(PTResult::PtItem(item.second, item.first));
    }
    target->is_optimized = true;
  }
}

void PTResultIterator::visit(PTResult *target, int64_t off, path_cond_t cond,
                             set<PTResult *> &visited) {
  // In fuzzing / partially-built summaries we can see null derived targets.
  // Don't crash in release builds; just treat them as empty.
  if (!target)
    return;

  // Check for cycles - if already visited, skip
  if (visited.count(target))
    return;

  visited.insert(target);

  // Direct targets
  for (PTResult::PtItem &item : target->pt_list) {
    if (!item.locator)
      continue;
    ObjectLocator *locator = item.locator->offsetBy(off);
    if (!locator)
      continue;
    path_cond_t new_cond =
        parent_graph->findOrCreateAndRegion(cond, item.cond);
    auto it = res.find(locator);
    if (it == res.end()) {
      res.insert(make_pair(locator, new_cond));
    } else {
      it->second = parent_graph->findOrCreateOrRegion(it->second, new_cond);
    }
  }

  // Derived targets
  for (PTResult::DerivedPtItem &item : target->derived_list) {
    if (!item.src_pts)
      continue;
    path_cond_t new_cond =
        parent_graph->findOrCreateAndRegion(cond, item.cond);
    visit(item.src_pts, off + item.offset, new_cond, visited);
  }

  // Don't erase from visited - we want to prevent cycles
}

namespace llvm {

raw_ostream &operator<<(raw_ostream &out, PTResultIterator &pt_it) {
  for (auto it = pt_it.begin(); it != pt_it.end(); ++it) {
    out << "  " << *it->first << "\n";
  }
  return out;
}

} // namespace llvm

// PTGraph
PTGraph::PTGraph(Function *F, LotusAA *lotus_aa)
    : analyzed_func(F), lotus_aa(lotus_aa), pt_index(0), obj_index(0),
      load_load_match_performed(false) {

  // Get dominance information
  dom_tree = lotus_aa->getDomTree(F);

  // Create NULL points-to result
  NullPTS = addPointsTo(nullptr, MemObject::NullObj, 0, getEmptyCond());
}

PTGraph::~PTGraph() {
  delete NullPTS;

  for (auto &it : pt_results) {
    if (it.second != NullPTS)
      delete it.second;
  }

  for (auto &obj : mem_objs) {
    delete obj.first;
  }

  for (auto &category : load_category_collection) {
    delete category;
  }
}

PTResult *PTGraph::findPTResult(Value *ptr, bool is_create) {
  auto it = pt_results.find(ptr);
  if (it != pt_results.end())
    return it->second;

  if (is_create) {
    PTResult *pts = new PTResult(ptr);
    pt_results[ptr] = pts;
    return pts;
  }

  return nullptr;
}

MemObject *PTGraph::newObject(Value *alloc_site, MemObject::ObjKind obj_type) {
  MemObject *obj = (obj_type == MemObject::CONCRETE)
                       ? new MemObject(alloc_site, this, obj_type)
                       : new SymbolicMemObject(alloc_site, this);

  if (isa_and_nonnull<GlobalValue>(alloc_site)) {
    global_objects.insert(obj);
  }

  mem_objs[obj] = obj_index++;
  return obj;
}

PTResult *PTGraph::addPointsTo(Value *ptr, MemObject *obj, int64_t offset,
                               path_cond_t cond) {
  // In SSA form each value should be assigned exactly once.  However,
  // inter-procedural summary application (processArg, processGlobal) may be
  // called on the same value from multiple call sites or from both the
  // intra-procedural pass and the summary-application path.  Rather than
  // asserting and crashing, we merge the new target into the existing result
  // so that the analysis remains sound (over-approximate).
  PTResult *pts = findPTResult(ptr, true);
  pts->add_target(cond, obj, offset);
  return pts;
}

PTResult *PTGraph::derivePtsFrom(Value *ptr, PTResult *other_pts,
                                 int64_t offset, path_cond_t cond) {
  // Same rationale as addPointsTo: merge rather than assert on re-processing.
  PTResult *pts = findPTResult(ptr, true);
  pts->add_derived_target(cond, other_pts, offset);
  return pts;
}


PTResult *PTGraph::assignPts(Value *ptr, PTResult *pts) {
  pt_results[ptr] = pts;
  return pts;
}

Type *PTGraph::normalizeType(Type *type) {
  assert(type && "Normalizing NULL type");
  return type; // Use original type
}

void PTGraph::refineResult(mem_value_t &to_refine) {
  map<Value *, map<Instruction *, pair<path_cond_t, float>, llvm_cmp>, llvm_cmp>
      tmp_to_merge_values;
  for (auto &val_struct : to_refine) {
    path_cond_t cond = val_struct.cond;
    Value *val = val_struct.val;
    Instruction *pos = val_struct.pos;
    float confidence = val_struct.confidence;
    if (tmp_to_merge_values.count(val) == 0 ||
        tmp_to_merge_values[val].count(pos) == 0) {
      tmp_to_merge_values[val][pos] = make_pair(cond, confidence);
    } else {
      path_cond_t pre_cond = tmp_to_merge_values[val][pos].first;
      float pre_confidence = tmp_to_merge_values[val][pos].second;
      tmp_to_merge_values[val][pos].first =
          findOrCreateOrRegion(pre_cond, cond);
      tmp_to_merge_values[val][pos].second =
          mem_value_item_t::compute_or_confidence(pre_confidence, confidence);
    }
  }

  to_refine.clear();
  for (auto &val_pair : tmp_to_merge_values) {
    for (auto &pos_cond_pair : val_pair.second) {
      to_refine.push_back(mem_value_item_t(pos_cond_pair.second.first,
                                           pos_cond_pair.first, val_pair.first,
                                           pos_cond_pair.second.second));
    }
  }
}

void PTGraph::trackPtrRightValue(Value *ptr, mem_value_t &res) {
  trackPtrRightValueUnderCondition(ptr, res, getEmptyCond(), 1.0f);
}

void PTGraph::trackPtrRightValueUnderCondition(Value *ptr, mem_value_t &res,
                                               path_cond_t base_cond,
                                               float base_confidence) {
  if ((int)res.size() >= 100)
    return;

  if (Argument *arg = dyn_cast<Argument>(ptr)) {
    res.push_back(mem_value_item_t(base_cond, nullptr, arg, base_confidence));
  } else if (LoadInst *load = dyn_cast<LoadInst>(ptr)) {
    mem_value_t load_result;
    getLoadValues(load->getPointerOperand(), load, load_result);
    for (auto &item : load_result) {
      path_cond_t final_cond =
          findOrCreateAndRegion(base_cond, item.cond);
      float final_confidence = mem_value_item_t::compute_and_confidence(
          base_confidence, item.confidence);
      trackPtrRightValueUnderCondition(item.val, res, final_cond,
                                       final_confidence);
    }
  } else if (PHINode *phi = dyn_cast<PHINode>(ptr)) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
      Value *incoming_val = phi->getIncomingValue(i);
      BasicBlock *incoming_bb = phi->getIncomingBlock(i);
      path_cond_t phi_cond =
          findOrCreateUnitPhiRegion(phi->getParent(), incoming_bb);
      trackPtrRightValueUnderCondition(
          incoming_val, res, findOrCreateAndRegion(base_cond, phi_cond),
          base_confidence);
    }
  } else if (SelectInst *sel = dyn_cast<SelectInst>(ptr)) {
    path_cond_t true_cond = sel->getCondition();
    path_cond_t false_cond = getEmptyCond();
    trackPtrRightValueUnderCondition(sel->getTrueValue(), res,
                                     findOrCreateAndRegion(base_cond,
                                                           true_cond),
                                     base_confidence);
    trackPtrRightValueUnderCondition(sel->getFalseValue(), res,
                                     findOrCreateAndRegion(base_cond,
                                                           false_cond),
                                     base_confidence);
  } else if (CastInst *cast = dyn_cast<CastInst>(ptr)) {
    trackPtrRightValueUnderCondition(cast->getOperand(0), res, base_cond,
                                     base_confidence);
  } else {
    res.push_back(mem_value_item_t(base_cond, dyn_cast<Instruction>(ptr), ptr,
                                   base_confidence));
  }

  refineResult(res);
}

void PTGraph::getLoadValues(Value *ptr, Instruction *from_loc, mem_value_t &res,
                            int64_t offset) {
  loadPtrAt(ptr, from_loc, res, false, offset);
}

void PTGraph::loadPtrAt(Value *ptr, Instruction *from_loc, mem_value_t &result,
                        bool create_symbol, int64_t query_offset) {
  // Use visited set to prevent infinite recursion
  std::set<std::tuple<Value *, Instruction *, int64_t>> visited;
  loadPtrAtImpl(ptr, from_loc, result, create_symbol, query_offset, visited);
}

void PTGraph::loadPtrAtImpl(
    Value *ptr, Instruction *from_loc, mem_value_t &result, bool create_symbol,
    int64_t query_offset,
    std::set<std::tuple<Value *, Instruction *, int64_t>> &visited) {
  // Defensive: callers should pass real IR values/locations, but fuzzing and
  // summary edges can route nullptrs here. Avoid null-deref inside type queries
  // and ObjectLocator::getValues() (which requires a non-null Instruction*).
  if (!ptr)
    return;

  // Cycle detection - prevent infinite recursion
  std::tuple<Value *, Instruction *, int64_t> key(ptr, from_loc, query_offset);
  if (visited.count(key))
    return;
  visited.insert(key);

  PTResult *ptr_pts = findPTResult(ptr);
  if (!ptr_pts) {
    // Return early if no points-to result (instead of asserting)
    return;
  }

  Type *value_type = nullptr;
  if (create_symbol) {
    PointerType *ptr_type = dyn_cast<PointerType>(ptr->getType());
    if (ptr_type) {
      value_type = getPointerElementTypeCompat(ptr_type, &getDL());
      // Adjust type for offset if needed
      // Simplified: just use the element type
    } else {
      value_type = DEFAULT_NON_POINTER_TYPE;
    }
  }

  // Collect all points-to targets
  PTResultIterator iter(ptr_pts, this);
  if (lotus_restrict_pts_count != -1 && iter.size() > lotus_restrict_pts_count)
    return;

  for (auto &point_to_item : iter) {
    ObjectLocator *loc = point_to_item.first;
    int64_t offset = loc->getOffset();
    MemObject *obj = loc->getObj();

    // Skip null and unknown objects
    if (obj->isNull() || obj->isUnknown())
      continue;

    // Adjust offset if query_offset provided
    if (query_offset != 0) {
      loc = obj->findLocator(offset + query_offset, true);
    }

    // Get values from this locator
    mem_value_t tmp_result;
    if (from_loc) {
      loc->getValues(from_loc, tmp_result, value_type,
                     ObjectLocator::FUNC_LEVEL_UNDEFINED, true);
    } else {
      // No program point: fall back to the coarse per-object stored-value cache
      // at this offset. This is conservative and avoids requiring dominance
      // info.
      const int64_t off_key = offset + query_offset;
      auto &stored = obj->getStoredValues();
      auto it = stored.find(off_key);
      if (it != stored.end()) {
        for (Value *v : it->second) {
          tmp_result.push_back(mem_value_item_t(getEmptyCond(), nullptr, v));
        }
      }

      // If nothing known was stored, mirror ObjectLocator::getValues() default.
      if (tmp_result.empty()) {
        tmp_result.push_back(mem_value_item_t(
            getEmptyCond(), nullptr,
            obj->isReallyAllocated() ? LocValue::UNDEF_VALUE
                                     : LocValue::FREE_VARIABLE));
      }
    }

    result.insert(result.end(), tmp_result.begin(), tmp_result.end());

    // Track loaded values for load instructions
    if (from_loc && (isa<LoadInst>(from_loc) || isa<CallBase>(from_loc))) {
      Value *val = from_loc;
      if (isa<LoadInst>(from_loc))
        val = from_loc;
      obj->getLoadedValues()[offset + query_offset].insert(val);
    }
  }
}

bool PTGraph::cacheLoadCategory(LoadInst *load_inst) {
  for (unsigned idx = 0; idx < load_category_collection.size(); idx++) {
    assert(!load_category_collection[idx]->empty());
    LoadInst *rep = *load_category_collection[idx]->begin();
    if (isSameValue(load_inst, rep)) {
      load_category_collection[idx]->insert(load_inst);
      load_category[load_inst] = idx;
      return false;
    }
  }

  // New category
  load_category[load_inst] = load_category_collection.size();
  auto *new_category = new set<LoadInst *, llvm_cmp>;
  new_category->insert(load_inst);
  load_category_collection.push_back(new_category);
  return true;
}

void PTGraph::performLoadLoadMatch() {
  if (load_load_match_performed)
    return;

  for (BasicBlock &B : *analyzed_func) {
    for (Instruction &I : B) {
      if (LoadInst *load = dyn_cast<LoadInst>(&I)) {
        cacheLoadCategory(load);
      }
    }
  }

  load_load_match_performed = true;
}

const set<LoadInst *, llvm_cmp> &
PTGraph::getAllLoadWithSameValue(LoadInst *load_inst) {
  assert(load_category.count(load_inst));
  int idx = load_category[load_inst];
  return *load_category_collection[idx];
}

bool PTGraph::isSameValue(LoadInst *l1, LoadInst *l2) {
  if (!load_category.empty() && load_category.count(l1) &&
      load_category.count(l2)) {
    return load_category[l1] == load_category[l2];
  }
  return isSameValue(l1->getPointerOperand(), l1, l2->getPointerOperand(), l2);
}

bool PTGraph::isSameValue(Value *ptr1, Instruction *pos1, Value *ptr2,
                          Instruction *pos2, int64_t offset1, int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1);
  PTResult *ptr2_pts = findPTResult(ptr2);

  if (!ptr1_pts || !ptr2_pts)
    return false;

  PTResultIterator iter1(ptr1_pts, this);
  PTResultIterator iter2(ptr2_pts, this);

  if (iter1.size() != iter2.size() || iter1.size() == 0)
    return false;

  // Check if all locations match
  set<ObjectLocator *, obj_loc_cmp> locs1, locs2;
  for (auto &item : iter1)
    locs1.insert(item.first->offsetBy(offset1));
  for (auto &item : iter2)
    locs2.insert(item.first->offsetBy(offset2));

  if (locs1 != locs2)
    return false;

  // Check if versions match
  for (auto *loc : locs1) {
    MemObject *obj = loc->getObj();
    if (obj->isNull() || obj->isUnknown())
      continue;

    if (loc->getVersion(pos1) != loc->getVersion(pos2))
      return false;
  }

  return true;
}

void PTGraph::dumpMemObjs() {
  for (auto &pair : mem_objs) {
    outs() << "ID:" << pair.second << "\n";
    pair.first->dump();
  }
}

int PTGraph::getObjectToCallApDepth(MemObject *obj, CallInst *call) {
  if (!obj || !call)
    return FUNC_OBJ_UNREACHABLE;

  // Get or compute frontier
  set<MemObject *, mem_obj_cmp> &frontier = object_call_ap_depth_frontier[call];
  map<MemObject *, int, mem_obj_cmp> &cache =
      object_call_arg_ap_depth_cache[call];

  if (frontier.empty()) {
    // Initialize: add global objects
    for (MemObject *global_obj : global_objects) {
      frontier.insert(global_obj);
      cache[global_obj] = 1;
    }

    // Add objects reachable from call arguments
    for (unsigned i = 0; i < call->arg_size(); i++) {
      Value *arg = call->getArgOperand(i);
      PTResult *pts_result = findPTResult(arg, false);
      if (pts_result) {
        PTResultIterator result_iter(pts_result, this);
        for (auto &pt_item : result_iter) {
          MemObject *pt_obj = pt_item.first->getObj();
          if (!cache.count(pt_obj)) {
            cache[pt_obj] = 1;
            frontier.insert(pt_obj);
          }
        }
      }
    }
  }

  // Check cache
  auto cache_find = cache.find(obj);
  if (cache_find != cache.end())
    return cache_find->second;

  // Not in cache - compute on demand
  if (frontier.empty()) {
    cache[obj] = FUNC_OBJ_UNREACHABLE;
    return FUNC_OBJ_UNREACHABLE;
  }

  // Get frontier depth
  MemObject *frontier_sample = *frontier.begin();
  int frontier_depth = cache[frontier_sample];

  // Expand frontier until target found or max depth reached
  set<MemObject *, mem_obj_cmp> new_frontier;
  while (frontier_depth < lotus_restrict_obj_ap_depth) {
    for (MemObject *frontier_obj : frontier) {
      map<int64_t, Type *> &updated_offsets = frontier_obj->getUpdatedOffset();

      for (auto &offset_pair : updated_offsets) {
        int64_t offset = offset_pair.first;
        ObjectLocator *locator = frontier_obj->findLocator(offset, false);
        if (locator) {
          mem_value_t pt_values;
          locator->getValues(call, pt_values);

          for (mem_value_item_t &value_item : pt_values) {
            Value *val = value_item.val;
            PTResult *pts_result = findPTResult(val, false);
            if (pts_result) {
              PTResultIterator result_iter(pts_result, this);
              for (auto &pt_item : result_iter) {
                MemObject *pt_obj = pt_item.first->getObj();
                if (!cache.count(pt_obj)) {
                  cache[pt_obj] = frontier_depth + 1;
                  new_frontier.insert(pt_obj);
                }
              }
            }
          }
        }
      }
    }

    frontier.clear();
    for (MemObject *mem_obj : new_frontier) {
      frontier.insert(mem_obj);
    }
    new_frontier.clear();

    // Check if target found
    cache_find = cache.find(obj);
    if (cache_find != cache.end())
      return cache_find->second;

    frontier_depth++;
  }

  cache[obj] = FUNC_OBJ_UNREACHABLE;
  return FUNC_OBJ_UNREACHABLE;
}

const DataLayout &PTGraph::getDL() { return lotus_aa->getDataLayout(); }

path_cond_t PTGraph::getEmptyCond() {
  return ConstantInt::getTrue(analyzed_func->getContext());
}

path_cond_t PTGraph::getFalseCond() {
  return ConstantInt::getFalse(analyzed_func->getContext());
}

bool PTGraph::isAlwaysSatisfied(path_cond_t cond) const {
  if (!cond)
    return true;
  if (auto *CI = dyn_cast<ConstantInt>(cond))
    return CI->isOne();
  return false;
}

bool PTGraph::isSatisfiable(path_cond_t cond) const {
  if (!cond)
    return true;
  if (auto *CI = dyn_cast<ConstantInt>(cond))
    return !CI->isZero();
  return true;
}

bool PTGraph::isNoEffectFunction(Function *F) const {
  return lotus_aa && F && lotus_aa->getSpecManager().isNoEffect(F);
}

path_cond_t PTGraph::findOrCreateAndRegion(path_cond_t lhs, path_cond_t rhs) {
  if (!lhs)
    lhs = getEmptyCond();
  if (!rhs)
    rhs = getEmptyCond();
  if (!isSatisfiable(lhs) || !isSatisfiable(rhs))
    return getFalseCond();
  if (isAlwaysSatisfied(lhs))
    return rhs;
  if (isAlwaysSatisfied(rhs))
    return lhs;
  if (lhs == rhs)
    return lhs;
  return rhs;
}

path_cond_t PTGraph::findOrCreateOrRegion(path_cond_t lhs, path_cond_t rhs) {
  if (!lhs)
    lhs = getEmptyCond();
  if (!rhs)
    rhs = getEmptyCond();
  if (isAlwaysSatisfied(lhs) || isAlwaysSatisfied(rhs))
    return getEmptyCond();
  if (!isSatisfiable(lhs))
    return rhs;
  if (!isSatisfiable(rhs))
    return lhs;
  if (lhs == rhs)
    return lhs;
  return getEmptyCond();
}

path_cond_t PTGraph::findOrCreateUnitPhiRegion(BasicBlock *cur_bb,
                                               BasicBlock *incoming_bb) {
  auto cache_it = phi_region_cache.find(cur_bb);
  if (cache_it != phi_region_cache.end()) {
    auto cond_it = cache_it->second.find(incoming_bb);
    if (cond_it != cache_it->second.end())
      return cond_it->second;
  }

  path_cond_t cond = incoming_bb ? incoming_bb->getTerminator() : getEmptyCond();
  phi_region_cache[cur_bb][incoming_bb] = cond;
  return cond;
}
