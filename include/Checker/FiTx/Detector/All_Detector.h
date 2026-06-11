/** @file All_Detector.h @brief Aggregate detector combining all FiTx bug detectors. */
#pragma once
#include "Checker/FiTx/Detector/DF_Detector.h"
#include "Checker/FiTx/Detector/DL_Detector.h"
#include "Checker/FiTx/Detector/DUL_Detector.h"
#include "Checker/FiTx/Detector/Leak_Detector.h"
#include "Checker/FiTx/Detector/NullPtr_Detector.h"
#include "Checker/FiTx/Detector/Ref_Detector.h"
#include "Checker/FiTx/Detector/UAF_Detector.h"
#include "Checker/FiTx/Detector/Unref_Detector.h"
#include "Checker/FiTx/Detector/UseBeforeInit_Detector.h"
#include "Checker/FiTx/Frontend/State.h"

#include <functional>
#include <string>
#include <vector>

const auto def_funcs = {
    DoubleFree::define_states,
    DoubleLock::define_states,
    DoubleUnlock::defineStates,
    MemoryLeak::defineStates,
    NullPointer::defineStates,
    UnreferenceCounter::defineStates,
    ReferenceCounter::defineStates,
    UseAfterFree::defineStates,
    UseBeforeInitialization::defineStates,
};
