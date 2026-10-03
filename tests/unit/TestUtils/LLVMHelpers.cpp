#include "TestUtils/LLVMHelpers.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace unittest {

std::unique_ptr<Module> parseModule(LLVMContext &context, const char *source,
                                    StringRef diag_owner) {
  SMDiagnostic err;
  auto module = parseAssemblyString(source, err, context);
  if (!module) {
    err.print(diag_owner.data(), errs());
  }
  return module;
}

std::unique_ptr<Module> parseModule(LLVMContext &context,
                                    const std::string &source,
                                    StringRef diag_owner) {
  return parseModule(context, source.c_str(), diag_owner);
}

std::unique_ptr<Module> parseModuleChecked(LLVMContext &context,
                                           const char *source,
                                           StringRef diag_owner) {
  auto module = parseModule(context, source, diag_owner);
  EXPECT_NE(module, nullptr);
  return module;
}

std::unique_ptr<Module> parseModuleChecked(LLVMContext &context,
                                           const std::string &source,
                                           StringRef diag_owner) {
  return parseModuleChecked(context, source.c_str(), diag_owner);
}

std::unique_ptr<Module> parseAssembly(LLVMContext &context, const char *source,
                                      StringRef diag_owner) {
  return parseModule(context, source, diag_owner);
}

std::unique_ptr<Module> parseAssembly(LLVMContext &context,
                                      const std::string &source,
                                      StringRef diag_owner) {
  return parseModule(context, source, diag_owner);
}

std::unique_ptr<Module> parseAssemblyChecked(LLVMContext &context,
                                             const char *source,
                                             StringRef diag_owner) {
  return parseModuleChecked(context, source, diag_owner);
}

std::unique_ptr<Module> parseAssemblyChecked(LLVMContext &context,
                                             const std::string &source,
                                             StringRef diag_owner) {
  return parseModuleChecked(context, source, diag_owner);
}

std::unique_ptr<Module> loadModule(StringRef path, LLVMContext &context,
                                   StringRef diag_owner) {
  SMDiagnostic err;
  auto module = parseIRFile(path, err, context);
  if (!module) {
    err.print(diag_owner.data(), errs());
  }
  return module;
}

std::unique_ptr<Module> loadModule(const std::string &path,
                                   LLVMContext &context, StringRef diag_owner) {
  return loadModule(StringRef(path), context, diag_owner);
}

Instruction *findInstructionByName(Function &func, StringRef name) {
  for (auto &bb : func) {
    for (auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

Instruction *findInstructionByName(Function *func, StringRef name) {
  return func ? findInstructionByName(*func, name) : nullptr;
}

const Instruction *findInstructionByName(const Function &func, StringRef name) {
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

const Instruction *findInstructionByName(const Function *func, StringRef name) {
  return func ? findInstructionByName(*func, name) : nullptr;
}

Instruction *findInst(Function &func, StringRef name) {
  return findInstructionByName(func, name);
}

Instruction *findInst(Function *func, StringRef name) {
  return findInstructionByName(func, name);
}

std::vector<CallBase *> findCallsTo(Function &func, StringRef callee_name) {
  std::vector<CallBase *> calls;
  for (auto &bb : func) {
    for (auto &inst : bb) {
      auto *call = dyn_cast<CallBase>(&inst);
      if (!call || !call->getCalledFunction()) {
        continue;
      }
      if (call->getCalledFunction()->getName() == callee_name) {
        calls.push_back(call);
      }
    }
  }
  return calls;
}

std::vector<CallBase *> findCallsTo(Function *func, StringRef callee_name) {
  return func ? findCallsTo(*func, callee_name) : std::vector<CallBase *>{};
}

std::vector<const CallBase *> findCallsTo(const Function &func,
                                          StringRef callee_name) {
  std::vector<const CallBase *> calls;
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      auto *call = dyn_cast<CallBase>(&inst);
      if (!call || !call->getCalledFunction()) {
        continue;
      }
      if (call->getCalledFunction()->getName() == callee_name) {
        calls.push_back(call);
      }
    }
  }
  return calls;
}

std::vector<const CallBase *> findCallsTo(const Function *func,
                                          StringRef callee_name) {
  return func ? findCallsTo(*func, callee_name)
              : std::vector<const CallBase *>{};
}

CallBase *findCallTo(Function &func, StringRef callee_name) {
  auto calls = findCallsTo(func, callee_name);
  return calls.empty() ? nullptr : calls.front();
}

CallBase *findCallTo(Function *func, StringRef callee_name) {
  return func ? findCallTo(*func, callee_name) : nullptr;
}

const CallBase *findCallTo(const Function &func, StringRef callee_name) {
  auto calls = findCallsTo(func, callee_name);
  return calls.empty() ? nullptr : calls.front();
}

const CallBase *findCallTo(const Function *func, StringRef callee_name) {
  return func ? findCallTo(*func, callee_name) : nullptr;
}

std::vector<CallBase *> getIndirectCalls(Function &func) {
  std::vector<CallBase *> calls;
  for (Instruction &inst : instructions(func)) {
    auto *call = dyn_cast<CallBase>(&inst);
    if (call && call->isIndirectCall()) {
      calls.push_back(call);
    }
  }
  return calls;
}

std::vector<const CallBase *> getIndirectCalls(const Function &func) {
  std::vector<const CallBase *> calls;
  for (const Instruction &inst : instructions(func)) {
    auto *call = dyn_cast<CallBase>(&inst);
    if (call && call->isIndirectCall()) {
      calls.push_back(call);
    }
  }
  return calls;
}

CallBase *findIndirectCall(Function &func) {
  auto calls = getIndirectCalls(func);
  return calls.empty() ? nullptr : calls.front();
}

CallBase *findIndirectCall(Function *func) {
  return func ? findIndirectCall(*func) : nullptr;
}

const CallBase *findIndirectCall(const Function &func) {
  auto calls = getIndirectCalls(func);
  return calls.empty() ? nullptr : calls.front();
}

const CallBase *findIndirectCall(const Function *func) {
  return func ? findIndirectCall(*func) : nullptr;
}

const BasicBlock *findBasicBlockByName(const Function &func, StringRef name) {
  for (const auto &bb : func) {
    if (bb.getName() == name) {
      return &bb;
    }
  }
  return nullptr;
}

BasicBlock *findBasicBlockByName(Function &func, StringRef name) {
  for (auto &bb : func) {
    if (bb.getName() == name) {
      return &bb;
    }
  }
  return nullptr;
}

BasicBlock *findBlock(Function &func, StringRef name) {
  return findBasicBlockByName(func, name);
}

BasicBlock *findBlock(Function *func, StringRef name) {
  return func ? findBasicBlockByName(*func, name) : nullptr;
}

const BasicBlock *findBlock(const Function &func, StringRef name) {
  return findBasicBlockByName(func, name);
}

const BasicBlock *findBlock(const Function *func, StringRef name) {
  return func ? findBasicBlockByName(*func, name) : nullptr;
}

PHINode *findPhi(BasicBlock &block, StringRef name) {
  for (auto &phi : block.phis()) {
    if (phi.getName() == name) {
      return &phi;
    }
  }
  return nullptr;
}

const PHINode *findPhi(const BasicBlock &block, StringRef name) {
  for (const auto &phi : block.phis()) {
    if (phi.getName() == name) {
      return &phi;
    }
  }
  return nullptr;
}

PHINode *findPhi(Function &func, StringRef name) {
  for (auto &bb : func) {
    for (auto &phi : bb.phis()) {
      if (phi.getName() == name) {
        return &phi;
      }
    }
  }
  return nullptr;
}

PHINode *findPhi(Function *func, StringRef name) {
  return func ? findPhi(*func, name) : nullptr;
}

const PHINode *findPhi(const Function &func, StringRef name) {
  for (const auto &bb : func) {
    for (const auto &phi : bb.phis()) {
      if (phi.getName() == name) {
        return &phi;
      }
    }
  }
  return nullptr;
}

const PHINode *findPhi(const Function *func, StringRef name) {
  return func ? findPhi(*func, name) : nullptr;
}

const Instruction *getFirstInstruction(const Function &func) {
  if (func.empty()) {
    return nullptr;
  }
  return &func.getEntryBlock().front();
}

Instruction *getFirstInstruction(Function &func) {
  if (func.empty()) {
    return nullptr;
  }
  return &func.getEntryBlock().front();
}

Function *findFunctionByName(Module &module, StringRef name) {
  return module.getFunction(name);
}

const Function *findFunctionByName(const Module &module, StringRef name) {
  return module.getFunction(name);
}

Function *getFunctionChecked(Module &module, StringRef name) {
  auto *function = findFunctionByName(module, name);
  EXPECT_NE(function, nullptr) << "missing function: " << name.str();
  return function;
}

const Function *getFunctionChecked(const Module &module, StringRef name) {
  auto *function = findFunctionByName(module, name);
  EXPECT_NE(function, nullptr) << "missing function: " << name.str();
  return function;
}

BasicBlock *getBlockChecked(Function &func, StringRef name) {
  auto *block = findBlock(func, name);
  EXPECT_NE(block, nullptr) << "missing block: " << name.str();
  return block;
}

const BasicBlock *getBlockChecked(const Function &func, StringRef name) {
  auto *block = findBlock(func, name);
  EXPECT_NE(block, nullptr) << "missing block: " << name.str();
  return block;
}

std::unique_ptr<Module> LlvmModuleTest::parseModule(const char *source) {
  return unittest::parseModule(context, source, "LlvmModuleTest");
}

std::unique_ptr<Module> LlvmModuleTest::parseModule(const std::string &source) {
  return unittest::parseModule(context, source, "LlvmModuleTest");
}

bool LlvmModuleTest::loadModule(const char *ir) {
  SMDiagnostic error;
  auto parsed = parseIR(MemoryBuffer::getMemBuffer(ir)->getMemBufferRef(),
                        error, context);
  if (!parsed) {
    error.print("LlvmModuleTest", errs());
    return false;
  }
  module = std::move(parsed);
  return true;
}

} // namespace unittest
} // namespace lotus
