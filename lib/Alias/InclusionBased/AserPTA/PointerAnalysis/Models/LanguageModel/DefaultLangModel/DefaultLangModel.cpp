/**
 * @file DefaultLangModel.cpp
 * @brief Default language model for external function identification.
 *
 * Defines sets of external functions that have special semantics for pointer
 * analysis, such as thread creation functions that require special handling.
 *
 * @author peiming
 */
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultExtFunctions.h"

#include <set>

#include <llvm/ADT/StringRef.h>

using namespace llvm;

const std::set<StringRef> aser::DefaultExtFunctions::THREAD_CREATIONS{
    "pthread_create"};