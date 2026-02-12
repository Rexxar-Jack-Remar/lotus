/**
 * @file CallWrapper.h
 * @brief Header for CallWrapper class
 */

#pragma once  
#include "IR/PDG/Support/LLVMEssentials.h"
#include "IR/PDG/Core/FunctionWrapper.h"
#include "IR/PDG/Core/Tree.h"
#include "IR/PDG/Support/PDGUtils.h"

namespace pdg
{
  /**
   * @brief Wrapper class for LLVM CallInst in the PDG
   * 
   * This class encapsulates a function call site (CallInst) and manages the mapping
   * between actual arguments at the call site and the formal parameters of the callee.
   * It handles:
   * - Construction of "actual in" and "actual out" trees for arguments and return values
   * - Mapping between actual values and their corresponding tree representations
   * - Support for field-sensitive parameter passing
   */
  class CallWrapper
  {
    private:
      // Define map type with full template parameters
      using ValueTreeMap = std::map<llvm::Value *, Tree *, std::less<llvm::Value *>, std::allocator<std::pair<llvm::Value * const, Tree *>>>;
      
      llvm::CallInst* _call_inst;
      llvm::Function* _called_func;
      std::vector<llvm::Value *> _arg_list;
      ValueTreeMap _arg_actual_in_tree_map;
      ValueTreeMap _arg_actual_out_tree_map;
      Tree * _ret_val_actual_in_tree;
      Tree * _ret_val_actual_out_tree;
      bool _has_param_trees = false;

    public:
      /**
       * @brief Constructor
       * @param ci The CallInst to wrap
       */
      CallWrapper(llvm::CallInst& ci)
      {
        _call_inst = &ci;
        _called_func = pdgutils::getCalledFunc(ci);
        for (auto *arg_iter = ci.arg_begin(); arg_iter != ci.arg_end(); arg_iter++)
        {
          _arg_list.push_back(*arg_iter);
        }
      }

      /**
       * @brief Build trees for actual arguments matching the callee's formal trees
       * @param callee_fw The FunctionWrapper of the called function
       */
      void buildActualTreeForArgs(FunctionWrapper &callee_fw);

      /**
       * @brief Build trees for the return value matching the callee's formal trees
       * @param callee_fw The FunctionWrapper of the called function
       */
      void buildActualTreesForRetVal(FunctionWrapper &callee_fw);

      llvm::CallInst *getCallInst() { return _call_inst; }
      llvm::Function *getCalledFunc() { return _called_func; }
      std::vector<llvm::Value *> &getArgList() { return _arg_list; }

      /**
       * @brief Get the "actual in" tree for a given argument value
       * @param actual_arg The argument value at the call site
       * @return Pointer to the Tree
       */
      Tree *getArgActualInTree(llvm::Value &actual_arg);

      /**
       * @brief Get the "actual out" tree for a given argument value
       * @param actual_arg The argument value at the call site
       * @return Pointer to the Tree
       */
      Tree *getArgActualOutTree(llvm::Value &actual_arg);

      Tree *getRetActualInTree() { return _ret_val_actual_in_tree; }
      Tree *getRetActualOutTree() { return _ret_val_actual_out_tree; }
      
      bool hasNullRetVal() { return (_ret_val_actual_in_tree == nullptr); }
      bool hasParamTrees() { return _has_param_trees; }
      void setHasParamTrees() { _has_param_trees = true; }
  };
} // namespace pdg
