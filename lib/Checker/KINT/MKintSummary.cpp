#include "MKintInternal.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

using namespace llvm;

namespace kint {

bool MKintPass::canSummarizeFunction(const Function &F) const {
  if (F.isDeclaration() || F.isVarArg())
    return false;
  if (!F.getReturnType()->isVoidTy() && !F.getReturnType()->isIntegerTy() &&
      !F.getReturnType()->isPointerTy())
    return false;
  for (const auto &bb : F) {
    for (const auto &inst : bb) {
      if (isa<InvokeInst>(inst) || isa<CallBrInst>(inst))
        return false;
      if (const auto *call = dyn_cast<CallInst>(&inst)) {
        if (call->isInlineAsm())
          return false;
        Function *callee = call->getCalledFunction();
        if (!callee && !isa<IntrinsicInst>(call))
          return false;
        if (callee && callee->isDeclaration() && !isAllocatorLike(callee) &&
            callee->getName() != "memset" && callee->getName() != "__memset" &&
            callee->getName() != "memcpy" && callee->getName() != "__memcpy" &&
            callee->getName() != "memmove" &&
            callee->getName() != "__memmove" && !call->doesNotAccessMemory() &&
            !call->onlyReadsMemory()) {
          return false;
        }
      }
    }
  }
  return true;
}

bool MKintPass::isAllocatorLike(const Function *callee) const {
  if (!callee)
    return false;
  const auto name = callee->getName();
  return name == "malloc" || name == "calloc" || name == "realloc" ||
         name == "kmalloc" || name == "kzalloc" || name == "vmalloc";
}

bool MKintPass::classifyPointerReturn(const Function &F, const Value *&root,
                                      std::string &reason) const {
  root = nullptr;
  if (!F.getReturnType()->isPointerTy())
    return true;

  for (const auto &bb : F) {
    const auto *ret = dyn_cast<ReturnInst>(bb.getTerminator());
    if (!ret)
      continue;
    const Value *retValue = ret->getReturnValue();
    if (!retValue || !retValue->getType()->isPointerTy()) {
      reason = "missing pointer return value";
      return false;
    }
    const Value *underlying =
        llvm::getUnderlyingObject(retValue->stripPointerCasts());
    if (!underlying) {
      reason = "unresolved pointer return root";
      return false;
    }
    const bool supported =
        isa<Argument>(underlying) || isa<GlobalVariable>(underlying) ||
        (isa<CallInst>(underlying) &&
         isAllocatorLike(cast<CallInst>(underlying)->getCalledFunction()));
    if (!supported) {
      reason = "unsupported pointer return root";
      return false;
    }
    if (root && root != underlying) {
      reason = "multiple pointer return roots";
      return false;
    }
    root = underlying;
  }

  if (!root) {
    reason = "missing pointer return root";
    return false;
  }
  return true;
}

bool MKintPass::isBoundaryVisiblePointerArg(const Argument &arg,
                                            const Function &F) const {
  for (const auto *user : arg.users()) {
    const auto *inst = dyn_cast<Instruction>(user);
    if (!inst || inst->getFunction() != &F)
      continue;
    return true;
  }
  return false;
}

std::vector<const GlobalVariable *>
MKintPass::collectReferencedGlobals(const Function &F) const {
  SetVector<const GlobalVariable *> globals;
  for (const auto &bb : F) {
    for (const auto &inst : bb) {
      for (const auto &operand : inst.operands()) {
        const Value *op = operand.get();
        if (!op)
          continue;
        if (const auto *gv = dyn_cast<GlobalVariable>(op->stripPointerCasts()))
          globals.insert(gv);
      }
    }
  }
  return {globals.begin(), globals.end()};
}

std::vector<const Value *>
MKintPass::collectSummaryObjects(const Function &F) const {
  SetVector<const Value *> objects;
  const Value *pointerReturnRoot = nullptr;
  std::string reason;
  (void)classifyPointerReturn(F, pointerReturnRoot, reason);
  for (const auto &arg : F.args()) {
    if (arg.getType()->isPointerTy() &&
        (isBoundaryVisiblePointerArg(arg, F) || pointerReturnRoot == &arg)) {
      objects.insert(&arg);
    }
  }
  for (const auto *gv : collectReferencedGlobals(F))
    objects.insert(gv);
  if (isa_and_nonnull<CallInst>(pointerReturnRoot))
    objects.insert(pointerReturnRoot);
  return {objects.begin(), objects.end()};
}

bool MKintPass::collectModifiedBoundaryObjects(Function &F,
                                               FunctionSummary &summary) {
  auto boundaryContains = [&](const Value *root) {
    for (const auto &binding : summary.boundary_objects) {
      if (binding.root == root)
        return true;
    }
    return false;
  };
  auto markRoot = [&](const Value *root) {
    if (!root)
      return false;
    root = resolveAliasedObject(root);
    if (boundaryContains(root)) {
      summary.modified_objects.insert(root);
      return true;
    }
    if (isa<AllocaInst>(root))
      return true;
    if (const auto *call = dyn_cast<CallInst>(root)) {
      if (root == summary.pointer_return_root) {
        summary.modified_objects.insert(root);
        return true;
      }
      if (isAllocatorLike(call->getCalledFunction()))
        return true;
    }
    return false;
  };
  auto markPointer = [&](const Value *ptr) {
    if (!ptr)
      return false;
    const Value *root = llvm::getUnderlyingObject(ptr->stripPointerCasts());
    return markRoot(root);
  };

  if (summary.pointer_return_root && isa<CallInst>(summary.pointer_return_root))
    summary.modified_objects.insert(summary.pointer_return_root);

  for (const auto &bb : F) {
    for (const auto &inst : bb) {
      if (const auto *store = dyn_cast<StoreInst>(&inst)) {
        if (!markPointer(store->getPointerOperand())) {
          m_summary_failure_reason = "unsupported boundary store target";
          return false;
        }
        continue;
      }
      if (const auto *memsetI = dyn_cast<MemSetInst>(&inst)) {
        if (!markPointer(memsetI->getRawDest())) {
          m_summary_failure_reason = "unsupported memset target";
          return false;
        }
        continue;
      }
      if (const auto *memcpyI = dyn_cast<MemCpyInst>(&inst)) {
        if (!markPointer(memcpyI->getRawDest())) {
          m_summary_failure_reason = "unsupported memcpy target";
          return false;
        }
        continue;
      }
      if (const auto *memmoveI = dyn_cast<MemMoveInst>(&inst)) {
        if (!markPointer(memmoveI->getRawDest())) {
          m_summary_failure_reason = "unsupported memmove target";
          return false;
        }
        continue;
      }
      if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
        if (!markPointer(rmw->getPointerOperand())) {
          m_summary_failure_reason = "unsupported atomicrmw target";
          return false;
        }
        continue;
      }
      if (const auto *cx = dyn_cast<AtomicCmpXchgInst>(&inst)) {
        if (!markPointer(cx->getPointerOperand())) {
          m_summary_failure_reason = "unsupported cmpxchg target";
          return false;
        }
        continue;
      }
      const auto *call = dyn_cast<CallInst>(&inst);
      if (!call)
        continue;
      Function *callee = call->getCalledFunction();
      if (!callee)
        continue;
      const auto name = callee->getName();
      if (name == "memset" || name == "__memset" || name == "memcpy" ||
          name == "__memcpy" || name == "memmove" || name == "__memmove") {
        continue;
      }
      if (isAllocatorLike(callee))
        continue;
      if (callee->isDeclaration()) {
        if (!call->doesNotAccessMemory() && !call->onlyReadsMemory()) {
          m_summary_failure_reason = "unsupported external side-effecting call";
          return false;
        }
        continue;
      }

      const FunctionSummary *nested = buildSummary(*callee);
      if (!nested || nested->availability != SummaryAvailability::Available) {
        m_summary_failure_reason = "unsupported nested call summary";
        return false;
      }
      for (const auto &binding : nested->boundary_objects) {
        if (!nested->modified_objects.count(binding.root))
          continue;
        switch (binding.kind) {
        case SummaryObjectKind::Argument:
          if (binding.arg_index >= call->arg_size() ||
              !markPointer(call->getArgOperand(binding.arg_index))) {
            m_summary_failure_reason = "unsupported nested modified argument";
            return false;
          }
          break;
        case SummaryObjectKind::Global:
          if (!markRoot(binding.root)) {
            m_summary_failure_reason = "unsupported nested modified global";
            return false;
          }
          break;
        case SummaryObjectKind::EscapedReturn:
          if (!call->getType()->isPointerTy() || !markRoot(call)) {
            m_summary_failure_reason =
                "unsupported nested modified escaped return";
            return false;
          }
          break;
        }
      }
    }
  }

  return true;
}

z3::expr
MKintPass::conjunctSummaryExprs(const std::vector<z3::expr> &exprs) const {
  auto &ctx = m_solver.value().ctx();
  if (exprs.empty())
    return ctx.bool_val(true);
  z3::expr_vector clauses(ctx);
  for (const auto &e : exprs)
    clauses.push_back(e);
  return z3::mk_and(clauses);
}

z3::expr
MKintPass::disjunctSummaryExprs(const std::vector<z3::expr> &exprs) const {
  auto &ctx = m_solver.value().ctx();
  if (exprs.empty())
    return ctx.bool_val(false);
  z3::expr_vector clauses(ctx);
  for (const auto &e : exprs)
    clauses.push_back(e);
  return z3::mk_or(clauses);
}

z3::expr MKintPass::closeSummaryClause(const FunctionSummary &summary,
                                       const z3::expr &clause) const {
  auto &ctx = m_solver.value().ctx();
  llvm::DenseSet<Z3_ast> exported;
  auto markExported = [&](const std::optional<z3::expr> &sym) {
    if (sym.has_value())
      exported.insert(sym.value());
  };

  markExported(summary.integer_return_symbol);
  markExported(summary.pointer_return_symbol);
  for (const auto &[_, sym] : summary.arg_symbols)
    markExported(sym);
  for (const auto &binding : summary.boundary_objects) {
    markExported(binding.base_in_symbol);
    markExported(binding.size_in_symbol);
    markExported(binding.mem_in);
    markExported(binding.base_out_symbol);
    markExported(binding.size_out_symbol);
    markExported(binding.mem_out);
  }

  llvm::DenseSet<Z3_ast> visitedNodes;
  llvm::DenseSet<Z3_ast> visitedConsts;
  std::vector<z3::expr> hiddenSymbols;
  collectFreeConstants(clause, visitedNodes, visitedConsts, hiddenSymbols);

  z3::expr_vector quantified(ctx);
  for (const auto &sym : hiddenSymbols) {
    if (!exported.count(sym))
      quantified.push_back(sym);
  }
  if (quantified.empty())
    return clause.simplify();
  return z3::exists(quantified, clause).simplify();
}

void MKintPass::finalizeSummaryContract(FunctionSummary &summary) {
  auto &ctx = m_solver.value().ctx();

  for (auto &binding : summary.boundary_objects) {
    if (binding.kind == SummaryObjectKind::EscapedReturn) {
      if (binding.base_out_symbol.has_value()) {
        summary.allocation_constraints.push_back(
            binding.base_out_symbol.value() != ctx.bv_val(0, m_ptr_bits));
      }
      if (binding.base_out_symbol.has_value() &&
          binding.size_out_symbol.has_value()) {
        summary.allocation_constraints.push_back(z3::bvadd_no_overflow(
            binding.base_out_symbol.value(), binding.size_out_symbol.value(),
            /*is_signed=*/false));
      }
      for (const auto &other : summary.boundary_objects) {
        if (other.root == binding.root)
          continue;
        if (!binding.base_out_symbol.has_value() ||
            !binding.size_out_symbol.has_value() ||
            !other.base_in_symbol.has_value() ||
            !other.size_in_symbol.has_value()) {
          continue;
        }
        const auto endThis =
            binding.base_out_symbol.value() + binding.size_out_symbol.value();
        const auto endOther =
            other.base_in_symbol.value() + other.size_in_symbol.value();
        summary.allocation_constraints.push_back(
            z3::ule(endThis, other.base_in_symbol.value()) ||
            z3::ule(endOther, binding.base_out_symbol.value()));
      }
      continue;
    }

    if (binding.mem_in.has_value() && binding.mem_out.has_value() &&
        !summary.modified_objects.count(binding.root) &&
        binding.include_in_frame) {
      summary.frame_constraints.push_back(binding.mem_out.value() ==
                                          binding.mem_in.value());
    }
  }

  if (!summary.path_case_clauses.empty()) {
    summary.exit_constraints.push_back(
        disjunctSummaryExprs(summary.path_case_clauses).simplify());
  }
}

const FunctionSummary *MKintPass::buildSummary(Function &F) {
  if (InterprocSummaryMode == SummaryMode::Off)
    return nullptr;

  auto it = m_summary_cache.find(&F);
  if (it == m_summary_cache.end()) {
    it = m_summary_cache.insert({&F, SummaryCacheEntry(&F)}).first;
  }
  SummaryCacheEntry &entry = it->second;
  if (entry.summary.availability == SummaryAvailability::Available)
    return &entry.summary;
  if (entry.summary.availability == SummaryAvailability::Unsupported &&
      !entry.building && !entry.summary.unsupported_reason.empty())
    return &entry.summary;
  if (entry.building) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.recursive = true;
    entry.summary.unsupported_reason = "recursive summary dependency";
    return &entry.summary;
  }
  if (!canSummarizeFunction(F)) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason = "unsupported function shape";
    return &entry.summary;
  }

  const Value *pointerReturnRoot = nullptr;
  std::string pointerReturnReason;
  if (!classifyPointerReturn(F, pointerReturnRoot, pointerReturnReason)) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason = pointerReturnReason;
    return &entry.summary;
  }

  auto &ctx = m_solver.value().ctx();
  auto old_solver = std::move(m_solver);
  auto old_smt_mem = std::move(m_smt_mem);
  auto old_v2sym = std::move(m_v2sym);
  auto old_bbpaths = std::move(m_bbpaths);
  auto old_obj_base = std::move(m_obj_base);
  auto old_obj_size = std::move(m_obj_size);
  auto old_obj_list = std::move(m_obj_list);
  auto old_obj_mem = std::move(m_obj_mem);
  auto old_obj_alias = std::move(m_obj_alias);
  auto old_object_frames = std::move(m_object_frames);
  auto old_sym_change_log = std::move(m_sym_change_log);
  auto old_sym_change_frames = std::move(m_sym_change_frames);
  auto old_path_constraints = std::move(m_path_constraints);
  auto old_constraint_frames = std::move(m_constraint_frames);
  auto old_universal_vars = std::move(m_universal_vars);
  auto old_universal_ids = std::move(m_universal_var_ids);
  FunctionSummary *old_building_summary = m_building_summary;
  const auto old_timeout = m_function_timeout;
  const auto old_path_limit = m_path_limit;
  const auto old_paths_explored = m_paths_explored;
  const auto old_path_limit_hit = m_path_limit_hit;
  const auto old_timeout_hit = m_timeout_hit;
  const auto old_summary_backedge_hit = m_summary_backedge_hit;
  const auto old_function_start_time = m_function_start_time;
  auto *old_aa = m_aa;
  auto *old_mssa = m_mssa;

  m_solver = z3::solver(ctx);
  m_smt_mem = std::make_unique<SmtMemory>(ctx, m_ptr_bits);
  m_v2sym.clear();
  m_bbpaths.clear();
  m_obj_base.clear();
  m_obj_size.clear();
  m_obj_list.clear();
  m_obj_mem.clear();
  m_obj_alias.clear();
  m_object_frames.clear();
  m_sym_change_log.clear();
  m_sym_change_frames.clear();
  m_path_constraints.clear();
  m_constraint_frames.clear();
  m_universal_vars.clear();
  m_universal_var_ids.clear();
  if (m_bug_detection)
    m_bug_detection->clearCurrentPath();

  entry.building = true;
  entry.summary = FunctionSummary(&F);
  entry.summary.has_integer_return = F.getReturnType()->isIntegerTy();
  entry.summary.has_pointer_return = F.getReturnType()->isPointerTy();
  entry.summary.pointer_return_root = pointerReturnRoot;
  if (entry.summary.has_integer_return) {
    const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
    entry.summary.integer_return_symbol = ctx.bv_const(
        ("%summary.ret." + F.getName().str() + "." + std::to_string(id))
            .c_str(),
        F.getReturnType()->getIntegerBitWidth());
  }
  if (entry.summary.has_pointer_return) {
    const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
    entry.summary.pointer_return_symbol = ctx.bv_const(
        ("%summary.ret.ptr." + F.getName().str() + "." + std::to_string(id))
            .c_str(),
        m_ptr_bits);
  }

  m_building_summary = &entry.summary;
  m_summary_failed = false;
  m_summary_failure_reason.clear();
  m_function_timeout = m_summary_timeout;
  m_path_limit = m_summary_path_limit;
  m_paths_explored = 0;
  m_path_limit_hit = false;
  m_timeout_hit = false;
  m_summary_backedge_hit = false;
  m_function_start_time = std::chrono::steady_clock::now();

  if (m_fam && (m_aa = m_fam->getCachedResult<llvm::AAManager>(F))) {
    // cached
  } else if (m_fam) {
    m_aa = &m_fam->getResult<llvm::AAManager>(F);
  } else {
    m_aa = nullptr;
  }
  if (m_fam) {
    if (auto *MSSARes = m_fam->getCachedResult<llvm::MemorySSAAnalysis>(F)) {
      m_mssa = &MSSARes->getMSSA();
    } else {
      m_mssa = &m_fam->getResult<llvm::MemorySSAAnalysis>(F).getMSSA();
    }
  } else {
    m_mssa = nullptr;
  }

  for (auto &arg : F.args()) {
    const std::string argName = ("summary." + F.getName().str() + ".arg." +
                                 std::to_string(arg.getArgNo()));
    if (arg.getType()->isIntegerTy()) {
      z3::expr sym =
          ctx.bv_const(argName.c_str(), arg.getType()->getIntegerBitWidth());
      entry.summary.integer_args.push_back(&arg);
      entry.summary.arg_symbols[&arg] = sym;
      setSym(&arg, sym);
    } else if (arg.getType()->isPointerTy()) {
      entry.summary.pointer_args.push_back(&arg);
      auto sizeSym = ctx.bv_const((argName + ".size").c_str(), m_ptr_bits);
      ensureObject(&arg, argName + ".obj", sizeSym, false);
      z3::expr sym = m_obj_base[&arg].value();
      entry.summary.arg_symbols[&arg] = sym;
      setSym(&arg, sym);
    }
  }

  for (const Value *obj : collectSummaryObjects(F)) {
    SummaryObjectKind kind = SummaryObjectKind::Argument;
    if (isa<GlobalVariable>(obj))
      kind = SummaryObjectKind::Global;
    else if (obj == pointerReturnRoot && isa<CallInst>(obj))
      kind = SummaryObjectKind::EscapedReturn;

    if (kind == SummaryObjectKind::Global && !m_obj_base.count(obj)) {
      const auto *gv = cast<GlobalVariable>(obj);
      const uint64_t bytes = m_dl->getTypeAllocSize(gv->getValueType());
      ensureObject(gv, ("summary.global." + gv->getName()).str(),
                   ctx.bv_val(bytes, m_ptr_bits), true);
    }
    unsigned argIndex = ~0U;
    if (const auto *arg = dyn_cast<Argument>(obj))
      argIndex = arg->getArgNo();
    std::optional<z3::expr> baseSym = std::nullopt;
    std::optional<z3::expr> sizeSym = std::nullopt;
    std::optional<z3::expr> memIn = std::nullopt;
    if (kind != SummaryObjectKind::EscapedReturn && m_obj_base.count(obj)) {
      baseSym = m_obj_base[obj];
      sizeSym = m_obj_size[obj];
      if (m_obj_mem.count(obj))
        memIn = m_obj_mem[obj];
    }
    entry.summary.boundary_objects.emplace_back(
        kind, obj, argIndex, /*include_in_frame=*/true, baseSym, sizeSym, memIn,
        std::nullopt, std::nullopt, std::nullopt);
  }

  if (!collectModifiedBoundaryObjects(F, entry.summary)) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason =
        m_summary_failure_reason.empty() ? "summary modifies collection failed"
                                         : m_summary_failure_reason;
    entry.building = false;
    m_solver = std::move(old_solver);
    m_smt_mem = std::move(old_smt_mem);
    m_v2sym = std::move(old_v2sym);
    m_bbpaths = std::move(old_bbpaths);
    m_obj_base = std::move(old_obj_base);
    m_obj_size = std::move(old_obj_size);
    m_obj_list = std::move(old_obj_list);
    m_obj_mem = std::move(old_obj_mem);
    m_obj_alias = std::move(old_obj_alias);
    m_object_frames = std::move(old_object_frames);
    m_sym_change_log = std::move(old_sym_change_log);
    m_sym_change_frames = std::move(old_sym_change_frames);
    m_path_constraints = std::move(old_path_constraints);
    m_constraint_frames = std::move(old_constraint_frames);
    m_universal_vars = std::move(old_universal_vars);
    m_universal_var_ids = std::move(old_universal_ids);
    m_building_summary = old_building_summary;
    m_function_timeout = old_timeout;
    m_path_limit = old_path_limit;
    m_paths_explored = old_paths_explored;
    m_path_limit_hit = old_path_limit_hit;
    m_timeout_hit = old_timeout_hit;
    m_summary_backedge_hit = old_summary_backedge_hit;
    m_function_start_time = old_function_start_time;
    m_aa = old_aa;
    m_mssa = old_mssa;
    m_summary_failed = false;
    m_summary_failure_reason.clear();
    return &entry.summary;
  }

  for (auto &binding : entry.summary.boundary_objects) {
    if (binding.mem_in.has_value() ||
        entry.summary.modified_objects.count(binding.root)) {
      const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
      binding.mem_out = ctx.constant(
          ("%summary.memout." + F.getName().str() + "." + std::to_string(id))
              .c_str(),
          ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
    }
  }

  entry.summary.entry_constraints = m_path_constraints;

  for (auto &bb : F) {
    for (const auto *pred : predecessors(&bb)) {
      if (m_backedges[&bb].contains(pred) || &bb == pred)
        continue;
      m_bbpaths[pred].push_back(&bb);
    }
  }

  m_solver.value().push();
  pushSymFrame();
  pushObjectFrame();
  m_smt_mem->push();
  pushConstraintFrame();
  path_solving(&F.getEntryBlock(), nullptr);
  for (auto &binding : entry.summary.boundary_objects) {
    if (binding.kind != SummaryObjectKind::EscapedReturn)
      continue;
    if (!m_obj_base.count(binding.root) || !m_obj_mem.count(binding.root) ||
        !m_obj_mem[binding.root].has_value()) {
      m_summary_failed = true;
      if (m_summary_failure_reason.empty()) {
        m_summary_failure_reason =
            "missing escaped return object state in summary";
      }
      break;
    }
    binding.base_out_symbol = m_obj_base[binding.root];
    binding.size_out_symbol = m_obj_size[binding.root];
    if (!binding.mem_out.has_value()) {
      const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
      binding.mem_out = ctx.constant(
          ("%summary.memout." + F.getName().str() + "." + std::to_string(id))
              .c_str(),
          ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
    }
  }
  m_smt_mem->pop();
  popSymFrame();
  popObjectFrame();
  popConstraintFrame();
  m_solver.value().pop();

  if (!m_summary_failed && (m_path_limit_hit || m_timeout_hit)) {
    m_summary_failed = true;
    if (m_summary_failure_reason.empty()) {
      m_summary_failure_reason = "summary exploration incomplete";
    }
  }
  if (!m_summary_failed && m_summary_backedge_hit) {
    m_summary_failed = true;
    if (m_summary_failure_reason.empty()) {
      m_summary_failure_reason = "loop backedge encountered in summary";
    }
  }

  if (!m_summary_failed && !entry.summary.path_case_clauses.empty()) {
    finalizeSummaryContract(entry.summary);
    entry.summary.availability = SummaryAvailability::Available;
  } else {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason = m_summary_failure_reason.empty()
                                           ? "summary construction failed"
                                           : m_summary_failure_reason;
  }
  entry.building = false;

  m_solver = std::move(old_solver);
  m_smt_mem = std::move(old_smt_mem);
  m_v2sym = std::move(old_v2sym);
  m_bbpaths = std::move(old_bbpaths);
  m_obj_base = std::move(old_obj_base);
  m_obj_size = std::move(old_obj_size);
  m_obj_list = std::move(old_obj_list);
  m_obj_mem = std::move(old_obj_mem);
  m_obj_alias = std::move(old_obj_alias);
  m_object_frames = std::move(old_object_frames);
  m_sym_change_log = std::move(old_sym_change_log);
  m_sym_change_frames = std::move(old_sym_change_frames);
  m_path_constraints = std::move(old_path_constraints);
  m_constraint_frames = std::move(old_constraint_frames);
  m_universal_vars = std::move(old_universal_vars);
  m_universal_var_ids = std::move(old_universal_ids);
  m_building_summary = old_building_summary;
  m_function_timeout = old_timeout;
  m_path_limit = old_path_limit;
  m_paths_explored = old_paths_explored;
  m_path_limit_hit = old_path_limit_hit;
  m_timeout_hit = old_timeout_hit;
  m_summary_backedge_hit = old_summary_backedge_hit;
  m_function_start_time = old_function_start_time;
  m_aa = old_aa;
  m_mssa = old_mssa;
  m_summary_failed = false;
  m_summary_failure_reason.clear();

  return &entry.summary;
}

SummaryAvailability MKintPass::applySummary(CallInst *call, BasicBlock *cur,
                                            BasicBlock *pred) {
  if (InterprocSummaryMode == SummaryMode::Off || !call)
    return SummaryAvailability::Disabled;
  Function *callee = call->getCalledFunction();
  if (!callee || callee->isDeclaration())
    return SummaryAvailability::Unsupported;

  const FunctionSummary *summary = buildSummary(*callee);
  if (!summary || summary->availability != SummaryAvailability::Available)
    return SummaryAvailability::Unsupported;

  auto &ctx = m_solver.value().ctx();
  z3::expr_vector from(ctx);
  z3::expr_vector to(ctx);

  for (const auto &arg : summary->integer_args) {
    auto it = summary->arg_symbols.find(arg);
    if (it == summary->arg_symbols.end() || !it->second.has_value())
      return SummaryAvailability::Unsupported;
    from.push_back(it->second.value());
    to.push_back(getIntExpr(call->getArgOperand(arg->getArgNo()), cur, pred));
  }
  for (const auto &arg : summary->pointer_args) {
    auto it = summary->arg_symbols.find(arg);
    if (it == summary->arg_symbols.end() || !it->second.has_value())
      return SummaryAvailability::Unsupported;
    from.push_back(it->second.value());
    to.push_back(getPtrExpr(call->getArgOperand(arg->getArgNo()), cur, pred));
  }

  auto mapBindingObject =
      [&](const SummaryObjectBinding &binding) -> const Value * {
    switch (binding.kind) {
    case SummaryObjectKind::Argument:
      if (binding.arg_index >= call->arg_size())
        return nullptr;
      return getObjectForPtr(call->getArgOperand(binding.arg_index));
    case SummaryObjectKind::Global:
      return resolveAliasedObject(binding.root);
    case SummaryObjectKind::EscapedReturn: {
      if (!call->getType()->isPointerTy())
        return nullptr;
      if (!m_obj_base.count(call)) {
        const auto id =
            g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
        z3::expr sizeExpr = ctx.bv_const(("%summary.call.size." +
                                          std::to_string((uintptr_t)call) +
                                          "." + std::to_string(id))
                                             .c_str(),
                                         m_ptr_bits);
        ensureObject(call, "summary.escape." + std::to_string((uintptr_t)call),
                     sizeExpr, /*sizeKnown=*/false);
      }
      return call;
    }
    }
    return nullptr;
  };

  DenseMap<const Value *, const Value *> mappedBoundaryRoots;
  SmallVector<std::pair<const Value *, z3::expr>, 8> memUpdates;
  for (const auto &objBinding : summary->boundary_objects) {
    const Value *mappedObject = mapBindingObject(objBinding);
    if (!mappedObject)
      return SummaryAvailability::Unsupported;
    auto mappedIt = mappedBoundaryRoots.find(mappedObject);
    if (mappedIt != mappedBoundaryRoots.end() &&
        mappedIt->second != objBinding.root) {
      // Summary construction assumes distinct visible roots denote distinct
      // boundary objects. If actuals alias, fall back instead of treating the
      // call as infeasible in the caller.
      return SummaryAvailability::Unsupported;
    }
    mappedBoundaryRoots[mappedObject] = objBinding.root;
    if (objBinding.base_in_symbol.has_value()) {
      if (!m_obj_base.count(mappedObject) ||
          !m_obj_base[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.base_in_symbol.value());
      to.push_back(m_obj_base[mappedObject].value());
    }
    if (objBinding.size_in_symbol.has_value()) {
      if (!m_obj_size.count(mappedObject) ||
          !m_obj_size[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.size_in_symbol.value());
      to.push_back(m_obj_size[mappedObject].value());
    }
    if (objBinding.mem_in.has_value()) {
      if (!m_obj_mem.count(mappedObject) ||
          !m_obj_mem[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.mem_in.value());
      to.push_back(m_obj_mem[mappedObject].value());
    }
    if (objBinding.base_out_symbol.has_value()) {
      if (!m_obj_base.count(mappedObject) ||
          !m_obj_base[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.base_out_symbol.value());
      to.push_back(m_obj_base[mappedObject].value());
    }
    if (objBinding.size_out_symbol.has_value()) {
      if (!m_obj_size.count(mappedObject) ||
          !m_obj_size[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.size_out_symbol.value());
      to.push_back(m_obj_size[mappedObject].value());
    }
    if (objBinding.mem_out.has_value()) {
      const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
      z3::expr postMem =
          ctx.constant(("%summary.call.mem." + std::to_string((uintptr_t)call) +
                        "." + std::to_string(id))
                           .c_str(),
                       ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
      from.push_back(objBinding.mem_out.value());
      to.push_back(postMem);
      memUpdates.emplace_back(mappedObject, postMem);
    }
  }

  if (summary->integer_return_symbol.has_value()) {
    const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
    z3::expr retSym =
        ctx.bv_const(("%summary.call.ret." + std::to_string((uintptr_t)call) +
                      "." + std::to_string(id))
                         .c_str(),
                     call->getType()->getIntegerBitWidth());
    from.push_back(summary->integer_return_symbol.value());
    to.push_back(retSym);
    setSym(call, retSym);
  }

  if (summary->pointer_return_symbol.has_value()) {
    if (!call->getType()->isPointerTy())
      return SummaryAvailability::Unsupported;
    z3::expr retPtr = ctx.bv_const(
        ("%summary.call.ptr." + std::to_string((uintptr_t)call)).c_str(),
        m_ptr_bits);
    if (summary->pointer_return_root) {
      if (isa<CallInst>(summary->pointer_return_root)) {
        if (!m_obj_base.count(call) || !m_obj_base[call].has_value())
          return SummaryAvailability::Unsupported;
        retPtr = m_obj_base[call].value();
      } else if (const auto *arg =
                     dyn_cast<Argument>(summary->pointer_return_root)) {
        const Value *mappedRoot =
            getObjectForPtr(call->getArgOperand(arg->getArgNo()));
        if (mappedRoot)
          m_obj_alias[call] = mappedRoot;
      } else if (isa<GlobalVariable>(summary->pointer_return_root)) {
        m_obj_alias[call] = summary->pointer_return_root;
      }
    }
    from.push_back(summary->pointer_return_symbol.value());
    to.push_back(retPtr);
    setSym(call, retPtr);
  }

  addConstraint(
      conjunctSummaryExprs(summary->entry_constraints).substitute(from, to));
  if (m_solver.value().check() == z3::unsat)
    return SummaryAvailability::Available;

  // Boogie-style call step: after preconditions are checked against the
  // incoming state, expose fresh post-state symbols for modified visible
  // memory before constraining them with summary postconditions.
  for (const auto &update : memUpdates)
    m_obj_mem[update.first] = update.second;

  addConstraint(
      conjunctSummaryExprs(summary->exit_constraints).substitute(from, to));
  addConstraint(conjunctSummaryExprs(summary->allocation_constraints)
                    .substitute(from, to));
  addConstraint(
      conjunctSummaryExprs(summary->frame_constraints).substitute(from, to));
  return SummaryAvailability::Available;
}

bool MKintPass::recordSummaryCase(ReturnInst *ret, BasicBlock *cur,
                                  BasicBlock *pred) {
  (void)pred;
  if (!m_building_summary || !ret)
    return true;

  z3::expr clause = buildPathConstraintConjunction();
  if (m_building_summary->integer_return_symbol.has_value()) {
    Value *retValue = ret->getReturnValue();
    if (!retValue || !retValue->getType()->isIntegerTy()) {
      m_summary_failure_reason = "non-integer return in integer summary";
      return false;
    }
    clause = clause && (m_building_summary->integer_return_symbol.value() ==
                        getIntExpr(retValue, cur, nullptr));
  }
  if (m_building_summary->pointer_return_symbol.has_value()) {
    Value *retValue = ret->getReturnValue();
    if (!retValue || !retValue->getType()->isPointerTy()) {
      m_summary_failure_reason = "non-pointer return in pointer summary";
      return false;
    }
    clause = clause && (m_building_summary->pointer_return_symbol.value() ==
                        getPtrExpr(retValue, cur, nullptr));
  }
  for (const auto &objBinding : m_building_summary->boundary_objects) {
    if (!objBinding.mem_out.has_value())
      continue;
    auto it = m_obj_mem.find(resolveAliasedObject(objBinding.root));
    if (it == m_obj_mem.end() || !it->second.has_value()) {
      m_summary_failure_reason = "missing summary object memory state";
      return false;
    }
    clause = clause && (objBinding.mem_out.value() == it->second.value());
  }
  m_building_summary->path_case_clauses.push_back(
      closeSummaryClause(*m_building_summary, clause));
  return true;
}

} // namespace kint
