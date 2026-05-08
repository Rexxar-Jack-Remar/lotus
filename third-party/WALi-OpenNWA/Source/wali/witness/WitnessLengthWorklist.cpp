/*!
 * @author Rich Joiner
 */

#include "wali/Common.hpp"
#include "wali/wfa/Trans.hpp"
#include "wali/witness/WitnessLengthWorklist.hpp"

namespace wali
{
  namespace witness
  {
    WitnessLengthWorklist::WitnessLengthWorklist() : PriorityWorklist<ShorterThan>()
    {
    }

    WitnessLengthWorklist::~WitnessLengthWorklist()
    {
    }
  } // namespace witness
} // namespace wali

