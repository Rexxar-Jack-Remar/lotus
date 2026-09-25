//
// Created by peiming on 3/30/20.
//

#pragma once

#include <llvm/ADT/Statistic.h>

#define LOCAL_STATISTIC(VARNAME, DESC)                                         \
  llvm::Statistic VARNAME = {DEBUG_TYPE, #VARNAME, DESC}

