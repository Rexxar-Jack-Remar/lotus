/*!
 * @author Nick Kidd
 *
 * $Id$
 *
 * Tests WFA intersection.
 */
#include "wali/wfa/WFA.hpp"
#include "Reach.hpp"
#include "wali/wfa/State.hpp"
#include "wali/wfa/Trans.hpp"
#include "wali/wfa/WeightMaker.hpp"
#include <iostream>

void dot()
{
  using wali::wali_key_t;
  using wali::sem_elem_t;
  using wali::getKey;
  using wali::wfa::WFA;
  using std::cout;
  sem_elem_t R = new Reach(true);

  // p - n1 - q
  WFA fa1;
  wali_key_t p = getKey("p");
  wali_key_t q = getKey("q");
  fa1.set_initial_state( p );
  fa1.add_final_state( q );
  fa1.addTrans( p, getKey("n1"), q, R->one());

  fa1.print( cout << "WFA 1\n" ) << '\n';

  // m - n1 - q 
  WFA fa2;
  wali_key_t m = getKey("m");
  fa2.set_initial_state( m );
  fa2.add_final_state( q );
  fa2.addTrans( m,getKey("n1"),q,R->one());
  fa2.print( cout << "WFA 2\n" ) << '\n';


  fa1.intersect( fa2 ).print( cout << "1 \\cap 2\n" ) << '\n';

}

int main()
{
  dot();
  std::cerr << "# Trans : " << wali::wfa::Trans::numTrans << '\n';
  std::cerr << "# States : " << wali::wfa::State::numStates << '\n';
  std::cerr << "# Reaches : " << Reach::numReaches << std::endl;
  return 0;
}

