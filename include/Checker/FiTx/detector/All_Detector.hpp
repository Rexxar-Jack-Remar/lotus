#pragma once
#include "Checker/FiTx/detector/DF_Detector.hpp"
#include "Checker/FiTx/detector/DL_Detector.hpp"
#include "Checker/FiTx/detector/DUL_Detector.hpp"
#include "Checker/FiTx/detector/Leak_Detector.hpp"
#include "Checker/FiTx/detector/NullPtr_Detector.hpp"
#include "Checker/FiTx/detector/RefDetector.hpp"
#include "Checker/FiTx/detector/UAF_Detector.hpp"
#include "Checker/FiTx/detector/UnrefDetector.hpp"
#include "Checker/FiTx/detector/UseBeforeInit_Detector.hpp"
#include "Checker/FiTx/frontend/State.hpp"

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
