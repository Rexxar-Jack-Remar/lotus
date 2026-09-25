//
// Created by peiming on 10/22/19.
//
#pragma once

namespace aser {

// this class handles static object modelling,
// e.g., field sensitive
template <typename MemModel> struct MemModelTrait {
  // context type
  using CtxTy = typename MemModel::UnknownTypeError;

  using ObjectTy = typename MemModel::UnknownTypeError;

  // CGObjNode<MemModel>* allocateNullObj();

  // CGObjNode<MemModel>* allocateUniObj();
};

} // namespace aser
