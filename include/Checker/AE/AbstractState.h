//===- AbstractState.h ----Abstract State--------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractValue.h"

#include <set>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

// Maximum field limit for GEP offset calculations (default: 10000)
[[maybe_unused]] static constexpr uint32_t MaxFieldLimit = 10000;

/// AbstractState - maps variables and memory locations to abstract values
class AbstractState {
public:
  typedef std::unordered_map<uint32_t, AbstractValue> VarToAbsValMap;
  typedef VarToAbsValMap AddrToAbsValMap;

  std::unordered_set<uint32_t> _freedAddrs;

public:
  VarToAbsValMap _varToAbsVal;
  AddrToAbsValMap _addrToAbsVal;

public:
  AbstractState() {}

  AbstractState(VarToAbsValMap &_varToValMap, AddrToAbsValMap &_locToValMap)
      : _varToAbsVal(_varToValMap), _addrToAbsVal(_locToValMap) {}

  AbstractState(const AbstractState &rhs)
      : _freedAddrs(rhs._freedAddrs), _varToAbsVal(rhs._varToAbsVal),
        _addrToAbsVal(rhs._addrToAbsVal) {}

  virtual ~AbstractState() = default;

  AbstractState &operator=(const AbstractState &rhs) {
    if (rhs != *this) {
      _varToAbsVal = rhs._varToAbsVal;
      _addrToAbsVal = rhs._addrToAbsVal;
      _freedAddrs = rhs._freedAddrs;
    }
    return *this;
  }

  AbstractState(AbstractState &&rhs)
      : _varToAbsVal(std::move(rhs._varToAbsVal)),
        _addrToAbsVal(std::move(rhs._addrToAbsVal)) {}

  AbstractState &operator=(AbstractState &&rhs) {
    if (&rhs != this) {
      _varToAbsVal = std::move(rhs._varToAbsVal);
      _addrToAbsVal = std::move(rhs._addrToAbsVal);
      _freedAddrs = std::move(rhs._freedAddrs);
    }
    return *this;
  }

  AbstractValue &operator[](uint32_t varId) { return _varToAbsVal[varId]; }

  const AbstractValue &operator[](uint32_t varId) const {
    return _varToAbsVal.at(varId);
  }

  bool inVarToAddrsTable(uint32_t id) const {
    if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
      if (_varToAbsVal.at(id).isAddr()) {
        return true;
      }
    }
    return false;
  }

  bool inVarToValTable(uint32_t id) const {
    if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
      if (_varToAbsVal.at(id).isInterval()) {
        return true;
      }
    }
    return false;
  }

  bool inAddrToAddrsTable(uint32_t id) const {
    if (_addrToAbsVal.find(id) != _addrToAbsVal.end()) {
      if (_addrToAbsVal.at(id).isAddr()) {
        return true;
      }
    }
    return false;
  }

  bool inAddrToValTable(uint32_t id) const {
    if (_addrToAbsVal.find(id) != _addrToAbsVal.end()) {
      if (_addrToAbsVal.at(id).isInterval()) {
        return true;
      }
    }
    return false;
  }

  const VarToAbsValMap &getVarToVal() const { return _varToAbsVal; }
  const AddrToAbsValMap &getLocToVal() const { return _addrToAbsVal; }

  AbstractState widening(const AbstractState &other);
  AbstractState narrowing(const AbstractState &other);

  void joinWith(const AbstractState &other);
  void meetWith(const AbstractState &other);

  void addToFreedAddrs(uint32_t addr) { _freedAddrs.insert(addr); }

  bool isFreedMem(uint32_t addr) const {
    return _freedAddrs.find(addr) != _freedAddrs.end();
  }

  void store(uint32_t addr, const AbstractValue &val) {
    assert(AddressValue::isVirtualMemAddress(addr) && "not virtual address?");
    uint32_t objId = getIDFromAddr(addr);
    if (isNullMem(addr))
      return;
    _addrToAbsVal[objId] = val;
  }

  AbstractValue &load(uint32_t addr) {
    assert(AddressValue::isVirtualMemAddress(addr) && "not virtual address?");
    uint32_t objId = getIDFromAddr(addr);
    return _addrToAbsVal[objId];
  }

  void printAbstractState() const;
  std::string toString() const { return ""; }
  bool equals(const AbstractState &other) const;
  uint32_t hash() const;

  static bool eqVarToValMap(const VarToAbsValMap &lhs,
                            const VarToAbsValMap &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &item : lhs) {
      auto it = rhs.find(item.first);
      if (it == rhs.end())
        return false;
      if (!item.second.equals(it->second))
        return false;
    }
    return true;
  }

  static bool geqVarToValMap(const VarToAbsValMap &lhs,
                             const VarToAbsValMap &rhs) {
    if (rhs.empty())
      return true;
    for (const auto &item : rhs) {
      auto it = lhs.find(item.first);
      if (it == lhs.end())
        return false;
      if (!it->second.getInterval().contain(item.second.getInterval()))
        return false;
    }
    return true;
  }

  bool operator==(const AbstractState &rhs) const {
    return eqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
           eqVarToValMap(_addrToAbsVal, rhs.getLocToVal());
  }

  bool operator!=(const AbstractState &rhs) const { return !(*this == rhs); }

  bool operator<(const AbstractState &rhs) const { return !(*this >= rhs); }

  bool operator>=(const AbstractState &rhs) const {
    return geqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
           geqVarToValMap(_addrToAbsVal, rhs.getLocToVal());
  }

  void clear() {
    _addrToAbsVal.clear();
    _varToAbsVal.clear();
    _freedAddrs.clear();
  }

  AbstractState bottom() const {
    AbstractState inv = *this;
    for (auto &item : inv._varToAbsVal) {
      if (item.second.isInterval())
        item.second.getInterval().set_to_bottom();
    }
    return inv;
  }

  AbstractState top() const {
    AbstractState inv = *this;
    for (auto &item : inv._varToAbsVal) {
      if (item.second.isInterval())
        item.second.getInterval().set_to_top();
    }
    return inv;
  }

  static inline bool isNullMem(uint32_t addr) { return addr == NullMemAddr; }
  static inline bool isInvalidMem(uint32_t addr) {
    return addr == InvalidMemAddr;
  }

  static inline uint32_t getVirtualMemAddress(uint32_t idx) {
    return AddressValue::getVirtualMemAddress(idx);
  }

  static inline bool isVirtualMemAddress(uint32_t val) {
    return AddressValue::isVirtualMemAddress(val);
  }

  inline uint32_t getIDFromAddr(uint32_t addr) const {
    return _freedAddrs.count(addr) ? AddressValue::getInternalID(InvalidMemAddr)
                                   : AddressValue::getInternalID(addr);
  }

  AddressValue getGepObjAddrs(uint32_t pointer, IntervalValue offset);
  AbstractValue loadValue(uint32_t varId);
  void storeValue(uint32_t varId, uint32_t valId);

  // GEP offset computation
  IntervalValue getByteOffset(const llvm::GetElementPtrInst *gep);
  IntervalValue getElementIndex(const llvm::GetElementPtrInst *gep);
  uint32_t getAllocaInstByteSize(const llvm::AllocaInst *alloca);
  // Improved version that uses abstract state for VLA size tracking
  uint32_t getAllocaInstByteSize(const llvm::AllocaInst *alloca,
                                 const AbstractState &as);

  // Object initialization
  void initObjVar(const llvm::Value *objVar);

  // Type queries
  const llvm::Type *getPointeeElement(uint32_t id);

  // Object size tracking
  std::unordered_map<uint32_t, uint32_t> _objToSize;
  void setObjSize(uint32_t objId, uint32_t size) { _objToSize[objId] = size; }
  uint32_t getObjSize(uint32_t objId) const {
    auto it = _objToSize.find(objId);
    return it != _objToSize.end() ? it->second : 0;
  }

  // GEP object offset tracking (similar to SVF's GepObjVar)
  // Maps GEP instruction pointer -> offset from base object
  std::unordered_map<const llvm::GetElementPtrInst *, IntervalValue>
      _gepObjOffsetFromBase;
  void setGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep,
                               const IntervalValue &offset) {
    _gepObjOffsetFromBase[gep] = offset;
  }
  bool hasGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const {
    return _gepObjOffsetFromBase.find(gep) != _gepObjOffsetFromBase.end();
  }
  IntervalValue
  getGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const {
    auto it = _gepObjOffsetFromBase.find(gep);
    if (it != _gepObjOffsetFromBase.end()) {
      return it->second;
    }
    return IntervalValue(0, 0);
  }

  AbstractState sliceState(std::set<uint32_t> &sl) const {
    AbstractState inv;
    for (uint32_t id : sl) {
      if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
        inv._varToAbsVal[id] = _varToAbsVal.at(id);
      }
    }
    return inv;
  }

  static constexpr uint32_t NullPtr = 0;
  static constexpr uint32_t BlkPtr = 1;
};

} // namespace analysis
} // namespace lotus
