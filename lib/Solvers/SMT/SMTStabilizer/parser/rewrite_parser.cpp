// Copyright (c) 2026 XiangZhang
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "parser.h"
#include "parser/dag.h"
#include "parser/kind.h"
#include "parser/number.h"
#include "parser/util.h"
namespace stabilizer::parser {
void echo_error(const std::string &msg) {
    throw std::runtime_error(msg);
}
// unary
std::shared_ptr<DAGNode> Parser::rewrite(NODE_KIND &t, std::shared_ptr<DAGNode> &p) {
    // return nullptr;
    switch (t) {
        case NODE_KIND::NT_CONST_ARRAY:
            return nullptr;
        case NODE_KIND::NT_NOT:
            if (p->isTrue())
                return mkFalse();
            else if (p->isFalse())
                return mkTrue();
            else if (p->isNot())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_NEG:
            if (p->isCInt())
                return mkConstInt(-toInt(p));
            else if (p->isCReal())
                return mkConstReal(-toReal(p));
            else if (p->isNeg())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_ABS:
            if (p->isCInt()) {
                Integer i = toInt(p);
                if (i < 0)
                    return mkConstInt(-i);
                else
                    return p;
            }
            else if (p->isCReal()) {
                Real r = toReal(p);
                if (r < 0)
                    return mkConstReal(-r);
                else
                    return p;
            }
            else if (p->isAbs())
                return p->getChild(0);
            else if (p->isNeg())
                p = p->getChild(0);
            return nullptr;
        case NODE_KIND::NT_SQRT:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).sqrt());
            }
            else
                return nullptr;

        case NODE_KIND::NT_SAFESQRT:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).safeSqrt());
            }
            else
                return nullptr;
        case NODE_KIND::NT_CEIL:
            if (p->getSort()->isInt() || p->isCInt())
                return p;
            else if (p->isCReal())
                return mkConstReal(toReal(p).ceil());
            else
                return nullptr;
        case NODE_KIND::NT_FLOOR:
            if (p->getSort()->isInt() || p->isCInt())
                return p;
            else if (p->isCReal())
                return mkConstReal(toReal(p).floor());
            else
                return nullptr;
        case NODE_KIND::NT_ROUND:
            if (p->getSort()->isInt() || p->isCInt())
                return p;
            else if (p->isCReal())
                return mkConstReal(toReal(p).round());
            else
                return nullptr;
        case NODE_KIND::NT_EXP:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).exp());
            }
            else if (p->isLn())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_LN:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).ln());
            }
            else if (p->isExp())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_LG:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).lg());
            }
            else
                return nullptr;
        case NODE_KIND::NT_LB:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).lb());
            }
            else if (p->isPow2())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_SIN:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).sin());
            }
            else
                return nullptr;
        case NODE_KIND::NT_COS:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).cos());
            }
            else
                return nullptr;
        case NODE_KIND::NT_SEC:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).sec());
            }
            else
                return nullptr;
        case NODE_KIND::NT_CSC:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).csc());
            }
            else
                return nullptr;
        case NODE_KIND::NT_TAN:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).tan());
            }
            else
                return nullptr;
        case NODE_KIND::NT_COT:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).cot());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ASIN:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).asin());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ACOS:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).acos());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ASEC:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).asec());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ACSC:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).acsc());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ATAN:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).atan());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ACOT:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).acot());
            }
            else
                return nullptr;
        case NODE_KIND::NT_SINH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).sinh());
            }
            else
                return nullptr;
        case NODE_KIND::NT_COSH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).cosh());
            }
            else
                return nullptr;
        case NODE_KIND::NT_TANH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).tanh());
            }
            else
                return nullptr;
        case NODE_KIND::NT_SECH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).sech());
            }
            else
                return nullptr;
        case NODE_KIND::NT_CSCH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).csch());
            }
            else
                return nullptr;
        case NODE_KIND::NT_COTH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).coth());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ASINH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).asinh());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ACOSH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).acosh());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ATANH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).atanh());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ASECH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).asech());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ACSCH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).acsch());
            }
            else
                return nullptr;
        case NODE_KIND::NT_ACOTH:
            if (p->isCInt() || p->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(p).acoth());
            }
            else
                return nullptr;
        case NODE_KIND::NT_TO_INT:
            if (p->isCInt())
                return p;
            else if (p->isCReal())
                return mkConstInt(toReal(p).toInteger());
            else
                return nullptr;
        case NODE_KIND::NT_TO_REAL:
            if (p->isCInt())
                return mkConstReal(toReal(p));
            else if (p->getSort()->isReal() || p->isCReal())
                return p;
            else
                return nullptr;
        case NODE_KIND::NT_IS_INT:
            if (p->isCInt())
                return mkTrue();
            else if (p->isCReal())
                return mkFalse();
            else
                return nullptr;
        case NODE_KIND::NT_IS_PRIME:
            if (p->isCInt()) {
                Integer i = toInt(p);
                if (MathUtils::isPrime(i))
                    return mkTrue();
                else
                    return mkFalse();
            }
            else {
                err_all(p, "Prime check on non-integer", line_number);
                return nullptr;
            }
        case NODE_KIND::NT_IS_EVEN:
            if (p->isCInt()) {
                Integer i = toInt(p);
                if (MathUtils::isEven(i))
                    return mkTrue();
                else
                    return mkFalse();
            }
            else {
                err_all(p, "Even check on non-integer", line_number);
                return nullptr;
            }
        case NODE_KIND::NT_IS_ODD:
            if (p->isCInt()) {
                Integer i = toInt(p);
                if (MathUtils::isOdd(i))
                    return mkTrue();
                else
                    return mkFalse();
            }
            else {
                err_all(p, "Odd check on non-integer", line_number);
                return nullptr;
            }
        case NODE_KIND::NT_FACT:
            if (p->isCInt()) {
                Integer i = toInt(p);
                if (i < 0) {
                    err_all(p, "Factorial of negative integer", line_number);
                    return nullptr;
                }
                else if (i == 0)
                    return mkConstInt(1);
                else
                    return mkConstInt(MathUtils::factorial(i));
            }
            else {
                err_all(p, "Factorial on non-integer", line_number);
                return nullptr;
            }
        case NODE_KIND::NT_BV_NOT:
            if (p->isCBV())
                return mkConstBv(BitVectorUtils::bvNot(p->toString()), p->getSort()->getBitWidth());
            else if (p->isBVNot())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_BV_NEG:
            if (p->isCBV())
                return mkConstBv(BitVectorUtils::bvNeg(p->toString()), p->getSort()->getBitWidth());
            else if (p->isBVNeg())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_BV_NEGO:
            return nullptr;
        case NODE_KIND::NT_FP_ABS:
        case NODE_KIND::NT_FP_NEG:
        case NODE_KIND::NT_FP_IS_NORMAL:
        case NODE_KIND::NT_FP_IS_SUBNORMAL:
        case NODE_KIND::NT_FP_IS_ZERO:
        case NODE_KIND::NT_FP_IS_INF:
        case NODE_KIND::NT_FP_IS_NAN:
        case NODE_KIND::NT_FP_IS_NEG:
        case NODE_KIND::NT_FP_IS_POS:
        case NODE_KIND::NT_FP_TO_REAL:
            return nullptr;
        case NODE_KIND::NT_STR_LEN:
            if (p->isCStr())
                return mkConstInt(StringUtils::strUnquate(p->toString()).size());
            else
                return nullptr;
        case NODE_KIND::NT_STR_TO_LOWER:
            if (p->isCStr())
                return mkConstStr(StringUtils::strToLower(p->toString()));
            else if (p->isStrToLower() || p->isStrToUpper())
                p = p->getChild(0);
            return nullptr;
        case NODE_KIND::NT_STR_TO_UPPER:
            if (p->isCStr())
                return mkConstStr(StringUtils::strToUpper(p->toString()));
            else if (p->isStrToUpper() || p->isStrToLower())
                p = p->getChild(0);
            return nullptr;
        case NODE_KIND::NT_STR_REV:
            if (p->isCStr())
                return mkConstStr(StringUtils::strRev(p->toString()));
            else if (p->isStrRev())
                return p->getChild(0);
            else
                return nullptr;
        case NODE_KIND::NT_STR_IS_DIGIT:
            if (p->isCStr())
                return TypeChecker::isInt(StringUtils::strUnquate(p->toString())) ? mkTrue() : mkFalse();
            else
                return nullptr;
        case NODE_KIND::NT_STR_FROM_INT:
            if (p->isCInt())
                return mkConstStr(p->toString());
            else
                return nullptr;
        case NODE_KIND::NT_STR_TO_INT:
            // return nullptr;
            // std::cout << p->getPureName() << std::endl;
            if (p->isCStr()) {
                std::string str = StringUtils::strUnquate(p->toString());
                if (TypeChecker::isInt(str)) {
                    return mkConstInt(Integer(str));
                }
                else {
                    return NodeManager::NAN_NODE;
                }
                // else {
                //     // std::cout << p->toString() << std::endl;
                //     err_all(p, "String to int on non-integer", line_number);
                // }
            }
            return nullptr;
        case NODE_KIND::NT_STR_TO_REG:
            return nullptr;
        case NODE_KIND::NT_STR_TO_CODE:
            if (p->isCStr()) {
                std::string str = StringUtils::strUnquate(p->toString());
                if (str.size() == 1) {
                    return mkConstInt(static_cast<int>(str[0]));
                }
                else
                    err_all(p, "String to code on non-single character string", line_number);
            }
            return nullptr;
        case NODE_KIND::NT_STR_FROM_CODE:
            if (p->isCInt()) {
                if (toInt(p) >= 0 && toInt(p) <= 127) {
                    return mkConstStr(std::string(1, static_cast<char>(toInt(p).toULong())));
                }
                else
                    err_all(p, "String from code on non-ASCII character", line_number);
            }
            return nullptr;
        case NODE_KIND::NT_REG_STAR:
        case NODE_KIND::NT_REG_PLUS:
        case NODE_KIND::NT_REG_OPT:
        case NODE_KIND::NT_REG_COMPLEMENT:
            return nullptr;
        case NODE_KIND::NT_BV_TO_NAT:
            if (p->isCBV())
                return mkConstInt(BitVectorUtils::bvToNat(p->toString()));
            else
                return nullptr;
        case NODE_KIND::NT_BV_TO_INT:
            if (p->isCBV())
                return mkConstInt(BitVectorUtils::bvToInt(p->toString()));
            else
                return nullptr;
        case NODE_KIND::NT_UBV_TO_INT:
            if (p->isCBV())
                return mkConstInt(BitVectorUtils::bvToNat(p->toString()));
            else
                return nullptr;
        case NODE_KIND::NT_SBV_TO_INT:
            if (p->isCBV())
                return mkConstInt(BitVectorUtils::bvToInt(p->toString()));
            else
                return nullptr;
        case NODE_KIND::NT_POW2:
            if (p->isCInt())
                return mkConstInt(MathUtils::pow(2, toInt(p)));
            else if (p->isLb())
                return p->getChild(0);
            else
                return nullptr;
        default:
            echo_error("unexpect NODE_KIND in rewrite unary !!!");
            return nullptr;
    }
}

// binary
std::shared_ptr<DAGNode> Parser::rewrite(NODE_KIND &t, std::shared_ptr<DAGNode> &l, std::shared_ptr<DAGNode> &r) {
    switch (t) {
        case NODE_KIND::NT_POW:
            if ((l->isCReal() || l->isCInt()) && (r->isCReal() || r->isCInt())) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(l).pow(toReal(r)));
            }
            else if (isZero(r) || isOne(l))
                return mkConstReal(1);
            else if (isOne(r))
                return l;
            else
                return nullptr;
        case NODE_KIND::NT_MOD:
            if (l->isCInt() && r->isCInt())
                return mkConstInt(toInt(l) % toInt(r));
            else if (isOne(r) || isZero(l) || l == r)
                return mkConstInt(0);
            else
                return nullptr;
        case NODE_KIND::NT_LOG:
            if ((l->isCInt() || l->isCReal()) && (r->isCInt() || r->isCReal())) {
                if (getEvaluateUseFloating())
                    return mkConstReal(toReal(l).log(toReal(r)));
            }
            else if (isOne(l))
                return mkConstReal(0);
            else if (l == r)
                return mkConstReal(1);
            else
                return nullptr;

        case NODE_KIND::NT_ATAN2:
            if ((l->isCInt() || l->isCReal()) && r->isCReal()) {
                if (getEvaluateUseFloating())
                    return mkConstReal(Real::atan2(toReal(l), toReal(r)));
            }
            else
                return nullptr;
        case NODE_KIND::NT_IS_DIVISIBLE:
            if (l->isCInt() && r->isCInt())
                return toInt(l) % toInt(r) == 0 ? mkTrue() : mkFalse();
            else {
                err_all(l, "Is divisible on non-integer", line_number);
                err_all(r, "Is divisible on non-integer", line_number);
                return nullptr;
            }
        case NODE_KIND::NT_GCD:
            if (l->isCInt() && r->isCInt())
                return mkConstInt(MathUtils::gcd(toInt(l), toInt(r)));
            else {
                err_all(l, "GCD on non-integer", line_number);
                err_all(r, "GCD on non-integer", line_number);
                return nullptr;
            }
        case NODE_KIND::NT_LCM:
            if (l->isCInt() && r->isCInt())
                return mkConstInt(MathUtils::lcm(toInt(l), toInt(r)));
            else {
                err_all(l, "LCM on non-integer", line_number);
                err_all(r, "LCM on non-integer", line_number);
                return nullptr;
            }
        case NODE_KIND::NT_BV_UDIV:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvUdiv(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isZero(r))
                return mkConstBv("#b" + std::string(l->getSort()->getBitWidth(), '1'), l->getSort()->getBitWidth());
            else if (isOne(r))
                return l;
            else
                return nullptr;
        case NODE_KIND::NT_BV_SDIV:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvSdiv(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isOne(r))
                return l;
            else if (r->isCBV() && BitVectorUtils::bvIsNegOne(r->toString())) {
                return mkBvNeg(l);
            }
            else
                return nullptr;
        case NODE_KIND::NT_BV_UREM:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvUrem(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isZero(r))
                return l;
            else if (isOne(r) || l == r)
                return mkConstBv(BitVectorUtils::intToBv(0, l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else
                return nullptr;
        case NODE_KIND::NT_BV_SREM:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvSrem(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isOne(r) || l == r)
                return mkConstBv(BitVectorUtils::intToBv(0, l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else
                return nullptr;
        case NODE_KIND::NT_BV_UMOD:
            t = NODE_KIND::NT_BV_UREM;
            return rewrite(t, l, r);
        case NODE_KIND::NT_BV_SMOD:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvSmod(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isZero(r))
                return l;
            else if (isOne(r) || l == r)
                return mkConstBv(BitVectorUtils::intToBv(0, l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else
                return nullptr;
        case NODE_KIND::NT_BV_SDIVO:
        case NODE_KIND::NT_BV_UDIVO:
        case NODE_KIND::NT_BV_SREMO:
        case NODE_KIND::NT_BV_UREMO:
        case NODE_KIND::NT_BV_SMODO:
        case NODE_KIND::NT_BV_UMODO:
            return nullptr;
        case NODE_KIND::NT_BV_SHL:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvShl(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isZero(r))
                return l;
            else if (r->isCBV() && BitVectorUtils::bvCompareToUint(r->toString(), l->getSort()->getBitWidth()) >= 0)
                return mkConstBv(BitVectorUtils::intToBv(0, l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else
                return nullptr;
        case NODE_KIND::NT_BV_LSHR:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvLshr(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isZero(r))
                return l;
            else if (r->isCBV() && BitVectorUtils::bvCompareToUint(r->toString(), l->getSort()->getBitWidth()) >= 0)
                return mkConstBv(BitVectorUtils::intToBv(0, l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else
                return nullptr;
        case NODE_KIND::NT_BV_ASHR:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvAshr(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isZero(r))
                return l;
            else
                return nullptr;
        case NODE_KIND::NT_BV_ULT:
            if (l->isCBV() && r->isCBV())
                return BitVectorUtils::bvComp(l->toString(), r->toString(), t) ? mkTrue() : mkFalse();
            else if (isZero(r))
                return mkFalse();
            else if (l->isCBV() && BitVectorUtils::bvIsMaxUnsigned(l->toString()))
                return mkFalse();
            else if (l == r)
                return mkFalse();
            else
                return nullptr;
        case NODE_KIND::NT_BV_ULE:
            if (l->isCBV() && r->isCBV())
                return BitVectorUtils::bvComp(l->toString(), r->toString(), t) ? mkTrue() : mkFalse();
            else if (l == r)
                return mkTrue();
            else
                return nullptr;
        case NODE_KIND::NT_BV_SLT:
            if (l->isCBV() && r->isCBV())
                return BitVectorUtils::bvComp(l->toString(), r->toString(), t) ? mkTrue() : mkFalse();
            else if (l->isCBV() && BitVectorUtils::bvIsMaxSigned(l->toString()))
                return mkFalse();
            else if (r->isCBV() && BitVectorUtils::bvIsMinSigned(r->toString()))
                return mkFalse();
            else if (l == r)
                return mkFalse();
            else
                return nullptr;
        case NODE_KIND::NT_BV_SLE:
            if (l->isCBV() && r->isCBV())
                return BitVectorUtils::bvComp(l->toString(), r->toString(), t) ? mkTrue() : mkFalse();
            else if (l == r)
                return mkTrue();
            else if (l->isCBV() && BitVectorUtils::bvIsMaxSigned(l->toString()))
                return mkEq({l, r});
            else if (r->isCBV() && BitVectorUtils::bvIsMinSigned(r->toString()))
                return mkEq({l, r});
            else
                return nullptr;
        case NODE_KIND::NT_BV_UGT:
            t = NODE_KIND::NT_BV_ULT;
            std::swap(l, r);
            return rewrite(t, l, r);
        case NODE_KIND::NT_BV_UGE:
            t = NODE_KIND::NT_BV_ULE;
            std::swap(l, r);
            return rewrite(t, l, r);
        case NODE_KIND::NT_BV_SGT:
            t = NODE_KIND::NT_BV_SLT;
            std::swap(l, r);
            return rewrite(t, l, r);
        case NODE_KIND::NT_BV_SGE:
            t = NODE_KIND::NT_BV_SLE;
            std::swap(l, r);
            return rewrite(t, l, r);
        case NODE_KIND::NT_NAT_TO_BV:
            if (l->isCInt() && r->isCInt())
                return mkConstBv(BitVectorUtils::natToBv(toInt(l), toInt(r)), toInt(r).toULong());
            else
                return nullptr;
        case NODE_KIND::NT_INT_TO_BV:
            if (l->isCInt() && r->isCInt())
                return mkConstBv(BitVectorUtils::intToBv(toInt(l), toInt(r)), toInt(r).toULong());
            else
                return nullptr;
        case NODE_KIND::NT_FP_REM:
        case NODE_KIND::NT_FP_SQRT:
        case NODE_KIND::NT_FP_ROUND_TO_INTEGRAL:
            return nullptr;
        case NODE_KIND::NT_SELECT:
            return nullptr;
        case NODE_KIND::NT_STR_PREFIXOF:
            if (l->isCStr() && r->isCStr())
                return StringUtils::strPrefixof(l->toString(), r->toString()) ? mkTrue() : mkFalse();
            else
                return nullptr;
        case NODE_KIND::NT_STR_SUFFIXOF:
            if (l->isCStr() && r->isCStr())
                return StringUtils::strSuffixof(l->toString(), r->toString()) ? mkTrue() : mkFalse();
            else
                return nullptr;
        case NODE_KIND::NT_STR_CHARAT:
            if (l->isCStr() && r->isCInt())
                return mkConstStr(StringUtils::strCharAt(l->toString(), toInt(r)));
            else
                return nullptr;
        case NODE_KIND::NT_STR_SPLIT:
            return nullptr;
        case NODE_KIND::NT_STR_IN_REG:
        case NODE_KIND::NT_REG_RANGE:
        case NODE_KIND::NT_REG_REPEAT:
            return nullptr;
        case NODE_KIND::NT_STR_CONTAINS:
            if (l->isCStr() && r->isCStr())
                return StringUtils::strContains(l->toString(), r->toString()) ? mkTrue() : mkFalse();
            else
                return nullptr;
        case NODE_KIND::NT_STR_NUM_SPLITS_RE:
            return nullptr;
        case NODE_KIND::NT_BV_REPEAT:
            if (l->isCBV() && r->isCInt())
                return mkConstBv(BitVectorUtils::bvRepeat(l->toString(), toInt(r)), l->getSort()->getBitWidth() * toInt(r).toULong());
            else if (isOne(r))
                return l;
            else
                return nullptr;
        case NODE_KIND::NT_BV_ZERO_EXT:
            if (l->isCBV() && r->isCInt())
                return mkConstBv(BitVectorUtils::bvZeroExtend(l->toString(), toInt(r)), l->getSort()->getBitWidth() + toInt(r).toULong());
            else if (isZero(r))
                return l;
            else if (l->isBVZeroExt() && l->getChild(1)->isCInt()) {
                r = mkConstInt(toInt(l->getChild(1)) + toInt(r));
                l = l->getChild(0);
                return rewrite(t, l, r);
            }
            else
                return nullptr;
        case NODE_KIND::NT_BV_SIGN_EXT:
            if (l->isCBV() && r->isCInt())
                return mkConstBv(BitVectorUtils::bvSignExtend(l->toString(), toInt(r)), l->getSort()->getBitWidth() + toInt(r).toULong());
            else if (isZero(r))
                return l;
            else if (l->isBVSignExt() && l->getChild(1)->isCInt()) {
                r = mkConstInt(toInt(l->getChild(1)) + toInt(r));
                l = l->getChild(0);
                return rewrite(t, l, r);
            }
            else
                return nullptr;
        case NODE_KIND::NT_BV_ROTATE_LEFT:
            if (l->isCBV() && r->isCInt())
                return mkConstBv(BitVectorUtils::bvRotateLeft(l->toString(), toInt(r)), l->getSort()->getBitWidth());
            else if (r->isCInt() && toInt(r).toULong() % l->getSort()->getBitWidth() == 0)
                return l;
            else if (l->isBVRotLeft() && r->isCInt()) {
                Integer total_rotate = toInt(r) + toInt(l->getChild(1));
                Integer bitwidth = Integer(l->getSort()->getBitWidth());
                total_rotate = total_rotate % bitwidth;
                r = mkConstInt(total_rotate);
                l = l->getChild(0);
                return rewrite(t, l, r);
            }
            else if (l->isBVRotRight() && r->isCInt()) {
                if (toInt(r) == toInt(l->getChild(1))) {
                    return l->getChild(0);
                }
                else if (toInt(r) > toInt(l->getChild(1))) {
                    Integer total_rotate = toInt(r) - toInt(l->getChild(1));
                    Integer bitwidth = Integer(l->getSort()->getBitWidth());
                    total_rotate = total_rotate % bitwidth;
                    r = mkConstInt(total_rotate);
                    l = l->getChild(0);
                    return rewrite(t, l, r);
                }
                else {
                    Integer total_rotate = toInt(l->getChild(1)) - toInt(r);
                    Integer bitwidth = Integer(l->getSort()->getBitWidth());
                    total_rotate = total_rotate % bitwidth;
                    t = NODE_KIND::NT_BV_ROTATE_RIGHT;
                    r = mkConstInt(total_rotate);
                    l = l->getChild(0);
                    return rewrite(t, l, r);
                }
            }
            else
                return nullptr;
        case NODE_KIND::NT_BV_ROTATE_RIGHT:
            if (l->isCBV() && r->isCInt())
                return mkConstBv(BitVectorUtils::bvRotateRight(l->toString(), toInt(r)), l->getSort()->getBitWidth());
            else if (r->isCInt() && toInt(r).toULong() % l->getSort()->getBitWidth() == 0)
                return l;
            else if (l->isBVRotRight() && r->isCInt()) {
                Integer total_rotate = toInt(r) + toInt(l->getChild(1));
                Integer bitwidth = Integer(l->getSort()->getBitWidth());
                total_rotate = total_rotate % bitwidth;
                r = mkConstInt(total_rotate);
                l = l->getChild(0);
                return rewrite(t, l, r);
            }
            else if (l->isBVRotLeft() && r->isCInt()) {
                if (toInt(r) == toInt(l->getChild(1))) {
                    return l->getChild(0);
                }
                else if (toInt(r) > toInt(l->getChild(1))) {
                    Integer total_rotate = toInt(r) - toInt(l->getChild(1));
                    Integer bitwidth = Integer(l->getSort()->getBitWidth());
                    total_rotate = total_rotate % bitwidth;
                    r = mkConstInt(total_rotate);
                    l = l->getChild(0);
                    return rewrite(t, l, r);
                }
                else {
                    Integer total_rotate = toInt(l->getChild(1)) - toInt(r);
                    Integer bitwidth = Integer(l->getSort()->getBitWidth());
                    total_rotate = total_rotate % bitwidth;
                    t = NODE_KIND::NT_BV_ROTATE_LEFT;
                    r = mkConstInt(total_rotate);
                    l = l->getChild(0);
                    return rewrite(t, l, r);
                }
            }
            else
                return nullptr;
        case NODE_KIND::NT_FP_MIN:
        case NODE_KIND::NT_FP_MAX:
            return nullptr;
        case NODE_KIND::NT_BV_XNOR:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvXnor(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (l == r)
                return mkConstBv(BitVectorUtils::mkOnes(l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else
                return nullptr;
        case NODE_KIND::NT_BV_NAND:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvNand(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (l == r)
                return mkBvNot(l);
            else if (isZero(l) || isZero(r))
                return mkConstBv(BitVectorUtils::mkOnes(l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else if (isOnes(l))
                return mkBvNot(r);
            else if (isOnes(r))
                return mkBvNot(l);
            else
                return nullptr;
        case NODE_KIND::NT_BV_NOR:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvNor(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (l == r)
                return mkBvNot(l);
            else if (isOnes(l) || isOnes(r))
                return mkConstBv(BitVectorUtils::intToBv(0, l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else if (isZero(l))
                return mkBvNot(r);
            else if (isZero(r))
                return mkBvNot(l);
            else
                return nullptr;
        case NODE_KIND::NT_BV_SUB:
            if (l->isCBV() && r->isCBV())
                return mkConstBv(BitVectorUtils::bvSub(l->toString(), r->toString()), l->getSort()->getBitWidth());
            else if (isZero(r))
                return l;
            else if (isZero(l))
                return mkBvNeg(r);
            else if (l == r)
                return mkConstBv(BitVectorUtils::intToBv(0, l->getSort()->getBitWidth()), l->getSort()->getBitWidth());
            else
                return nullptr;
        default:
            // echo_error("unexpect NODE_KIND in rewrite binary !!!");
            return nullptr;
    }
}

// ternary
std::shared_ptr<DAGNode> Parser::rewrite(NODE_KIND &t, std::shared_ptr<DAGNode> &l, std::shared_ptr<DAGNode> &m, std::shared_ptr<DAGNode> &r) {
    switch (t) {
        case NODE_KIND::NT_ITE:
            if (l->isTrue())
                return m;
            else if (l->isFalse())
                return r;
            else if (m == r)
                return m;
            else
                return nullptr;
        case NODE_KIND::NT_FP_ADD:
        case NODE_KIND::NT_FP_SUB:
        case NODE_KIND::NT_FP_MUL:
        case NODE_KIND::NT_FP_DIV:
        case NODE_KIND::NT_FP_TO_UBV:
        case NODE_KIND::NT_FP_TO_SBV:
            return nullptr;
        case NODE_KIND::NT_STORE:
            return nullptr;
        case NODE_KIND::NT_STR_SUBSTR:
            if (l->isCStr() && m->isCInt() && r->isCInt())
                return mkConstStr(StringUtils::strSubstr(l->toString(), toInt(m), toInt(r)));
            else
                return nullptr;
        case NODE_KIND::NT_STR_INDEXOF:
            if (l->isCStr() && m->isCStr() && r->isCInt())
                return mkConstInt(StringUtils::strIndexof(l->toString(), m->toString(), toInt(r)));
            else
                return nullptr;
        case NODE_KIND::NT_STR_UPDATE:
            if (l->isCStr() && m->isCInt() && r->isCStr())
                return mkConstStr(StringUtils::strUpdate(l->toString(), toInt(m), r->toString()));
            else
                return nullptr;
        case NODE_KIND::NT_STR_REPLACE:
            if (l->isCStr() && m->isCStr() && r->isCStr())
                return mkConstStr(StringUtils::strReplace(l->toString(), m->toString(), r->toString()));
            else
                return nullptr;
        case NODE_KIND::NT_STR_REPLACE_ALL:
            if (l->isCStr() && m->isCStr() && r->isCStr())
                return mkConstStr(StringUtils::strReplaceAll(l->toString(), m->toString(), r->toString()));
            else
                return nullptr;
        case NODE_KIND::NT_STR_SPLIT_AT:
        case NODE_KIND::NT_STR_SPLIT_REST:
        case NODE_KIND::NT_STR_SPLIT_AT_RE:
        case NODE_KIND::NT_STR_SPLIT_REST_RE:
        case NODE_KIND::NT_STR_REPLACE_REG:
        case NODE_KIND::NT_STR_REPLACE_REG_ALL:
        case NODE_KIND::NT_STR_INDEXOF_REG:
        case NODE_KIND::NT_REG_LOOP:
            return nullptr;
        case NODE_KIND::NT_BV_EXTRACT:
            if (l->isCBV() && m->isCInt() && r->isCInt()) {
                return mkConstBv(BitVectorUtils::bvExtract(l->toString(), toInt(m), toInt(r)), toInt(m).toULong() - toInt(r).toULong() + 1);
            }
            else if (m->isCInt() && toInt(m) == l->getSort()->getBitWidth() - 1 && isZero(r))
                return l;
            else if (m->isInt() && r->isCInt() && l->isBVExtract() && l->getChild(1)->isCInt() && l->getChild(2)->isCInt()) {
                m = mkConstInt(toInt(m) + toInt(l->getChild(2)));
                r = mkConstInt(toInt(r) + toInt(l->getChild(2)));
                l = l->getChild(0);
                return rewrite(t, l, m, r);
            }
            else
                return nullptr;
        default:
            echo_error("unexpect NODE_KIND in rewrite ternary !!!");
            return nullptr;
    }
}

// 4-ary
std::shared_ptr<DAGNode> Parser::rewrite(NODE_KIND &t, std::shared_ptr<DAGNode> &l, std::shared_ptr<DAGNode> &ml, std::shared_ptr<DAGNode> &mr, std::shared_ptr<DAGNode> &r) {
    switch (t) {
        case NODE_KIND::NT_FP_FMA:
            // case NODE_KIND::NT_FP_TO_FP:
            return nullptr;
        default:
            echo_error("unexpect NODE_KIND in rewrite 4-ary !!!");
            return nullptr;
    }
}

// n-ary
std::shared_ptr<DAGNode> Parser::rewrite(NODE_KIND &t, std::vector<std::shared_ptr<DAGNode>> &p) {
    // return nullptr;
    switch (t) {
        case NODE_KIND::NT_LE: {
            std::vector<std::shared_ptr<DAGNode>> np{p.front()};
            np.reserve(p.size());
            size_t suf_count = (np.front()->isCInt() || np.front()->isCReal());
            std::shared_ptr<DAGNode> last_const = suf_count ? np.front() : nullptr;
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                if (p.at(i) == np.back())
                    continue;
                else if (p.at(i)->isCInt() || p.at(i)->isCReal()) {
                    if (last_const != nullptr) {
                        if (p.at(i)->isCInt() && last_const->isCInt()) {
                            auto lvalue = toInt(last_const), rvalue = toInt(p.at(i));
                            if (lvalue > rvalue)
                                return mkFalse();
                            else if (last_const == np.back() && lvalue == rvalue)
                                continue;
                        }
                        else {
                            auto lvalue = toReal(last_const), rvalue = toReal(p.at(i));
                            if (lvalue > rvalue)
                                return mkFalse();
                            else if (last_const == np.back() && lvalue == rvalue)
                                continue;
                        }
                    }

                    last_const = p.at(i);
                    ++suf_count;
                }
                else
                    suf_count = 0;
                if (suf_count > 2) {
                    np.pop_back();
                    suf_count = 2;
                }
                np.emplace_back(p.at(i));
            }

            size_t lidx = 0, ridx = np.size() - 1;
            if (np.front()->isConst())
                while (lidx + 1 < np.size()) {
                    if (!np.at(lidx + 1)->isConst())
                        break;
                    ++lidx;
                }
            if (np.back()->isConst())
                while (ridx > 0) {
                    if (!np.at(ridx - 1)->isConst())
                        break;
                    --ridx;
                }
            if (lidx >= ridx)
                return mkTrue();
            np = std::vector<std::shared_ptr<DAGNode>>(np.begin() + lidx, np.begin() + ridx + 1);

            if (np.size() <= 1)
                return mkTrue();
            else if (np.front()->isConst() && np.back()->isConst()) {
                if (toReal(np.front()) == toReal(np.back())) {
                    np.pop_back();
                    t = NODE_KIND::NT_EQ;
                    return rewrite(t, np);
                }
            }
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_LT: {
            std::vector<std::shared_ptr<DAGNode>> np{p.front()};
            np.reserve(p.size());
            size_t suf_count = (np.front()->isCInt() || np.front()->isCReal());
            std::shared_ptr<DAGNode> last_const = suf_count ? np.front() : nullptr;
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                if (p.at(i) == np.back())
                    return mkFalse();
                else if (p.at(i)->isCInt() || p.at(i)->isCReal()) {
                    if (last_const != nullptr) {
                        if (p.at(i)->isCInt() && last_const->isCInt()) {
                            auto lvalue = toInt(last_const), rvalue = toInt(p.at(i));
                            if (lvalue >= rvalue)
                                return mkFalse();
                        }
                        else {
                            auto lvalue = toReal(last_const), rvalue = toReal(p.at(i));
                            if (lvalue >= rvalue)
                                return mkFalse();
                        }
                    }

                    last_const = p.at(i);
                    ++suf_count;
                }
                else
                    suf_count = 0;
                if (suf_count > 2) {
                    np.pop_back();
                    suf_count = 2;
                }
                np.emplace_back(p.at(i));
            }
            size_t lidx = 0, ridx = np.size() - 1;
            if (np.front()->isConst())
                while (lidx + 1 < np.size()) {
                    if (!np.at(lidx + 1)->isConst())
                        break;
                    ++lidx;
                }
            if (np.back()->isConst())
                while (ridx > 0) {
                    if (!np.at(ridx - 1)->isConst())
                        break;
                    --ridx;
                }
            if (lidx >= ridx)
                return mkTrue();
            np = std::vector<std::shared_ptr<DAGNode>>(np.begin() + lidx, np.begin() + ridx + 1);

            if (np.size() <= 1)
                return mkTrue();
            // if (np.size() == suf_count || np.size() == 1)
            //     return mkTrue();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_GE:
            t = NODE_KIND::NT_LE;
            std::reverse(p.begin(), p.end());
            return rewrite(t, p);
        case NODE_KIND::NT_GT:
            t = NODE_KIND::NT_LT;
            std::reverse(p.begin(), p.end());
            return rewrite(t, p);
        case NODE_KIND::NT_EQ:
        case NODE_KIND::NT_EQ_BOOL:
        case NODE_KIND::NT_EQ_OTHER:
            std::sort(p.begin(), p.end(), [](const std::shared_ptr<DAGNode> &a, const std::shared_ptr<DAGNode> &b) {
                if (a->isConst() && !b->isConst())
                    return false;
                else if (!a->isConst() && b->isConst())
                    return true;
                else
                    return a < b;
            });
            p.erase(std::unique(p.begin(), p.end()), p.end());
            while (p.size() >= 2 && p.rbegin()[0]->isConst() && p.rbegin()[1]->isConst()) {
                if (p.rbegin()[0]->isCInt() && p.rbegin()[1]->isCInt())
                    return mkFalse();
                else if (p.rbegin()[0]->isNumeral() && p.rbegin()[1]->isNumeral()) {
                    if (toReal(p.rbegin()[0]) != toReal(p.rbegin()[1]))
                        return mkFalse();
                    p.pop_back();
                }
                else if (p.rbegin()[0]->getName() == "(fp_bit_representation)" || p.rbegin()[1]->getName() == "(fp_bit_representation)") {
                    break;
                }
                else
                    return mkFalse();
            }
            if (p.size() <= 1)
                return mkTrue();
            return nullptr;
        case NODE_KIND::NT_DIV_INT: {
            if (p.front()->isCInt() || p.front()->isCReal()) {
                std::vector<std::shared_ptr<DAGNode>> np;
                np.reserve(p.size());
                std::shared_ptr<DAGNode> last_const = p.front();
                for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                    if (p.at(i)->isCInt() || p.at(i)->isCReal()) {
                        if (last_const->isCInt() && p.at(i)->isCInt())
                            last_const = mkConstInt(toInt(last_const) / toInt(p.at(i)));
                        else if (getEvaluateUseFloating())
                            last_const = mkConstInt((toReal(last_const) / toReal(p.at(i))).floor().toInt());
                        else
                            echo_error("unexpected divide int rewrite !!!");
                    }
                    else {
                        for (size_t j = i; j < isz; ++j)
                            np.emplace_back(p.at(j));
                        break;
                    }
                }
                if (np.empty())
                    return last_const;
                np.insert(np.begin(), last_const);
                p.swap(np);
            }
            return nullptr;
        }
        case NODE_KIND::NT_DIV_REAL:
            return nullptr;
        case NODE_KIND::NT_STR_LT: {
            std::vector<std::shared_ptr<DAGNode>> np{p.front()};
            np.reserve(p.size());
            size_t suf_count = np.front()->isCStr();
            std::shared_ptr<DAGNode> last_const = suf_count ? np.front() : nullptr;
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                if (p.at(i) == np.back())
                    return mkFalse();
                else if (p.at(i)->isCStr()) {
                    if (last_const != nullptr) {
                        auto lvalue = last_const->toString(), rvalue = p.at(i)->toString();
                        if (lvalue >= rvalue)
                            return mkFalse();
                    }

                    last_const = p.at(i);
                    ++suf_count;
                }
                else
                    suf_count = 0;
                if (suf_count > 2) {
                    np.pop_back();
                    suf_count = 2;
                }
                np.emplace_back(p.at(i));
            }
            size_t lidx = 0, ridx = np.size() - 1;
            if (np.front()->isConst())
                while (lidx + 1 < np.size()) {
                    if (!np.at(lidx + 1)->isConst())
                        break;
                    ++lidx;
                }
            if (np.back()->isConst())
                while (ridx > 0) {
                    if (!np.at(ridx - 1)->isConst())
                        break;
                    --ridx;
                }
            if (lidx >= ridx)
                return mkTrue();
            np = std::vector<std::shared_ptr<DAGNode>>(np.begin() + lidx, np.begin() + ridx + 1);

            if (np.size() <= 1)
                return mkTrue();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_STR_LE: {
            std::vector<std::shared_ptr<DAGNode>> np{p.front()};
            np.reserve(p.size());
            size_t suf_count = np.front()->isCStr();
            std::shared_ptr<DAGNode> last_const = suf_count ? np.front() : nullptr;
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                if (p.at(i) == np.back())
                    continue;
                else if (p.at(i)->isCStr()) {
                    if (last_const != nullptr) {
                        auto lvalue = last_const->toString(), rvalue = p.at(i)->toString();
                        if (lvalue > rvalue)
                            return mkFalse();
                        else if (last_const == np.back() && lvalue == rvalue)
                            continue;
                    }

                    last_const = p.at(i);
                    ++suf_count;
                }
                else
                    suf_count = 0;
                if (suf_count > 2) {
                    np.pop_back();
                    suf_count = 2;
                }
                np.emplace_back(p.at(i));
            }

            size_t lidx = 0, ridx = np.size() - 1;
            if (np.front()->isConst())
                while (lidx + 1 < np.size()) {
                    if (!np.at(lidx + 1)->isConst())
                        break;
                    ++lidx;
                }
            if (np.back()->isConst())
                while (ridx > 0) {
                    if (!np.at(ridx - 1)->isConst())
                        break;
                    --ridx;
                }
            if (lidx >= ridx)
                return mkTrue();
            np = std::vector<std::shared_ptr<DAGNode>>(np.begin() + lidx, np.begin() + ridx + 1);

            if (np.size() <= 1)
                return mkTrue();
            else if (np.front()->isConst() && np.back()->isConst()) {
                if (np.front()->toString() == np.back()->toString()) {
                    np.pop_back();
                    t = NODE_KIND::NT_EQ;
                    return rewrite(t, np);
                }
            }
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_STR_GT:
            t = NODE_KIND::NT_STR_LT;
            std::reverse(p.begin(), p.end());
            return rewrite(t, p);
        case NODE_KIND::NT_STR_GE:
            t = NODE_KIND::NT_STR_LE;
            std::reverse(p.begin(), p.end());
            return rewrite(t, p);
        case NODE_KIND::NT_DISTINCT:
        case NODE_KIND::NT_DISTINCT_BOOL:
        case NODE_KIND::NT_DISTINCT_OTHER:
            std::sort(p.begin(), p.end());
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                if (p.at(i) == p.at(i - 1))
                    return mkFalse();
            }
            return nullptr;
        case NODE_KIND::NT_AND: {
            std::sort(p.begin(), p.end());
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isFalse())
                    return mkFalse();
                else if (pc->isTrue())
                    continue;
                else if (!np.empty() && pc == np.back())
                    continue;
                else
                    np.emplace_back(pc);
            }
            if (np.empty())
                return mkTrue();
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_OR: {
            std::sort(p.begin(), p.end());
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isTrue())
                    return mkTrue();
                else if (pc->isFalse())
                    continue;
                else if (!np.empty() && pc == np.back())
                    continue;
                else
                    np.emplace_back(pc);
            }
            if (np.empty())
                return mkFalse();
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_IMPLIES: {
            if (p.back()->isTrue())
                return mkTrue();
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (size_t i = 0, isz = p.size() - 1; i < isz; ++i) {
                if (p.at(i)->isFalse())
                    return mkTrue();
                else if (p.at(i)->isTrue())
                    continue;
                else if (!np.empty() && p.at(i) == np.back())
                    continue;
                else if (p.at(i) == p.back())
                    return mkTrue();
                else
                    np.emplace_back(p.at(i));
            }
            np.emplace_back(p.back());
            if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_XOR: {
            std::sort(p.begin(), p.end());
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isFalse())
                    continue;
                else if (!np.empty() && pc == np.back())
                    np.pop_back();
                else
                    np.emplace_back(pc);
            }
            if (np.empty())
                return mkFalse();
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_ADD: {
            std::shared_ptr<DAGNode> cst = nullptr;
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isCInt()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstInt(toInt(cst) + toInt(pc));
                }
                else if (pc->isCReal()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstReal(toReal(cst) + toReal(pc));
                }
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (!isZero(cst))
                    np.emplace_back(cst);
            }
            if (np.empty())
                return mkConstInt(0);
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_MUL: {
            std::shared_ptr<DAGNode> cst = nullptr;
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isCInt()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstInt(toInt(cst) * toInt(pc));
                }
                else if (pc->isCReal()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstReal(toReal(cst) * toReal(pc));
                }
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (isZero(cst))
                    return cst->isCInt() ? mkConstInt(0) : mkConstReal(0.0);
                else if (!isOne(cst))
                    np.emplace_back(cst);
            }
            if (np.size() == 1)
                return np.front();
            else if (np.empty())
                return mkConstInt(1);
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_IAND: {
            std::shared_ptr<DAGNode> cst = nullptr;
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isCInt()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstInt(toInt(cst) & toInt(pc));
                }
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (isZero(cst))
                    return cst;
                else
                    np.emplace_back(cst);
            }
            if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_SUB: {
            std::vector<std::shared_ptr<DAGNode>> np{p.front()};
            np.reserve(p.size());
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                const auto &pc = p.at(i);
                if (pc == np.back()) {
                    np.pop_back();
                    if (pc->getSort()->isInt())
                        np.emplace_back(mkConstInt(0));
                    else
                        np.emplace_back(mkConstReal(0.0));
                }
                else if (pc->isNumeral() && np.back()->isNumeral()) {
                    if (pc->isCInt() && np.back()->isCInt())
                        np.back() = mkConstInt(toInt(np.back()) - toInt(pc));
                    else
                        np.back() = mkConstReal(toReal(np.back()) - toReal(pc));
                }
                else {
                    for (size_t j = i; j < isz; ++j)
                        np.emplace_back(p.at(j));
                    break;
                }
            }

            if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_BV_AND: {
            std::sort(p.begin(), p.end());
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            std::shared_ptr<DAGNode> cst = nullptr;
            for (const auto &pc : p) {
                if (pc->isCBV()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstBv(BitVectorUtils::bvAnd(cst->toString(), pc->toString()), cst->getSort()->getBitWidth());
                }
                else if (!np.empty() && pc == np.back())
                    continue;
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (isZero(cst))
                    return cst;
                else if (!isOnes(cst))
                    np.emplace_back(cst);
            }

            if (np.empty())
                return mkConstBv(BitVectorUtils::mkOnes(p.front()->getSort()->getBitWidth()), p.front()->getSort()->getBitWidth());
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_BV_OR: {
            std::sort(p.begin(), p.end());
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            std::shared_ptr<DAGNode> cst = nullptr;
            for (const auto &pc : p) {
                if (pc->isCBV()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstBv(BitVectorUtils::bvOr(cst->toString(), pc->toString()), cst->getSort()->getBitWidth());
                }
                else if (!np.empty() && pc == np.back())
                    continue;
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (isOnes(cst))
                    return cst;
                else if (!isZero(cst))
                    np.emplace_back(cst);
            }

            if (np.empty())
                return mkConstBv(BitVectorUtils::intToBv(0, p.front()->getSort()->getBitWidth()), p.front()->getSort()->getBitWidth());
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_BV_XOR: {
            std::sort(p.begin(), p.end());
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            std::shared_ptr<DAGNode> cst = nullptr;
            for (const auto &pc : p) {
                if (pc->isCBV()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstBv(BitVectorUtils::bvXor(cst->toString(), pc->toString()), cst->getSort()->getBitWidth());
                }
                else if (!np.empty() && pc == np.back())
                    np.pop_back();
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (!isZero(cst))
                    np.emplace_back(cst);
            }

            if (np.empty())
                return mkConstBv(BitVectorUtils::intToBv(0, p.front()->getSort()->getBitWidth()), p.front()->getSort()->getBitWidth());
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }

        case NODE_KIND::NT_BV_COMP: {
            std::sort(p.begin(), p.end());
            p.erase(std::unique(p.begin(), p.end()), p.end());
            bool has_const = false;
            for (const auto &pc : p) {
                if (pc->isCBV()) {
                    if (has_const)
                        return mkFalse();
                    else
                        has_const = true;
                }
            }
            if (p.size() <= 1)
                return mkConstBv("#b1", 1);
            return nullptr;
        }
        case NODE_KIND::NT_BV_ADD: {
            std::shared_ptr<DAGNode> cst = nullptr;
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isCBV()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstBv(BitVectorUtils::bvAdd(cst->toString(), pc->toString()), cst->getSort()->getBitWidth());
                }
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (!isZero(cst))
                    np.emplace_back(cst);
            }
            if (np.empty())
                return mkConstBv(BitVectorUtils::intToBv(0, p.front()->getSort()->getBitWidth()), p.front()->getSort()->getBitWidth());
            else if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_BV_MUL: {
            std::shared_ptr<DAGNode> cst = nullptr;
            std::vector<std::shared_ptr<DAGNode>> np;
            np.reserve(p.size());
            for (const auto &pc : p) {
                if (pc->isCBV()) {
                    if (cst == nullptr)
                        cst = pc;
                    else
                        cst = mkConstBv(BitVectorUtils::bvMul(cst->toString(), pc->toString()), cst->getSort()->getBitWidth());
                }
                else
                    np.emplace_back(pc);
            }

            if (cst != nullptr) {
                if (isZero(cst))
                    return cst;
                else if (!isOne(cst))
                    np.emplace_back(cst);
            }
            if (np.size() == 1)
                return np.front();
            else if (np.empty())
                return mkConstBv(BitVectorUtils::intToBv(1, p.front()->getSort()->getBitWidth()), p.front()->getSort()->getBitWidth());
            p.swap(np);
            return nullptr;
        }

        case NODE_KIND::NT_BV_SADDO:
        case NODE_KIND::NT_BV_UADDO:
        case NODE_KIND::NT_BV_SMULO:
        case NODE_KIND::NT_BV_UMULO:
            return nullptr;
        case NODE_KIND::NT_BV_CONCAT: {
            // return nullptr;
            std::vector<std::shared_ptr<DAGNode>> np{p.front()};
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                if (p.at(i)->isCBV() && np.back()->isCBV()) {
                    // std::cout << np.back()->getSort()->getBitWidth() << ' ' << p.at(i)->getSort()->getBitWidth() << std::endl;

                    np.back() = (mkConstBv(BitVectorUtils::bvConcat(np.back()->toString(), p.at(i)->toString()),
                                           np.back()->getSort()->getBitWidth() + p.at(i)->getSort()->getBitWidth()));
                }
                else
                    np.emplace_back(p.at(i));
            }
            if (np.size() == 1)
                return np.front();
            else if (np.size() == 2) {
                if (isZero(np.front())) {
                    return mkBvZeroExt(np.back(), mkConstInt(np.front()->getSort()->getBitWidth()));
                }
            }
            p.swap(np);
            return nullptr;
        }

        case NODE_KIND::NT_FP_LE:
        case NODE_KIND::NT_FP_LT:
            return nullptr;
        case NODE_KIND::NT_FP_GE:
            t = NODE_KIND::NT_FP_LE;
            std::reverse(p.begin(), p.end());
            return rewrite(t, p);
        case NODE_KIND::NT_FP_GT:
            t = NODE_KIND::NT_FP_LT;
            std::reverse(p.begin(), p.end());
            return rewrite(t, p);
        case NODE_KIND::NT_FP_EQ:
            return nullptr;
        case NODE_KIND::NT_STR_CONCAT: {
            // return nullptr;
            std::vector<std::shared_ptr<DAGNode>> np{p.front()};
            for (size_t i = 1, isz = p.size(); i < isz; ++i) {
                if (p.at(i)->isCStr() && np.back()->isCStr()) {
                    std::string l_str = np.back()->toString();
                    std::string r_str = p.at(i)->toString();
                    if (l_str.back() == '\"' && r_str.front() == '\"')
                        np.back() = mkConstStr(l_str.substr(0, l_str.size() - 1) + r_str.substr(1));
                    else
                        np.back() = mkConstStr(l_str + r_str);
                }
                else if (p.at(i)->isCStr() && (p.at(i)->toString() == "\"\""))
                    continue;
                else
                    np.emplace_back(p.at(i));
            }
            // return nullptr;
            if (np.size() > 1 && np.front()->isCStr() && (np.front()->toString() == "\"\"")) {
                np.erase(np.begin());
            }
            if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_REG_CONCAT:
        case NODE_KIND::NT_REG_UNION:
        case NODE_KIND::NT_REG_INTER:
        case NODE_KIND::NT_REG_DIFF:
            return nullptr;
        case NODE_KIND::NT_FORALL:
        case NODE_KIND::NT_EXISTS:
            return nullptr;
        case NODE_KIND::NT_MAX: {
            std::shared_ptr<DAGNode> cst = nullptr;
            std::sort(p.begin(), p.end());
            p.erase(std::unique(p.begin(), p.end()), p.end());
            std::vector<std::shared_ptr<DAGNode>> np;
            for (const auto &pc : p) {
                if (pc->isCInt()) {
                    if (cst == nullptr)
                        cst = pc;
                    else if (cst->isCInt()) {
                        if (toInt(pc) > toInt(cst))
                            cst = pc;
                    }
                    else if (cst->isCReal()) {
                        if (toReal(pc) > toReal(cst))
                            cst = pc;
                    }
                }
                else if (pc->isCReal()) {
                    if (cst == nullptr)
                        cst = pc;
                    else if (toReal(pc) > toReal(cst))
                        cst = pc;
                }
                else {
                    if (pc->isConst())
                        echo_error("unexpected constant type in MIN operation!");
                    np.emplace_back(pc);
                }
            }
            if (cst != nullptr)
                np.emplace_back(cst);
            if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        case NODE_KIND::NT_MIN: {
            std::shared_ptr<DAGNode> cst = nullptr;
            std::vector<std::shared_ptr<DAGNode>> np;
            std::sort(p.begin(), p.end());
            p.erase(std::unique(p.begin(), p.end()), p.end());
            for (const auto &pc : p) {
                if (pc->isCInt()) {
                    if (cst == nullptr)
                        cst = pc;
                    else if (cst->isCInt()) {
                        if (toInt(pc) < toInt(cst))
                            cst = pc;
                    }
                    else if (cst->isCReal()) {
                        if (toReal(pc) < toReal(cst))
                            cst = pc;
                    }
                }
                else if (pc->isCReal()) {
                    if (cst == nullptr)
                        cst = pc;
                    else if (toReal(pc) < toReal(cst))
                        cst = pc;
                }
                else {
                    if (pc->isConst())
                        echo_error("unexpected constant type in MIN operation!");
                    np.emplace_back(pc);
                }
            }
            if (cst != nullptr)
                np.emplace_back(cst);
            if (np.size() == 1)
                return np.front();
            p.swap(np);
            return nullptr;
        }
        default: {
            std::cout << "NODEKIND: " << kindToString(t) << std::endl;
            echo_error("unexpect NODE_KIND in rewrite n-ary !!!");
            return nullptr;
        }
    }
}

std::shared_ptr<DAGNode> Parser::rewrite_oper(NODE_KIND &t, std::vector<std::shared_ptr<DAGNode>> &p) {
    // return nullptr;
    if (!options->getRewrite())
        return nullptr;
    if (getArity(t) > 0 && p.size() != getArity(t)) {
        throw std::runtime_error("error: params number not equal to arity !!!");
    }
    switch (t) {
        // zero-ary
        case NODE_KIND::NT_NULL:
        case NODE_KIND::NT_CONST:
        case NODE_KIND::NT_VAR:
        case NODE_KIND::NT_TEMP_VAR:
        case NODE_KIND::NT_CONST_TRUE:
        case NODE_KIND::NT_CONST_FALSE:
        case NODE_KIND::NT_CONST_PI:
        case NODE_KIND::NT_CONST_E:
        case NODE_KIND::NT_INFINITY:
        case NODE_KIND::NT_POS_INFINITY:
        case NODE_KIND::NT_NEG_INFINITY:
        case NODE_KIND::NT_NAN:
        case NODE_KIND::NT_EPSILON:
        case NODE_KIND::NT_POS_EPSILON:
        case NODE_KIND::NT_NEG_EPSILON:
        case NODE_KIND::NT_REG_NONE:
        case NODE_KIND::NT_REG_ALL:
        case NODE_KIND::NT_REG_ALLCHAR:
        case NODE_KIND::NT_QUANT_VAR:
            return nullptr;

        // unary
        case NODE_KIND::NT_CONST_ARRAY:
        case NODE_KIND::NT_NOT:
        case NODE_KIND::NT_NEG:
        case NODE_KIND::NT_ABS:
        case NODE_KIND::NT_SQRT:
        case NODE_KIND::NT_SAFESQRT:
        case NODE_KIND::NT_CEIL:
        case NODE_KIND::NT_FLOOR:
        case NODE_KIND::NT_ROUND:
        case NODE_KIND::NT_EXP:
        case NODE_KIND::NT_LN:
        case NODE_KIND::NT_LG:
        case NODE_KIND::NT_LB:
        case NODE_KIND::NT_SIN:
        case NODE_KIND::NT_COS:
        case NODE_KIND::NT_SEC:
        case NODE_KIND::NT_CSC:
        case NODE_KIND::NT_TAN:
        case NODE_KIND::NT_COT:
        case NODE_KIND::NT_ASIN:
        case NODE_KIND::NT_ACOS:
        case NODE_KIND::NT_ASEC:
        case NODE_KIND::NT_ACSC:
        case NODE_KIND::NT_ATAN:
        case NODE_KIND::NT_ACOT:
        case NODE_KIND::NT_SINH:
        case NODE_KIND::NT_COSH:
        case NODE_KIND::NT_TANH:
        case NODE_KIND::NT_SECH:
        case NODE_KIND::NT_CSCH:
        case NODE_KIND::NT_COTH:
        case NODE_KIND::NT_ASINH:
        case NODE_KIND::NT_ACOSH:
        case NODE_KIND::NT_ATANH:
        case NODE_KIND::NT_ASECH:
        case NODE_KIND::NT_ACSCH:
        case NODE_KIND::NT_ACOTH:
        case NODE_KIND::NT_TO_INT:
        case NODE_KIND::NT_TO_REAL:
        case NODE_KIND::NT_IS_INT:
        case NODE_KIND::NT_IS_PRIME:
        case NODE_KIND::NT_IS_EVEN:
        case NODE_KIND::NT_IS_ODD:
        case NODE_KIND::NT_FACT:
        case NODE_KIND::NT_BV_NOT:
        case NODE_KIND::NT_BV_NEG:
        case NODE_KIND::NT_BV_NEGO:
        case NODE_KIND::NT_FP_ABS:
        case NODE_KIND::NT_FP_NEG:
        case NODE_KIND::NT_FP_IS_NORMAL:
        case NODE_KIND::NT_FP_IS_SUBNORMAL:
        case NODE_KIND::NT_FP_IS_ZERO:
        case NODE_KIND::NT_FP_IS_INF:
        case NODE_KIND::NT_FP_IS_NAN:
        case NODE_KIND::NT_FP_IS_NEG:
        case NODE_KIND::NT_FP_IS_POS:
        case NODE_KIND::NT_FP_TO_REAL:
        case NODE_KIND::NT_STR_LEN:
        case NODE_KIND::NT_STR_TO_LOWER:
        case NODE_KIND::NT_STR_TO_UPPER:
        case NODE_KIND::NT_STR_REV:
        case NODE_KIND::NT_STR_IS_DIGIT:
        case NODE_KIND::NT_STR_FROM_INT:
        case NODE_KIND::NT_STR_TO_INT:
        case NODE_KIND::NT_STR_TO_REG:
        case NODE_KIND::NT_STR_TO_CODE:
        case NODE_KIND::NT_STR_FROM_CODE:
        case NODE_KIND::NT_REG_STAR:
        case NODE_KIND::NT_REG_PLUS:
        case NODE_KIND::NT_REG_OPT:
        case NODE_KIND::NT_REG_COMPLEMENT:
        case NODE_KIND::NT_BV_TO_NAT:
        case NODE_KIND::NT_BV_TO_INT:
        case NODE_KIND::NT_UBV_TO_INT:
        case NODE_KIND::NT_SBV_TO_INT:
        case NODE_KIND::NT_POW2:
            return rewrite(t, p.front());

        // binary
        case NODE_KIND::NT_POW:
        case NODE_KIND::NT_MOD:
        case NODE_KIND::NT_LOG:
        case NODE_KIND::NT_ATAN2:
        case NODE_KIND::NT_IS_DIVISIBLE:
        case NODE_KIND::NT_GCD:
        case NODE_KIND::NT_LCM:
        case NODE_KIND::NT_BV_UDIV:
        case NODE_KIND::NT_BV_SDIV:
        case NODE_KIND::NT_BV_UREM:
        case NODE_KIND::NT_BV_SREM:
        case NODE_KIND::NT_BV_UMOD:
        case NODE_KIND::NT_BV_SMOD:
        case NODE_KIND::NT_BV_SDIVO:
        case NODE_KIND::NT_BV_UDIVO:
        case NODE_KIND::NT_BV_SREMO:
        case NODE_KIND::NT_BV_UREMO:
        case NODE_KIND::NT_BV_SMODO:
        case NODE_KIND::NT_BV_UMODO:
        case NODE_KIND::NT_BV_SHL:
        case NODE_KIND::NT_BV_LSHR:
        case NODE_KIND::NT_BV_ASHR:
        case NODE_KIND::NT_BV_ULT:
        case NODE_KIND::NT_BV_ULE:
        case NODE_KIND::NT_BV_UGT:
        case NODE_KIND::NT_BV_UGE:
        case NODE_KIND::NT_BV_SLT:
        case NODE_KIND::NT_BV_SLE:
        case NODE_KIND::NT_BV_SGT:
        case NODE_KIND::NT_BV_SGE:
        case NODE_KIND::NT_NAT_TO_BV:
        case NODE_KIND::NT_INT_TO_BV:
        case NODE_KIND::NT_FP_REM:
        case NODE_KIND::NT_FP_SQRT:
        case NODE_KIND::NT_FP_ROUND_TO_INTEGRAL:
        case NODE_KIND::NT_SELECT:
        case NODE_KIND::NT_STR_PREFIXOF:
        case NODE_KIND::NT_STR_SUFFIXOF:
        case NODE_KIND::NT_STR_CHARAT:
        case NODE_KIND::NT_STR_SPLIT:
        case NODE_KIND::NT_STR_IN_REG:
        case NODE_KIND::NT_STR_CONTAINS:
        case NODE_KIND::NT_STR_NUM_SPLITS_RE:
        case NODE_KIND::NT_REG_RANGE:
        case NODE_KIND::NT_REG_REPEAT:
        case NODE_KIND::NT_BV_REPEAT:
        case NODE_KIND::NT_BV_ZERO_EXT:
        case NODE_KIND::NT_BV_SIGN_EXT:
        case NODE_KIND::NT_BV_ROTATE_LEFT:
        case NODE_KIND::NT_BV_ROTATE_RIGHT:
        case NODE_KIND::NT_FP_MIN:
        case NODE_KIND::NT_FP_MAX:
        case NODE_KIND::NT_BV_XNOR:
        case NODE_KIND::NT_BV_NAND:
        case NODE_KIND::NT_BV_NOR:
        case NODE_KIND::NT_BV_SUB:
            return rewrite(t, p.at(0), p.at(1));

        // ternary
        case NODE_KIND::NT_ITE:
        case NODE_KIND::NT_FP_ADD:
        case NODE_KIND::NT_FP_SUB:
        case NODE_KIND::NT_FP_MUL:
        case NODE_KIND::NT_FP_DIV:
        case NODE_KIND::NT_FP_TO_UBV:
        case NODE_KIND::NT_FP_TO_SBV:
        case NODE_KIND::NT_STORE:
        case NODE_KIND::NT_STR_SUBSTR:
        case NODE_KIND::NT_STR_INDEXOF:
        case NODE_KIND::NT_STR_UPDATE:
        case NODE_KIND::NT_STR_REPLACE:
        case NODE_KIND::NT_STR_REPLACE_ALL:
        case NODE_KIND::NT_STR_SPLIT_AT:
        case NODE_KIND::NT_STR_SPLIT_REST:
        case NODE_KIND::NT_STR_SPLIT_AT_RE:
        case NODE_KIND::NT_STR_SPLIT_REST_RE:
        case NODE_KIND::NT_STR_REPLACE_REG:
        case NODE_KIND::NT_STR_REPLACE_REG_ALL:
        case NODE_KIND::NT_STR_INDEXOF_REG:
        case NODE_KIND::NT_REG_LOOP:
        case NODE_KIND::NT_BV_EXTRACT:
            return rewrite(t, p.at(0), p.at(1), p.at(2));

        // 4-ary
        case NODE_KIND::NT_FP_FMA:
            // case NODE_KIND::NT_FP_TO_FP:
            return rewrite(t, p.at(0), p.at(1), p.at(2), p.at(3));

        // n-ary
        case NODE_KIND::NT_LE:
        case NODE_KIND::NT_LT:
        case NODE_KIND::NT_GE:
        case NODE_KIND::NT_GT:
        case NODE_KIND::NT_EQ:
        case NODE_KIND::NT_EQ_BOOL:
        case NODE_KIND::NT_EQ_OTHER:
        case NODE_KIND::NT_DIV_INT:
        case NODE_KIND::NT_DIV_REAL:
        case NODE_KIND::NT_STR_LT:
        case NODE_KIND::NT_STR_LE:
        case NODE_KIND::NT_STR_GT:
        case NODE_KIND::NT_STR_GE:
        case NODE_KIND::NT_DISTINCT:
        case NODE_KIND::NT_DISTINCT_BOOL:
        case NODE_KIND::NT_DISTINCT_OTHER:
        case NODE_KIND::NT_AND:
        case NODE_KIND::NT_OR:
        case NODE_KIND::NT_IMPLIES:
        case NODE_KIND::NT_XOR:
        case NODE_KIND::NT_ADD:
        case NODE_KIND::NT_MUL:
        case NODE_KIND::NT_IAND:
        case NODE_KIND::NT_SUB:
        case NODE_KIND::NT_BV_AND:
        case NODE_KIND::NT_BV_OR:
        case NODE_KIND::NT_BV_XOR:
        case NODE_KIND::NT_BV_COMP:
        case NODE_KIND::NT_BV_ADD:
        case NODE_KIND::NT_BV_MUL:
        case NODE_KIND::NT_BV_SADDO:
        case NODE_KIND::NT_BV_UADDO:
        case NODE_KIND::NT_BV_SMULO:
        case NODE_KIND::NT_BV_UMULO:
        case NODE_KIND::NT_BV_CONCAT:

        case NODE_KIND::NT_FP_LE:
        case NODE_KIND::NT_FP_LT:
        case NODE_KIND::NT_FP_GE:
        case NODE_KIND::NT_FP_GT:
        case NODE_KIND::NT_FP_EQ:
        case NODE_KIND::NT_STR_CONCAT:
        case NODE_KIND::NT_REG_CONCAT:
        case NODE_KIND::NT_REG_UNION:
        case NODE_KIND::NT_REG_INTER:
        case NODE_KIND::NT_REG_DIFF:
        case NODE_KIND::NT_FORALL:
        case NODE_KIND::NT_EXISTS:
        case NODE_KIND::NT_MAX:
        case NODE_KIND::NT_MIN:
            return rewrite(t, p);

        case NODE_KIND::NT_FP_TO_FP:  // 3 or 4
            return nullptr;

        default:
            return nullptr;
    }
}
}  // namespace stabilizer::parser