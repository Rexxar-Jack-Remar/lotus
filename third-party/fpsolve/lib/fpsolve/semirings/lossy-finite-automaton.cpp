#include "fpsolve/semirings/lossy-finite-automaton.h"

LossyFiniteAutomaton LossyFiniteAutomaton::EMPTY = LossyFiniteAutomaton(FiniteAutomaton());
LossyFiniteAutomaton LossyFiniteAutomaton::EPSILON = LossyFiniteAutomaton(FiniteAutomaton::epsilon());
