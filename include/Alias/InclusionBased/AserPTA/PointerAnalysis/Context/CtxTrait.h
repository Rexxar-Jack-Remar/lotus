//
// Created by peiming on 8/14/19.
//
#pragma once

namespace aser {

template <typename ctx> class CtxTrait {
  using unknownTypeError = typename ctx::unknownTypeErrorType;
};

} // namespace aser

