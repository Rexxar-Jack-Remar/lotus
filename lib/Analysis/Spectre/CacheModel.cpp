// from https://github.com/DanielGuoVT/pldi_code
// ===- Branch.cpp - Transform branches dependent on secrete data
// ---------------===//
//   - 10/04 treat all input as sensitive
//
//===---------------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include "Analysis/Spectre/CacheSpecuAnalysis.h"

#include <deque>
using namespace llvm;
using namespace std;
#define DEBUG_TYPE "spectre"
namespace spectre {

void CacheModel::SetAges(vector<unsigned> ages) { this->Ages = ages; }

void CacheModel::SetVarsMap(ValueMap<Value *, Var *> &vars) {
  for (const auto &i : vars)
    this->Vars[i.first] = i.second;
}

CacheModel::CacheModel(unsigned lineSize, unsigned lineNum, unsigned setNum,
                       bool must) {
  auto isPowerOf2 = [](unsigned x) -> bool { return (x & (x - 1)) == 0; };
  if (!(isPowerOf2(lineSize) && isPowerOf2(setNum)) && (lineNum % setNum == 0))
    errs() << "Fatal: cache configuration invalid!\n";

  CacheLineNum = lineNum;
  CacheLineSize = lineSize;
  CacheSetNum = setNum;
  CacheLinesPerSet = lineNum / setNum;
  MustMod = must;
  MaxAddr = 0;
  HitCount = MissCount = SpecuHitCount = SpecuMissCount = 0;
}

unsigned CacheModel::LocateVar(Value *var, unsigned offset) {
  if (offset == -1)
    offset = 0;
  if (this->Vars.find(var) == this->Vars.end())
    return -1;

  return (this->Vars[var]->AddrB + offset) / CacheLineSize;
}

unsigned CacheModel::GetAge(Value *var, unsigned offset) { return offset; }

unsigned CacheModel::SetAge(Value *var, unsigned age, unsigned offset) {
  return age;
}

unsigned CacheModel::Access(Value *var, bool force) {
  if (!force)
    return 1;
  // this is used to force access a variable, do not use in normal way.
  static int age = 0;
  for (int i = (this->Vars[var]->AgeIndex + this->Vars[var]->AgeSize) - 1;
       i >= (int)(this->Vars[var]->AgeIndex); --i) {
    Ages[i] = age;
    age++;
  }
  return 0;
}

unsigned CacheModel::AddVar(Value *var, Type *ty, unsigned alignment) {
  if (this->Vars.find(var) != this->Vars.end())
    return 1;

  unsigned size = GetTySize(ty);
  if (size == 0)
    return 1;
  if (alignment == 0)
    alignment = 1;
  Var *newVar = new Var{};
  ;

  if (MaxAddr % alignment)
    MaxAddr = (MaxAddr / alignment + 1) * alignment;

  newVar->AddrB = MaxAddr;
  newVar->AddrE = MaxAddr + size - 1;
  MaxAddr = MaxAddr + size;
  newVar->Val = var;
  newVar->ty = ty;
  newVar->alignment = alignment;

  unsigned lineB = newVar->AddrB / CacheLineSize;
  unsigned lineE = newVar->AddrE / CacheLineSize;

  newVar->AgeIndex = lineB;
  newVar->AgeSize = lineE - lineB + 1;
  newVar->LineB = lineB % CacheLineNum;
  newVar->LineE = lineE % CacheLineNum;
  while (Ages.size() < lineE + 1) {
    Ages.push_back(CacheLinesPerSet);
  }

  this->Vars[var] = newVar;
  return 0;
}

unsigned CacheModel::Access(Value *var, unsigned offset) {
  if (this->Vars.find(var) == this->Vars.end()) {
    //		errs() << "Fatal: try to access variable not been added!\n";
    return 1;
  }

  // if offset is unknown, chose the line uncached if possible
  unsigned CacheLoc = -1;
  if (offset == -1)
    offset = 0;

  if (offset == -1) {
    for (int i = this->Vars[var]->AgeIndex;
         i < (this->Vars[var]->AgeIndex + this->Vars[var]->AgeSize); i++) {
      if (Ages[i] >= CacheLinesPerSet) {
        CacheLoc = i;
        break;
      }
    }
    if (CacheLoc == (unsigned)-1)
      CacheLoc = this->Vars[var]->AddrB / CacheLineSize;
  } else {
    unsigned addr = this->Vars[var]->AddrB + offset;
    if (addr > this->Vars[var]->AddrE) {
      //			errs() << "Fatal: try to access variable offset
      // out of bound!\n";
      return 2;
    }
    CacheLoc = addr / CacheLineSize;
  }

  if (CacheLoc >= Ages.size()) {
    errs() << "Fatal error when locate variable in cache analysis:";
    var->dump();
    errs() << "@Function CacheModel::Access().\n";
    return -1;
  }

  unsigned age = Ages[CacheLoc];
  unsigned SetNum = CacheLoc % CacheSetNum;
  if (cacheRecord.find(CacheLoc) == cacheRecord.end())
    cacheRecord.insert(CacheLoc);

  for (unsigned i = SetNum; i < Ages.size(); i += CacheSetNum) {
    if (i == CacheLoc)
      Ages[CacheLoc] = 0;
    else if (Ages[i] <= age) {
      if (Ages[i] == age && MustMod)
        continue;
      if (Ages[i] < CacheLinesPerSet)
        Ages[i]++;
    }
  }

  // return 1, if var in cache, else 0. use return value to count cache
  // hits/miss
  return age < CacheLineNum ? 1 : 0;
}

CacheModel *CacheModel::fork() {
  CacheModel *ret = new CacheModel(this->CacheLineSize, this->CacheLineNum,
                                   this->CacheSetNum, this->MustMod);
  ret->SetMaxAddr(this->MaxAddr);
  ret->SetVarsMap(this->Vars);
  ret->SetAges(this->Ages);
  ret->HitCount = this->HitCount;
  ret->MissCount = this->MissCount;
  ret->SpecuHitCount = this->SpecuHitCount;
  ret->SpecuMissCount = this->SpecuMissCount;
  ret->cacheRecord = this->cacheRecord;

  return ret;
}

bool CacheModel::equal(CacheModel *model) {
  // TODO: implement
  return true;
}
CacheModel *CacheModel::merge(CacheModel *mod) {
  if (mod == nullptr)
    return this;
  if (MustMod) {
    for (const auto &var : mod->Vars) {
      Value *val = var.first;
      if (this->Vars.find(val) == this->Vars.end()) {
        this->AddVar(val, var.second->ty, var.second->alignment);
        for (int i = 0; i < mod->Vars[val]->AgeSize; i++) {
          //					unsigned modAge =
          // mod->Ages[mod->Vars[val]->AgeIndex+i];
          this->Ages[this->Vars[val]->AgeIndex + i] = CacheLinesPerSet;
        }
        continue;
      }

      for (int i = 0; i < mod->Vars[val]->AgeSize; i++) {
        unsigned modAge = mod->Ages[mod->Vars[val]->AgeIndex + i];
        unsigned thisAge = this->Ages[this->Vars[val]->AgeIndex + i];
        thisAge = thisAge > modAge ? thisAge : modAge;
        this->Ages[this->Vars[val]->AgeIndex + i] = thisAge;
      }
    }
    for (const auto &var : this->Vars) {
      Value *val = var.first;
      if (mod->Vars.find(val) == mod->Vars.end()) {
        for (int i = 0; i < this->Vars[val]->AgeSize; i++)
          this->Ages[this->Vars[val]->AgeIndex + i] = CacheLinesPerSet;
      }
    }
  }

  if (this->MissCount == mod->MissCount)
    this->SpecuMissCount = this->SpecuMissCount < mod->SpecuMissCount
                               ? this->SpecuMissCount
                               : mod->SpecuMissCount;
  else
    this->SpecuMissCount = this->MissCount > mod->MissCount
                               ? this->SpecuMissCount
                               : mod->SpecuMissCount;

  this->MissCount =
      this->MissCount > mod->MissCount ? this->MissCount : mod->MissCount;
  for (auto cr : mod->cacheRecord) {
    if (this->cacheRecord.find(cr) == this->cacheRecord.end())
      this->cacheRecord.insert(cr);
  }

  return this;
}

bool CacheModel::isInCache(string varName) {

  bool incache = false;
  for (const auto &v : this->Vars) {
    if (!isVarPartiallyCached(v.second))
      continue;
    Value *val = v.first;
    if (val->getName() == varName) {
      if (Ages[v.second->AgeIndex] < this->CacheLinesPerSet)
        incache = true;
    }
  }
  return incache;
}

void CacheModel::dump(bool verbose) {
  dbgs() << "========cache state===========\n";
  for (const auto &v : this->Vars) {
    if (!isVarPartiallyCached(v.second) && !verbose)
      continue;

    Value *val = v.first;

    if (val->hasName())
      dbgs() << val->getName() << "\n";
    else
      val->dump();

    if (verbose) {
      dbgs() << "\t addrB: " << v.second->AddrB
             << "\t addrE:" << v.second->AddrE
             << "\n\t ageIndex:" << v.second->AgeIndex
             << "\t ageSize:" << v.second->AgeSize
             << "\n\t align:" << v.second->alignment << "\n";
    }

    dbgs() << "\t occupy " << v.second->AgeSize << " lines at "
           << v.second->AddrB << " : ";

    for (int i = v.second->AgeIndex;
         i < (v.second->AgeIndex + v.second->AgeSize); ++i) {
      dbgs() << Ages[i] << ", ";
    }
    dbgs() << "\n";
  }
  dbgs() << "\nTotal #Misses: " << this->MissCount;
  dbgs() << "\t#SpecuMisses: " << this->SpecuMissCount;
  dbgs() << "\n===================\n";
}

} // namespace spectre
