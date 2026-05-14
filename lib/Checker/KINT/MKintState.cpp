#include "MKintInternal.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

using namespace llvm;

namespace kint {

void MKintPass::pushSymFrame() {
  m_sym_change_frames.push_back(m_sym_change_log.size());
}

void MKintPass::pushObjectFrame() {
  m_object_frames.push_back(
      {m_obj_base, m_obj_size, m_obj_list, m_obj_mem, m_obj_alias});
}

void MKintPass::popObjectFrame() {
  if (m_object_frames.empty())
    return;
  ObjectStateFrame frame = std::move(m_object_frames.back());
  m_object_frames.pop_back();
  m_obj_base = std::move(frame.obj_base);
  m_obj_size = std::move(frame.obj_size);
  m_obj_list = std::move(frame.obj_list);
  m_obj_mem = std::move(frame.obj_mem);
  m_obj_alias = std::move(frame.obj_alias);
}

void MKintPass::popSymFrame() {
  if (m_sym_change_frames.empty())
    return;
  const size_t frameStart = m_sym_change_frames.back();
  m_sym_change_frames.pop_back();

  while (m_sym_change_log.size() > frameStart) {
    const SymChange ch = m_sym_change_log.back();
    m_sym_change_log.pop_back();
    if (!ch.key)
      continue;
    if (ch.hadOld) {
      m_v2sym[ch.key] = ch.oldValue;
    } else {
      m_v2sym.erase(ch.key);
    }
  }
}

void MKintPass::setSym(const Value *v, const z3::expr &e) {
  SymChange ch;
  ch.key = v;
  auto it = m_v2sym.find(v);
  ch.hadOld = (it != m_v2sym.end());
  if (ch.hadOld)
    ch.oldValue = it->second;
  m_sym_change_log.push_back(ch);
  m_v2sym[v] = e;
}

void MKintPass::pushConstraintFrame() {
  m_constraint_frames.push_back(m_path_constraints.size());
}

void MKintPass::popConstraintFrame() {
  if (m_constraint_frames.empty())
    return;
  const size_t frameStart = m_constraint_frames.back();
  m_constraint_frames.pop_back();
  while (m_path_constraints.size() > frameStart) {
    m_path_constraints.pop_back();
  }
}

void MKintPass::addConstraint(const z3::expr &e) {
  m_solver.value().add(e);
  if (m_robust_reachability || m_building_summary) {
    m_path_constraints.push_back(e);
  }
}

const Value *MKintPass::resolveAliasedObject(const Value *obj) const {
  const Value *cur = obj;
  DenseSet<const Value *> seen;
  while (cur) {
    if (!seen.insert(cur).second)
      break;
    auto it = m_obj_alias.find(cur);
    if (it == m_obj_alias.end() || !it->second)
      break;
    cur = it->second;
  }
  return cur;
}

const Value *MKintPass::getObjectForPtr(const Value *ptr) const {
  if (!ptr)
    return nullptr;
  const Value *stripped = ptr->stripPointerCasts();
  const Value *obj = llvm::getUnderlyingObject(stripped);
  obj = resolveAliasedObject(obj);
  if (obj && m_obj_base.count(obj))
    return obj;
  return nullptr;
}

z3::expr MKintPass::loadBytesFromMem(const z3::expr &mem,
                                     const z3::expr &offset, unsigned numBytes,
                                     bool littleEndian) const {
  auto &ctx = m_solver.value().ctx();
  if (numBytes == 0)
    return ctx.bv_val(0, 0);
  z3::expr result = z3::select(
      mem, offset + ctx.bv_val(littleEndian ? (numBytes - 1) : 0, m_ptr_bits));
  for (unsigned i = 1; i < numBytes; ++i) {
    const unsigned byteIndex = littleEndian ? (numBytes - 1 - i) : i;
    z3::expr b = z3::select(mem, offset + ctx.bv_val(byteIndex, m_ptr_bits));
    result = z3::concat(result, b);
  }
  return result;
}

z3::expr MKintPass::storeBytesToMem(const z3::expr &mem, const z3::expr &offset,
                                    const z3::expr &value, unsigned numBytes,
                                    bool littleEndian) const {
  auto &ctx = m_solver.value().ctx();
  if (numBytes == 0)
    return mem;
  const unsigned targetBits = numBytes * 8;
  z3::expr v = value;
  const unsigned vbw = v.get_sort().bv_size();
  if (vbw < targetBits)
    v = z3::zext(v, targetBits - vbw);
  else if (vbw > targetBits)
    v = v.extract(targetBits - 1, 0);

  z3::expr cur = mem;
  for (unsigned i = 0; i < numBytes; ++i) {
    const unsigned valueByteIndex = littleEndian ? i : (numBytes - 1 - i);
    const unsigned lo = valueByteIndex * 8;
    const unsigned hi = lo + 7;
    z3::expr byteVal = v.extract(hi, lo);
    cur = z3::store(cur, offset + ctx.bv_val(i, m_ptr_bits), byteVal);
  }
  return cur;
}

void MKintPass::havocObject(const Value *obj, const std::string &hint) {
  if (!obj)
    return;
  auto &ctx = m_solver.value().ctx();
  const auto id = g_obj_mem_id.fetch_add(1, std::memory_order_relaxed);
  const std::string name = "%objmem." + hint + "." + std::to_string(id);
  m_obj_mem[obj] = ctx.constant(
      name.c_str(), ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
}

bool MKintPass::callMayModObject(llvm::CallBase *call, const Value *obj) const {
  if (!call || !obj || !m_aa)
    return true;
  llvm::LocationSize size = llvm::LocationSize::afterPointer();
  if (m_obj_size.count(obj)) {
    if (auto sizeExpr = m_obj_size.find(obj)->second) {
      if (sizeExpr->is_numeral()) {
        uint64_t bytes = 0;
        if (Z3_get_numeral_uint64(sizeExpr->ctx(), *sizeExpr, &bytes)) {
          size = llvm::LocationSize::precise(bytes);
        }
      }
    }
  }
  llvm::MemoryLocation loc(obj, size);
  auto modref = m_aa->getModRefInfo(call, loc);
  return llvm::isModSet(modref);
}

z3::expr MKintPass::buildPathConstraintConjunction() const {
  auto &ctx = m_solver.value().ctx();
  if (m_path_constraints.empty())
    return ctx.bool_val(true);
  z3::expr_vector pcs(ctx);
  for (const auto &c : m_path_constraints)
    pcs.push_back(c);
  return z3::mk_and(pcs);
}

void MKintPass::registerUniversal(const z3::expr &e) {
  if (!m_robust_reachability)
    return;
  Z3_ast key = e;
  if (m_universal_var_ids.insert(key).second) {
    m_universal_vars.push_back(e);
  }
}

void MKintPass::dumpEfConstraint(const Instruction *inst, interr type,
                                 const z3::expr &q) const {
  if (m_dump_ef_path.empty())
    return;
  std::ofstream out(m_dump_ef_path, std::ios::app);
  if (!out.is_open())
    return;
  out << "=== EF Constraint ===\n";
  if (inst && inst->getParent() && inst->getParent()->getParent()) {
    out << "Function: " << inst->getParent()->getParent()->getName().str()
        << "\n";
  }
  if (type != interr::NONE) {
    out << "Bug: " << bugTypeToString(type) << "\n";
  }
  if (inst) {
    std::string instStr;
    llvm::raw_string_ostream instOS(instStr);
    instOS << *inst;
    out << "Inst: " << instOS.str() << "\n";
  }
  out << q.to_string() << "\n";
  out << "=====================\n";
}

bool MKintPass::checkBugCondition(const Instruction *inst, interr type,
                                  const z3::expr &bugCond) {
  if (m_building_summary)
    return false;
  if (!m_robust_reachability) {
    m_solver.value().push();
    m_solver.value().add(bugCond);
    const bool sat = (m_solver.value().check() == z3::sat);
    m_solver.value().pop();
    return sat;
  }
  if (type != interr::NONE && !isRobustBugEnabled(type)) {
    return false;
  }

  auto &ctx = m_solver.value().ctx();
  z3::solver qsolver(ctx);
  z3::expr body = buildPathConstraintConjunction() && bugCond;
  z3::expr q = body;
  if (!m_universal_vars.empty()) {
    z3::expr_vector uvars(ctx);
    for (const auto &v : m_universal_vars)
      uvars.push_back(v);
    q = z3::forall(uvars, body);
    qsolver.add(q);
  } else {
    qsolver.add(body);
  }
  dumpEfConstraint(inst, type, q);
  return qsolver.check() == z3::sat;
}

bool MKintPass::isRobustBugEnabled(interr type) const {
  if (m_robust_bug_filter.empty())
    return true;
  return m_robust_bug_filter.count(type) > 0;
}

void MKintPass::parseRobustBugFilter(const std::string &csv) {
  m_robust_bug_filter.clear();
  if (csv.empty())
    return;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t end = csv.find(',', start);
    if (end == std::string::npos)
      end = csv.size();
    auto token = csv.substr(start, end - start);
    auto trim = [](const std::string &s) {
      size_t b = s.find_first_not_of(" \t");
      size_t e = s.find_last_not_of(" \t");
      if (b == std::string::npos)
        return std::string();
      return s.substr(b, e - b + 1);
    };
    token = trim(token);
    if (token == "overflow") {
      m_robust_bug_filter.insert(interr::INT_OVERFLOW);
    } else if (token == "div0" || token == "div") {
      m_robust_bug_filter.insert(interr::DIV_BY_ZERO);
    } else if (token == "shift") {
      m_robust_bug_filter.insert(interr::BAD_SHIFT);
    } else if (token == "oob" || token == "array-oob") {
      m_robust_bug_filter.insert(interr::ARRAY_OOB);
    } else if (token == "dead") {
      m_robust_bug_filter.insert(interr::DEAD_TRUE_BR);
      m_robust_bug_filter.insert(interr::DEAD_FALSE_BR);
    }
    start = end + 1;
  }
}

bool MKintPass::isLittleEndian() const {
  return m_dl ? m_dl->isLittleEndian() : true;
}
void MKintPass::ensureObject(const Value *obj, const std::string &hintName,
                             const z3::expr &sizeBytes, bool sizeKnown) {
  if (m_obj_base.count(obj))
    return;

  auto &ctx = m_solver.value().ctx();
  const auto base = ctx.bv_const(hintName.c_str(), m_ptr_bits);

  m_obj_base[obj] = base;
  m_obj_size[obj] = sizeBytes;
  m_obj_list.push_back(obj);
  havocObject(obj, hintName);

  // Basic well-formedness: keep base non-zero to avoid conflating with null.
  addConstraint(base != ctx.bv_val(0, m_ptr_bits));

  // Disjointness constraints against previously created objects.
  for (const auto *other : m_obj_list) {
    if (other == obj)
      continue;
    if (!m_obj_base.count(other) || !m_obj_size.count(other))
      continue;
    const auto otherBase = m_obj_base[other].value();
    const auto otherSize = m_obj_size[other].value();
    if (sizeKnown) {
      // Non-overlap: [base, base+size) does not overlap [otherBase,
      // otherBase+otherSize)
      const auto endThis = base + sizeBytes;
      const auto endOther = otherBase + otherSize;
      addConstraint(z3::ule(endThis, otherBase) || z3::ule(endOther, base));
    } else {
      // Unknown size: at least force distinct bases.
      addConstraint(base != otherBase);
    }
  }

  // Avoid modular wraparound when computing [base, base+size) for known-size
  // objects.
  if (sizeKnown) {
    addConstraint(z3::bvadd_no_overflow(base, sizeBytes, /*is_signed=*/false));
  }
}

bool MKintPass::maybeCheckOOB(const Instruction *at, const Value *ptrOperand,
                              uint64_t accessBytes, BasicBlock *cur,
                              BasicBlock *pred) {
  if (!CheckArrayOOB)
    return true;
  if (!at || !ptrOperand || !m_solver || !m_smt_mem)
    return true;
  if (accessBytes == 0)
    return true;

  const Value *stripped = ptrOperand->stripPointerCasts();
  const auto *gep = dyn_cast<GetElementPtrInst>(stripped);
  if (!gep)
    return true; // keep reporting consistent with existing ARRAY_OOB pipeline

  const Value *obj = llvm::getUnderlyingObject(stripped);
  if (!obj)
    return true;
  if (!m_obj_base.count(obj) || !m_obj_size.count(obj))
    return true;

  const auto baseOpt = m_obj_base[obj];
  const auto sizeOpt = m_obj_size[obj];
  if (!baseOpt.has_value() || !sizeOpt.has_value())
    return true;

  auto &solver = m_solver.value();
  auto &ctx = solver.ctx();
  const auto &base = baseOpt.value();
  const auto &size = sizeOpt.value();
  const auto addr = getPtrExpr(ptrOperand, cur, pred);
  const auto len = ctx.bv_val(accessBytes, m_ptr_bits);

  // In-bounds check for a byte range: addr >= base && addr + len <= base +
  // size, without wrapping.
  const z3::expr noWrap = z3::bvadd_no_overflow(addr, len, /*is_signed=*/false);
  const z3::expr inBounds =
      (z3::uge(addr, base) && z3::ule(addr + len, base + size) && noWrap);

  if (checkBugCondition(gep, interr::ARRAY_OOB, !inBounds)) {
    m_gep_oob.insert(const_cast<GetElementPtrInst *>(gep));
    if (m_bug_detection) {
      m_bug_detection->recordBug(gep, interr::ARRAY_OOB);
    }
  }

  // Constrain the remaining exploration to defined, in-bounds behaviors (LLVM
  // semantics for out-of-bounds are UB).
  addConstraint(inBounds);
  return solver.check() != z3::unsat;
}

bool MKintPass::addWellDefinedConstraints(BinaryOperator *op, BasicBlock *cur,
                                          BasicBlock *pred) {
  if (!op || !m_solver)
    return true;
  auto &solver = m_solver.value();
  auto &ctx = solver.ctx();

  const auto lhs = getIntExpr(op->getOperand(0), cur, pred);
  const auto rhs = getIntExpr(op->getOperand(1), cur, pred);
  const unsigned bw = lhs.get_sort().bv_size();

  bool added = false;
  const auto addAndMark = [&](const z3::expr &e) {
    addConstraint(e);
    added = true;
  };

  switch (op->getOpcode()) {
  case Instruction::UDiv:
  case Instruction::URem:
  case Instruction::SDiv:
  case Instruction::SRem:
    // Div/rem by zero is poison; keep exploring only defined paths.
    addAndMark(rhs != ctx.bv_val(0, bw));
    if (op->getOpcode() == Instruction::SDiv) {
      // Signed division overflow (INT_MIN / -1) is poison in LLVM.
      addAndMark(z3::bvsdiv_no_overflow(lhs, rhs));
    }
    break;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    // Shift amount must be < bitwidth; otherwise poison.
    addAndMark(z3::ult(rhs, ctx.bv_val(bw, bw)));
    break;
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
    if (auto *ofop = dyn_cast<OverflowingBinaryOperator>(op)) {
      const bool nsw = ofop->hasNoSignedWrap();
      const bool nuw = ofop->hasNoUnsignedWrap();
      if (nuw) {
        if (op->getOpcode() == Instruction::Add) {
          addAndMark(z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/false));
        } else if (op->getOpcode() == Instruction::Sub) {
          addAndMark(z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/false));
        } else if (op->getOpcode() == Instruction::Mul) {
          addAndMark(z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/false));
        }
      }
      if (nsw) {
        if (op->getOpcode() == Instruction::Add) {
          addAndMark(z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvadd_no_underflow(lhs, rhs));
        } else if (op->getOpcode() == Instruction::Sub) {
          addAndMark(z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvsub_no_overflow(lhs, rhs));
        } else if (op->getOpcode() == Instruction::Mul) {
          addAndMark(z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvmul_no_underflow(lhs, rhs));
        }
      }
    }
    break;
  default:
    break;
  }

  if (!added)
    return true;
  return solver.check() != z3::unsat;
}

z3::expr MKintPass::getValueExpr(const Value *v, BasicBlock *cur,
                                 BasicBlock *pred) {
  if (!v)
    return m_solver.value().ctx().bv_val(0, 1);
  if (v->getType()->isIntegerTy())
    return getIntExpr(v, cur, pred);
  if (v->getType()->isPointerTy())
    return getPtrExpr(v, cur, pred);
  // Unsupported sort: return a fresh 1-bit value to keep the solver going.
  const std::string name = "%unsupported." + std::to_string((uintptr_t)v);
  return m_solver.value().ctx().bv_const(name.c_str(), 1);
}

z3::expr MKintPass::getIntExpr(const Value *v, BasicBlock *cur,
                               BasicBlock *pred) {
  auto it = m_v2sym.find(v);
  if (it != m_v2sym.end())
    return it->second.value();

  auto &ctx = m_solver.value().ctx();

  if (const auto *ci = dyn_cast<ConstantInt>(v)) {
    return bvValFromAPInt(ctx, ci->getValue());
  }

  if (const auto *fr = dyn_cast<FreezeInst>(v)) {
    auto r = getIntExpr(fr->getOperand(0), cur, pred);
    setSym(v, r);
    return r;
  }

  if (const auto *ev = dyn_cast<ExtractValueInst>(v)) {
    if (ev->getNumIndices() == 1) {
      const unsigned idx = *ev->idx_begin();
      if (const auto *woi =
              dyn_cast<WithOverflowInst>(ev->getAggregateOperand())) {
        z3::expr res = ctx.bv_val(
            0, woi->getArgOperand(0)->getType()->getIntegerBitWidth());
        z3::expr ov = ctx.bool_val(false);
        if (computeWithOverflow(
                woi, m_solver.value(),
                [&](const llvm::Value *x) { return getIntExpr(x, cur, pred); },
                res, ov)) {
          if (idx == 0) {
            setSym(v, res);
            return res;
          }
          if (idx == 1) {
            auto b = boolToBv1(ov);
            setSym(v, b);
            return b;
          }
        }
      }
    }
  }

  if (const auto *pti = dyn_cast<PtrToIntInst>(v)) {
    auto p = getPtrExpr(pti->getOperand(0), cur, pred);
    const unsigned bw = pti->getType()->getIntegerBitWidth();
    if (bw < m_ptr_bits)
      p = p.extract(bw - 1, 0);
    else if (bw > m_ptr_bits)
      p = z3::zext(p, bw - m_ptr_bits);
    setSym(v, p);
    return p;
  }

  if (const auto *itp = dyn_cast<IntToPtrInst>(v)) {
    auto i = getIntExpr(itp->getOperand(0), cur, pred);
    const unsigned ibw = i.get_sort().bv_size();
    if (ibw < m_ptr_bits)
      i = z3::zext(i, m_ptr_bits - ibw);
    else if (ibw > m_ptr_bits)
      i = i.extract(m_ptr_bits - 1, 0);
    // IntToPtr result is a pointer, not int; fall back to fresh int symbol.
  }

  if (const auto *cast = dyn_cast<CastInst>(v)) {
    Value *operand = cast->getOperand(0);
    if (operand->getType()->isIntegerTy() && cast->getType()->isIntegerTy()) {
      auto in = getIntExpr(operand, cur, pred);
      const unsigned inBw = in.get_sort().bv_size();
      const unsigned outBw = cast->getType()->getIntegerBitWidth();
      z3::expr out = in;
      if (outBw < inBw)
        out = out.extract(outBw - 1, 0);
      else if (outBw > inBw) {
        if (isa<SExtInst>(cast))
          out = z3::sext(out, outBw - inBw);
        else
          out = z3::zext(out, outBw - inBw);
      }
      setSym(v, out);
      return out;
    }
  }

  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    if (sel->getType()->isIntegerTy()) {
      auto c = getIntExpr(sel->getCondition(), cur, pred);
      auto t = getIntExpr(sel->getTrueValue(), cur, pred);
      auto f = getIntExpr(sel->getFalseValue(), cur, pred);
      z3::expr condBool = (c == ctx.bv_val(1, 1));
      z3::expr r = z3::ite(condBool, t, f);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *bin = dyn_cast<BinaryOperator>(v)) {
    if (bin->getType()->isIntegerTy()) {
      const auto lhs = getIntExpr(bin->getOperand(0), cur, pred);
      const auto rhs = getIntExpr(bin->getOperand(1), cur, pred);
      z3::expr result = lhs;
      switch (bin->getOpcode()) {
      case Instruction::Add:
        result = lhs + rhs;
        break;
      case Instruction::Sub:
        result = lhs - rhs;
        break;
      case Instruction::Mul:
        result = lhs * rhs;
        break;
      case Instruction::UDiv:
        result = z3::udiv(lhs, rhs);
        break;
      case Instruction::SDiv:
        result = lhs / rhs;
        break;
      case Instruction::URem:
        result = z3::urem(lhs, rhs);
        break;
      case Instruction::SRem:
        result = z3::srem(lhs, rhs);
        break;
      case Instruction::Shl:
        result = z3::shl(lhs, rhs);
        break;
      case Instruction::LShr:
        result = z3::lshr(lhs, rhs);
        break;
      case Instruction::AShr:
        result = z3::ashr(lhs, rhs);
        break;
      case Instruction::And:
        result = lhs & rhs;
        break;
      case Instruction::Or:
        result = lhs | rhs;
        break;
      case Instruction::Xor:
        result = lhs ^ rhs;
        break;
      default:
        result = lhs;
        break;
      }
      setSym(v, result);
      return result;
    }
  }

  if (const auto *icmp = dyn_cast<ICmpInst>(v)) {
    auto *lhs = icmp->getOperand(0);
    auto *rhs = icmp->getOperand(1);
    z3::expr condBool = ctx.bool_val(true);
    if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
      const auto l = getIntExpr(lhs, cur, pred);
      const auto r = getIntExpr(rhs, cur, pred);
      switch (icmp->getPredicate()) {
      case ICmpInst::ICMP_EQ:
        condBool = (l == r);
        break;
      case ICmpInst::ICMP_NE:
        condBool = (l != r);
        break;
      case ICmpInst::ICMP_SGT:
        condBool = z3::sgt(l, r);
        break;
      case ICmpInst::ICMP_SGE:
        condBool = z3::sge(l, r);
        break;
      case ICmpInst::ICMP_SLT:
        condBool = z3::slt(l, r);
        break;
      case ICmpInst::ICMP_SLE:
        condBool = z3::sle(l, r);
        break;
      case ICmpInst::ICMP_UGT:
        condBool = z3::ugt(l, r);
        break;
      case ICmpInst::ICMP_UGE:
        condBool = z3::uge(l, r);
        break;
      case ICmpInst::ICMP_ULT:
        condBool = z3::ult(l, r);
        break;
      case ICmpInst::ICMP_ULE:
        condBool = z3::ule(l, r);
        break;
      default:
        break;
      }
    } else if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
      const auto l = getPtrExpr(lhs, cur, pred);
      const auto r = getPtrExpr(rhs, cur, pred);
      switch (icmp->getPredicate()) {
      case ICmpInst::ICMP_EQ:
        condBool = (l == r);
        break;
      case ICmpInst::ICMP_NE:
        condBool = (l != r);
        break;
      default:
        break;
      }
    }
    auto bv = z3::ite(condBool, ctx.bv_val(1, 1), ctx.bv_val(0, 1));
    setSym(v, bv);
    return bv;
  }

  if (const auto *phi = dyn_cast<PHINode>(v)) {
    // Ideally resolved on block entry. If not, keep it symbolic.
    const std::string name = "%phi." + std::to_string((uintptr_t)phi);
    auto r = ctx.bv_const(name.c_str(), phi->getType()->getIntegerBitWidth());
    setSym(v, r);
    return r;
  }

  if (const auto *call = dyn_cast<CallInst>(v)) {
    if (call->getType()->isIntegerTy()) {
      const std::string name = "%call." + std::to_string((uintptr_t)call);
      auto r =
          ctx.bv_const(name.c_str(), call->getType()->getIntegerBitWidth());
      setSym(v, r);
      bool unknown_call = true;
      if (auto *callee = call->getCalledFunction()) {
        if (!callee->isDeclaration()) {
          unknown_call = false;
        }
      }
      if (unknown_call) {
        if (m_robust_reachability) {
          registerUniversal(r);
        }
      }
      return r;
    }
  }

  // Default: fresh int.
  const unsigned bw = v->getType()->getIntegerBitWidth();
  const std::string name = "%int." + std::to_string((uintptr_t)v);
  auto r = ctx.bv_const(name.c_str(), bw);
  setSym(v, r);
  return r;
}

z3::expr MKintPass::gepOffsetBytes(const GetElementPtrInst *gep,
                                   BasicBlock *cur, BasicBlock *pred) {
  auto &ctx = m_solver.value().ctx();
  if (!gep || !m_dl)
    return ctx.bv_val(0, m_ptr_bits);

  // Fast path: all-constant GEP.
  APInt constOff(m_ptr_bits, 0);
  if (gep->accumulateConstantOffset(*m_dl, constOff)) {
    return ctx.bv_val(constOff.getZExtValue(), m_ptr_bits);
  }

  z3::expr off = ctx.bv_val(0, m_ptr_bits);
  Type *ty = gep->getSourceElementType();
  unsigned idxNo = 0;
  for (const auto *idxIt = gep->idx_begin(); idxIt != gep->idx_end();
       ++idxIt, ++idxNo) {
    Value *idxV = idxIt->get();
    if (!idxV)
      continue;

    if (auto *st = dyn_cast<StructType>(ty)) {
      auto *ci = dyn_cast<ConstantInt>(idxV);
      if (!ci) {
        // Non-constant struct indices are not supported in LLVM IR, but be
        // defensive.
        const std::string name =
            "%gep.structidx." + std::to_string((uintptr_t)gep);
        return ctx.bv_const(name.c_str(), m_ptr_bits);
      }
      const unsigned field = static_cast<unsigned>(ci->getZExtValue());
      const auto *layout = m_dl->getStructLayout(st);
      off = off + ctx.bv_val(layout->getElementOffset(field), m_ptr_bits);
      ty = st->getElementType(field);
      continue;
    }

    uint64_t elemBytes = 0;
    if (auto *at = dyn_cast<ArrayType>(ty)) {
      elemBytes = m_dl->getTypeAllocSize(at->getElementType());
      ty = at->getElementType();
    } else {
      // First index on a scalar pointer: step by the source element size.
      elemBytes = m_dl->getTypeAllocSize(ty);
    }

    z3::expr idx = getIntExpr(idxV, cur, pred);
    const unsigned ibw = idx.get_sort().bv_size();
    if (ibw < m_ptr_bits)
      idx = z3::sext(idx, m_ptr_bits - ibw);
    else if (ibw > m_ptr_bits)
      idx = idx.extract(m_ptr_bits - 1, 0);
    off = off + (idx * ctx.bv_val(elemBytes, m_ptr_bits));
  }

  return off;
}

z3::expr MKintPass::getPtrExpr(const Value *v, BasicBlock *cur,
                               BasicBlock *pred) {
  auto it = m_v2sym.find(v);
  if (it != m_v2sym.end())
    return it->second.value();

  auto &ctx = m_solver.value().ctx();

  if (isa<ConstantPointerNull>(v)) {
    return ctx.bv_val(0, m_ptr_bits);
  }

  if (const auto *gv = dyn_cast<GlobalVariable>(v)) {
    if (!m_obj_base.count(gv)) {
      const uint64_t bytes = m_dl->getTypeAllocSize(gv->getValueType());
      ensureObject(gv, ("global." + gv->getName()).str(),
                   ctx.bv_val(bytes, m_ptr_bits), true);
    }
    setSym(v, m_obj_base[gv].value());
    return m_obj_base[gv].value();
  }

  if (const auto *fr = dyn_cast<FreezeInst>(v)) {
    auto r = getPtrExpr(fr->getOperand(0), cur, pred);
    setSym(v, r);
    return r;
  }

  if (const auto *ai = dyn_cast<AllocaInst>(v)) {
    if (!m_obj_base.count(ai)) {
      const uint64_t elemBytes = m_dl->getTypeAllocSize(ai->getAllocatedType());
      z3::expr sizeBytesExpr = ctx.bv_val(elemBytes, m_ptr_bits);
      bool known = true;
      if (ai->isArrayAllocation()) {
        if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
          sizeBytesExpr =
              ctx.bv_val(elemBytes * ci->getZExtValue(), m_ptr_bits);
        } else {
          auto countExpr = getIntExpr(ai->getArraySize(), cur, pred);
          const unsigned cbw = countExpr.get_sort().bv_size();
          if (cbw < m_ptr_bits)
            countExpr = z3::zext(countExpr, m_ptr_bits - cbw);
          else if (cbw > m_ptr_bits)
            countExpr = countExpr.extract(m_ptr_bits - 1, 0);
          sizeBytesExpr = countExpr * ctx.bv_val(elemBytes, m_ptr_bits);
          known = countExpr.is_numeral();
        }
      }
      ensureObject(ai,
                   ("alloca." + ai->getFunction()->getName().str() + "." +
                    std::to_string((uintptr_t)ai)),
                   sizeBytesExpr, known);
    }
    setSym(v, m_obj_base[ai].value());
    return m_obj_base[ai].value();
  }

  if (const auto *arg = dyn_cast<Argument>(v)) {
    if (arg->getType()->isPointerTy()) {
      const std::string name = (arg->getParent()->getName() + ".argptr" +
                                std::to_string(arg->getArgNo()))
                                   .str();
      auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *gep = dyn_cast<GetElementPtrInst>(v)) {
    auto base = getPtrExpr(gep->getPointerOperand(), cur, pred);
    auto off = gepOffsetBytes(gep, cur, pred);
    auto r = base + off;
    setSym(v, r);
    return r;
  }

  if (const auto *bc = dyn_cast<BitCastInst>(v)) {
    auto r = getPtrExpr(bc->getOperand(0), cur, pred);
    setSym(v, r);
    return r;
  }

  if (const auto *itp = dyn_cast<IntToPtrInst>(v)) {
    auto i = getIntExpr(itp->getOperand(0), cur, pred);
    const unsigned ibw = i.get_sort().bv_size();
    if (ibw < m_ptr_bits)
      i = z3::zext(i, m_ptr_bits - ibw);
    else if (ibw > m_ptr_bits)
      i = i.extract(m_ptr_bits - 1, 0);
    setSym(v, i);
    return i;
  }

  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    if (sel->getType()->isPointerTy()) {
      auto c = getIntExpr(sel->getCondition(), cur, pred);
      auto t = getPtrExpr(sel->getTrueValue(), cur, pred);
      auto f = getPtrExpr(sel->getFalseValue(), cur, pred);
      z3::expr condBool = (c == ctx.bv_val(1, 1));
      auto r = z3::ite(condBool, t, f);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *phi = dyn_cast<PHINode>(v)) {
    const std::string name = "%phi.ptr." + std::to_string((uintptr_t)phi);
    auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
    setSym(v, r);
    return r;
  }

  if (const auto *call = dyn_cast<CallInst>(v)) {
    if (call->getType()->isPointerTy()) {
      const std::string name = "%call.ptr." + std::to_string((uintptr_t)call);
      auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
      setSym(v, r);
      return r;
    }
  }

  const std::string name = "%ptr." + std::to_string((uintptr_t)v);
  auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
  setSym(v, r);
  return r;
}

} // namespace kint
