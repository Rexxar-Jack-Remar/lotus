#ifndef PARSER_H
#define PARSER_H

#include <string>

#include "fpsolve/semirings/commutativeRExp.h"
#include "fpsolve/semirings/prefix-semiring.h"

#include "fpsolve/datastructs/equations.h"

#ifdef USE_GENEPI
#include "fpsolve/semirings/semilinSetNdd.h"
#endif

#include "fpsolve/polynomials/commutative_polynomial.h"
#include "fpsolve/polynomials/non_commutative_polynomial.h"

template <typename SR>
class CommutativePolynomial;

class CommutativeRExp;

class Parser
{
private:
public:
  Parser();
  Equations<CommutativeRExp> rexp_parser(std::string input);
#ifdef USE_GENEPI
  Equations<SemilinSetNdd> slsetndd_parser(std::string input);
#endif

  NCEquations<FreeSemiring> free_parser(std::string input);
  NCEquations<PrefixSemiring> prefix_parser(std::string input, unsigned int length);

};

#endif
