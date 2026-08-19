SMT-LIB to LLVM Translation (SLOT)
==================================

This section documents the ``SLOT`` (SMT-LIB to LLVM) translation framework.

Overview
--------

**Location**: ``lib/Solvers/SMT/SLOT/``

**Headers**:
- ``Solvers/SMT/SLOT/SMTFormula.h``
- ``Solvers/SMT/SLOT/SMTNode.h``
- ``Solvers/SMT/LLVMNode.h``
- ``Solvers/SMT/SLOT/SLOTExceptions.h``

SLOT provides utilities for translating SMT-LIB formulas into equivalent LLVM IR
functions. This enables using LLVM toolchains for SMT formula analysis and
optimization.

Purpose
-------

Translating SMT formulas to LLVM IR allows:

1. **LLVM Optimizations**: Apply LLVM optimization passes to SMT formulas
2. **Code Generation**: Compile SMT formulas to native code
3. **Analysis Integration**: Use LLVM analysis passes on formulas
4. **Verification**: Leverage LLVM verification infrastructure

Key Components
--------------

SMTFormula Class
~~~~~~~~~~~~~~~~

The main class for translating SMT-LIB to LLVM.

**Header**:
.. code-block:: cpp

   #include "Solvers/SMT/SLOT/SMTFormula.h"

**Constructor**:
.. code-block:: cpp

   SLOT::SMTFormula(
       LLVMContext &lcx,        // LLVM context
       Module *lmodule,         // Output module
       IRBuilder<> &builder,    // IR builder
       std::string smt_string,  // SMT-LIB formula
       std::string func_name     // Output function name
   );

**Methods**:
- ``ToLLVM()`` - Generate LLVM IR function from the SMT formula

**Usage**:
.. code-block:: cpp

   #include "Solvers/SMT/SLOT/SMTFormula.h"

   using namespace SLOT;

   std::string smt_formula = R"(
       (declare-fun x () (_ BitVec 8))
       (declare-fun y () (_ BitVec 8))
       (assert (bvugt x y))
   )";

   LLVMContext lcx;
   Module *M = new Module("test", lcx);
   IRBuilder<> builder(lcx);

   SMTFormula formula(lcx, M, builder, smt_formula, "test_func");
   formula.ToLLVM();

SMTNode Class
~~~~~~~~~~~~~

Represents SMT expressions as LLVM values.

**Header**:
.. code-block:: cpp

   #include "Solvers/SMT/SLOT/SMTNode.h"

Supports translation of:
- Bit-vector constants and variables
- Arithmetic operations (add, sub, mul, etc.)
- Comparison operations
- Bit-wise operations
- Array operations

LLVMNode Class
~~~~~~~~~~~~~~

Translates LLVM values back to SMT expressions.

**Header**:
.. code-block:: cpp

   #include "Solvers/SMT/SLOT/LLVMNode.h"

SLOTExceptions
~~~~~~~~~~~~~

Custom exceptions for error handling:

.. code-block:: cpp

   namespace SLOT {
       class UnsupportedSMTOpException;
       class UnsupportedLLVMOpException;
       class UnsupportedTypeException;
       class NotImplementedException;
   }

Supported SMT-LIB Constructs
----------------------------

Variables:
- ``(declare-fun <name> () <type>)``
- ``(declare-const <name> <type>)``

Types:
- ``(_ BitVec <n>)`` - Bit-vectors
- ``Bool`` - Boolean
- ``Int`` - Integers
- ``Real`` - Reals
- ``(_ FloatingPoint <e> <s>)`` - Floating point

Operations:
- Arithmetic: ``+, -, *, /, mod``
- Bit-vector: ``bvadd, bvsub, bvmult, bvudiv, bvurem``
- Comparisons: ``=, <, >, <=, >=``
- Bit-wise: ``bvand, bvor, bvxor, bvnot``
- Shifts: ``bvshl, bvlshr, bvashr``

Example
-------

Translate an SMT formula to LLVM and apply optimizations:

.. code-block:: cpp

   #include "Solvers/SMT/SLOT/SMTFormula.h"
   #include "llvm/IR/Verifier.h"
   #include "llvm/Transforms/Scalar.h"

   // Parse SMT-LIB
   SLOT::SMTFormula formula(lcx, M, builder, smt_input, "check");

   // Generate LLVM IR
   formula.ToLLVM();

   // Create optimization passes
   legacy::PassManager PM;
   PM.add(createGVNPass());       // Common subexpression elimination
   PM.add(createSimplifyInstPass()); // Instruction simplification
   PM.run(*M);

   // Verify the result
   llvm::verifyModule(*M);

Related Components
------------------

- See :doc:`staub` - STAUB (uses SLOT for bounded translation)
- See :doc:`smt` - Core SMT solver infrastructure
