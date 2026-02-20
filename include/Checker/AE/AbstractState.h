//===- AbstractState.h ----Abstract State--------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractValue.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

// Forward declaration (SVFIRWrapper only needs pointer)
class SVFIRWrapper;

// Maximum field limit for GEP offset calculations (default: 10000)
[[maybe_unused]] static constexpr uint32_t MaxFieldLimit = 10000;

/// AbstractState - maps variables and memory locations to abstract values
class AbstractState {
public:
  typedef std::unordered_map<uint32_t, AbstractValue> VarToAbsValMap;
  typedef VarToAbsValMap AddrToAbsValMap;

  static constexpr uint32_t NullPtr = 0;
  static constexpr uint32_t BlkPtr = 1;

  std::unordered_set<uint32_t> _freedAddrs;
  VarToAbsValMap _varToAbsVal;
  AddrToAbsValMap _addrToAbsVal;

  SVFIRWrapper *svfir_ = nullptr;

  AbstractState() {}

  AbstractState(VarToAbsValMap &_varToValMap, AddrToAbsValMap &_locToValMap)
      : _varToAbsVal(_varToValMap), _addrToAbsVal(_locToValMap) {}

  AbstractState(const AbstractState &rhs)
      : _freedAddrs(rhs._freedAddrs), _varToAbsVal(rhs._varToAbsVal),
        _addrToAbsVal(rhs._addrToAbsVal), svfir_(rhs.svfir_),
        _objToSize(rhs._objToSize) {}

  virtual ~AbstractState() = default;

  AbstractState &operator=(const AbstractState &rhs) {
    if (rhs != *this) {
      _varToAbsVal = rhs._varToAbsVal;
      _addrToAbsVal = rhs._addrToAbsVal;
      _freedAddrs = rhs._freedAddrs;
      _objToSize = rhs._objToSize;
      svfir_ = rhs.svfir_;
    }
    return *this;
  }

  AbstractState(AbstractState &&rhs)
      : _freedAddrs(std::move(rhs._freedAddrs)),
        _varToAbsVal(std::move(rhs._varToAbsVal)),
        _addrToAbsVal(std::move(rhs._addrToAbsVal)), svfir_(rhs.svfir_),
        _objToSize(std::move(rhs._objToSize)) {}

  AbstractState &operator=(AbstractState &&rhs) {
    if (&rhs != this) {
      _varToAbsVal = std::move(rhs._varToAbsVal);
      _addrToAbsVal = std::move(rhs._addrToAbsVal);
      _freedAddrs = std::move(rhs._freedAddrs);
      _objToSize = std::move(rhs._objToSize);
      svfir_ = rhs.svfir_;
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
    if (!AddressValue::isVirtualMemAddress(addr))
      return;
    uint32_t objId = getIDFromAddr(addr);
    if (isNullMem(addr))
      return;
    // Check for freed memory before storing
    if (isFreedMem(addr))
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

  static bool geqFreedAddrs(const std::unordered_set<uint32_t> &lhs,
                            const std::unordered_set<uint32_t> &rhs) {
    for (uint32_t addr : rhs) {
      if (lhs.find(addr) == lhs.end())
        return false;
    }
    return true;
  }

  static bool geqObjSizeMap(const std::unordered_map<uint32_t, uint32_t> &lhs,
                            const std::unordered_map<uint32_t, uint32_t> &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &item : rhs) {
      auto it = lhs.find(item.first);
      if (it == lhs.end())
        return false;
      // Treat object size as exact metadata for lattice-order checks.
      if (it->second != item.second)
        return false;
    }
    return true;
  }

  bool operator==(const AbstractState &rhs) const {
    return eqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
           eqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
           _freedAddrs == rhs._freedAddrs && _objToSize == rhs._objToSize;
  }

  bool operator!=(const AbstractState &rhs) const { return !(*this == rhs); }

  bool operator<(const AbstractState &rhs) const { return !(*this >= rhs); }

  bool operator>=(const AbstractState &rhs) const {
    return geqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
           geqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
           geqFreedAddrs(_freedAddrs, rhs._freedAddrs) &&
           geqObjSizeMap(_objToSize, rhs._objToSize);
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
  uint32_t getAllocaInstByteSize(const llvm::AllocaInst *alloca,
                                 const AbstractState &as);

  AddressValue getGepObjAddrs(uint32_t pointer, IntervalValue offset,
                              const llvm::GetElementPtrInst *gep);
  uint32_t getGepFieldSize(llvm::Type *srcType, int64_t offset,
                           const llvm::DataLayout &dl);

  // Object initialization
  void initObjVar(const llvm::Value *objVar);

  // Pointer to SVFIRWrapper for PTA-based queries
  void setSVFIRWrapper(SVFIRWrapper *wrapper) { svfir_ = wrapper; }
  SVFIRWrapper *getSVFIRWrapper() const { return svfir_; }

  // Type queries using PTA
  const llvm::Type *getPointeeElement(uint32_t id);

  // Get object size using PTA
  uint32_t getObjectSize(const llvm::Value *obj) const;

  // Get points-to set for a pointer using PTA
  // Returns vector of objects (as void* to avoid template in header)
  void getPointsToSet(const llvm::Value *ptr,
                      std::vector<void *> &result) const;

  // Object size tracking (public for AE use)
  void setObjSize(uint32_t objId, uint32_t size) { _objToSize[objId] = size; }
  uint32_t getObjSize(uint32_t objId) const {
    auto it = _objToSize.find(objId);
    return it != _objToSize.end() ? it->second : 0;
  }

private:
  // Object size tracking (storage)
  std::unordered_map<uint32_t, uint32_t> _objToSize;

  // GEP object offset tracking (similar to SVF's GepObjVar)
  // Maps GEP instruction pointer -> offset from base object
  std::unordered_map<const llvm::GetElementPtrInst *, IntervalValue>
      _gepObjOffsetFromBase;

  // GEP field-sensitive object tracking: maps (baseObjId, offset) -> field
  // object ID This maintains field sensitivity similar to SVF's getGepObjVar()
  std::map<std::pair<uint32_t, int64_t>, uint32_t> _gepFieldObjMap;
  uint32_t _nextGepFieldId =
      0x80000000; // Start from high IDs to avoid collision

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

  // Get or create a field-sensitive GEP object ID (matching SVF's getGepObjVar)
  uint32_t getGepFieldObjId(uint32_t baseObjId, int64_t offset) {
    auto key = std::make_pair(baseObjId, offset);
    auto it = _gepFieldObjMap.find(key);
    if (it != _gepFieldObjMap.end()) {
      return it->second;
    }
    uint32_t newId = _nextGepFieldId++;
    _gepFieldObjMap[key] = newId;
    return newId;
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
};

} // namespace analysis
} // namespace lotus
