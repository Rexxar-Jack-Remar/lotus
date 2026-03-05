#ifndef CHECKER_GVFA_CHECKERUTILS_H
#define CHECKER_GVFA_CHECKERUTILS_H

#include <functional>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace CheckerUtils {

/**
 * Iterates over all instructions in the module.
 */
void forEachInstruction(Module *M,
                        std::function<void(const Instruction *)> func);

/**
 * Checks if a call instruction is a memory allocation (malloc, calloc, new,
 * etc.).
 */
bool isMemoryAllocation(const CallInst *CI);

/**
 * Checks if a call instruction is a memory deallocation (free, delete, etc.).
 */
bool isMemoryDeallocation(const CallInst *CI);

/**
 * Checks if a library function is known to dereference a specific argument.
 */
bool doesLibFunctionDereferenceArg(StringRef FuncName, unsigned ArgIdx);

/**
 * Checks if a function is a known initialization/sanitization function
 * (memset, bzero, etc.) that makes data "initialized" or "safe".
 */
bool isInitializationFunction(StringRef FuncName);

/**
 * Checks if a function is safe to pass stack addresses to (e.g., printf).
 * These functions are assumed not to capture the pointer.
 */
bool isSafeStackCaptureFunction(StringRef FuncName);

} // namespace CheckerUtils

#endif // CHECKER_GVFA_CHECKERUTILS_H
