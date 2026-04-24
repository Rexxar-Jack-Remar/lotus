MemorySSA — Memory Static Single Assignment
============================================

Overview
========

The **Memory Static Single Assignment (MemorySSA)** is an intermediate representation
that extends SSA form to memory operations. It provides a structured way to track
memory def-use chains by working with shadow memory instructions inserted by the
Sea-DSA ShadowMem pass.

* **Location**: ``lib/IR/MemorySSA/``, ``include/IR/MemorySSA/``

MemorySSA operates on top of Sea-DSA's ShadowMem instrumentation, which inserts
special shadow function calls (e.g., ``shadow.mem.load``, ``shadow.mem.store``,
``shadow.mem.arg.ref``) into the LLVM IR. The MemorySSA infrastructure provides
APIs to extract and query memory def-use information from these shadow instructions,
enabling interprocedural memory analysis and optimizations.

Key Features
============

* **Shadow Instruction Parsing**: Extracts memory SSA information from Sea-DSA's
  shadow memory instructions
* **Interprocedural Memory Tracking**: Tracks memory regions across function boundaries
  through call sites and formal parameters
* **Def-Use Chain Analysis**: Provides APIs to query memory definitions and uses
  for program analysis
* **Call Site Analysis**: Analyzes memory regions accessed by callee functions
  at call sites (read-only, modified, read-and-modified, or newly created)
* **Formal Parameter Tracking**: Tracks memory regions corresponding to formal
  parameters of functions

Shadow Memory Instructions
==========================

MemorySSA works with shadow memory instructions inserted by Sea-DSA's ShadowMem pass.
These instructions provide information about memory operations:

**Intraprocedural Operations**:

* ``shadow.mem.load(NodeID, TLVar, SingletonGlobal)`` – Memory load (use)
* ``TLVar shadow.mem.store(NodeID, TLVar, SingletonGlobal)`` – Memory store (definition)
* ``TLVar shadow.mem.arg.init(NodeID, SingletonGlobal)`` – Initial value of formal
  parameter or global with unique scalar
* ``TLVar shadow.mem.global.init(NodeID, TLVar, Global)`` – Initial value of global

**Interprocedural Operations**:

* ``shadow.mem.arg.ref(NodeID, TLVar, Idx, SingletonGlobal)`` – Read-only input
  actual parameter
* ``TLVar shadow.mem.arg.mod(NodeID, TLVar, Idx, SingletonGlobal)`` – Modified
  input/output actual parameter
* ``TLVar shadow.mem.arg.ref_mod(NodeID, TLVar, Idx, SingletonGlobal)`` – Read-and-modified
  input/output actual parameter
* ``TLVar shadow.mem.arg.new(NodeID, TLVar, Idx, SingletonGlobal)`` – Output actual
  parameter (newly created)
* ``shadow.mem.in(NodeID, TLVar, Idx, SingletonGlobal)`` – Input formal parameter
* ``shadow.mem.out(NodeID, TLVar, Idx, SingletonGlobal)`` – Output formal parameter

Where:
* **NodeID**: Unique identifier (i32) for memory regions
* **TLVar**: Top-level variable (LLVM register) of pointer type
* **Idx**: Parameter index for matching actual and formal parameters
* **SingletonGlobal**: Indicates whether the memory region corresponds to a global
  variable whose address has not been taken

Components
==========

**MemorySSACallSite** (``MemorySSA.h``, ``MemorySSA.cpp``):

Represents a call site with its associated memory SSA information. It gathers all
shadow memory instructions related to actual parameters of a call instruction.

* **Constructor**: ``MemorySSACallSite(CallInst *ci, bool only_singleton)`` – Builds
  the call site representation by scanning backwards for shadow memory instructions
* **Query Methods**:
  * ``numParams()`` – Returns the number of memory-related actual parameters
  * ``isRef(idx)`` – Checks if the idx-th parameter is read-only
  * ``isMod(idx)`` – Checks if the idx-th parameter is modified
  * ``isRefMod(idx)`` – Checks if the idx-th parameter is read-and-modified
  * ``isNew(idx)`` – Checks if the idx-th parameter is newly created
  * ``getNonPrimed(idx)`` – Returns the non-primed (before update) variable
  * ``getPrimed(idx)`` – Returns the primed (after update) variable

**MemorySSAFunction** (``MemorySSA.h``, ``MemorySSA.cpp``):

Gathers memory SSA-related input/output formal parameters of a function.

* **Constructor**: ``MemorySSAFunction(Function &F, Pass &P, bool only_singleton)`` –
  Builds the function representation by scanning exit blocks for shadow.mem.in
  instructions
* **Query Methods**:
  * ``getInFormal(idx)`` – Returns the input formal parameter for the given index
    (returns null if not found)
  * ``getNumInFormals()`` – Returns the number of input formal parameters

**MemorySSACallsManager** (``MemorySSA.h``, ``MemorySSA.cpp``):

Manages memory SSA information for all functions and call sites in a module.

* **Constructor**: ``MemorySSACallsManager(Module &M, Pass &P, bool only_singleton)`` –
  Builds memory SSA information for all functions and call sites in the module
* **Query Methods**:
  * ``getFunction(Function *F)`` – Returns the MemorySSAFunction for the given function
  * ``getCallSite(CallInst *CI)`` – Returns the MemorySSACallSite for the given call
    instruction

**Utility Functions** (``MemorySSA.h``):

* ``MemSSAOp`` – Enumeration of shadow memory operation types
* ``MemSSAStrToOp(StringRef name)`` – Converts shadow function name to operation type
* ``isMemSSALoad(CallBase *CB, bool onlySingleton)`` – Checks if instruction is a
  memory load
* ``isMemSSAStore(CallBase *CB, bool onlySingleton)`` – Checks if instruction is a
  memory store
* ``isMemSSAArgRef/ArgMod/ArgRefMod/ArgNew(CallBase *CB, bool onlySingleton)`` –
  Checks for various parameter operation types
* ``getMemSSAParamIdx(CallBase *CB)`` – Extracts parameter index from shadow instruction
* ``getMemSSAParamNonPrimed/Primed(CallBase *CB, bool onlySingleton)`` – Extracts
  non-primed/primed variables from shadow instructions
* ``hasMemSSALoadUser(Value *V, bool onlySingleton)`` – Checks if a value has memory
  load users

Usage
=====

**Example: Analyzing a Call Site**:

.. code-block:: cpp

   #include "IR/MemorySSA/MemorySSA.h"
   #include "Alias/UnificationBased/seadsa/ShadowMem.hh"
   
   using namespace llvm;
   using namespace previrt::analysis;
   
   // First, run ShadowMem pass to instrument the code
   legacy::PassManager PM;
   PM.add(new seadsa::ShadowMem(dsaAnalysis, asi));
   PM.run(module);
   
   // Build MemorySSA information for the module
   MemorySSACallsManager mssaManager(module, pass, false /* only_singleton */);
   
   // Analyze a call site
   for (auto &F : module) {
     for (auto &I : instructions(&F)) {
       if (CallInst *CI = dyn_cast<CallInst>(&I)) {
         const MemorySSACallSite *cs = mssaManager.getCallSite(CI);
         if (cs) {
           // Query memory regions accessed by the callee
           for (unsigned i = 0; i < cs->numParams(); ++i) {
             if (cs->isRef(i)) {
               // Parameter is read-only
               const Value *nonPrimed = cs->getNonPrimed(i);
             } else if (cs->isMod(i) || cs->isRefMod(i)) {
               // Parameter is modified
               const Value *nonPrimed = cs->getNonPrimed(i);
               const Value *primed = cs->getPrimed(i);
             } else if (cs->isNew(i)) {
               // Parameter is newly created by callee
               const Value *primed = cs->getPrimed(i);
             }
           }
         }
       }
     }
   }

**Example: Querying Function Parameters**:

.. code-block:: cpp

   // Get memory SSA information for a function
   const MemorySSAFunction *mssaFunc = mssaManager.getFunction(&F);
   if (mssaFunc) {
     // Iterate over input formal parameters
     for (unsigned i = 0; i < mssaFunc->getNumInFormals(); ++i) {
       const Value *inFormal = mssaFunc->getInFormal(i);
       if (inFormal) {
         // Process the input formal parameter
       }
     }
   }

**Example: Checking Shadow Instructions**:

.. code-block:: cpp

   // Check if an instruction is a shadow memory operation
   if (const CallBase *CB = dyn_cast<CallBase>(&I)) {
     if (isMemSSALoad(CB, false)) {
       // Handle memory load
     } else if (isMemSSAStore(CB, false)) {
       // Handle memory store
     } else if (isMemSSAArgRef(CB, false)) {
       // Handle read-only parameter
     }
   }

Integration
===========

MemorySSA is used by various analyses and optimizations in Lotus:

* **IPDeadStoreElimination**: Interprocedural dead store elimination pass that uses
  MemorySSA to track memory def-use chains across function boundaries to identify
  and eliminate dead stores.

* **Memory Analysis**: Provides foundation for memory-aware analyses that need to
  understand memory dependencies and def-use relationships.

* **Optimization Passes**: Enables memory-aware optimizations that can reason about
  memory side effects of function calls.

The MemorySSA infrastructure provides a convenient abstraction over Sea-DSA's
shadow memory instrumentation, making it easier to write analyses that need to
understand memory operations and their dependencies across function boundaries.
