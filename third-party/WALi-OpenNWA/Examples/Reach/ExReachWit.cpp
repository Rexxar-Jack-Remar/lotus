/**
 * @author Nick Kidd
 * @version $Id$
 */

/*

   void foo() {
     return; // 2
   }
   int main() {
     int err = -5; // 8
     foo();        // 9
     err = 0;      // 10
     return 0;     // 12
   }

 */
#include "wali/wpds/WPDS.hpp"
#include "Reach.hpp"
#include "wali/wfa/State.hpp"
#include "wali/wfa/Trans.hpp"
#include "wali/witness/WitnessWrapper.hpp"
#include "wali/wpds/Config.hpp"
#include "wali/wpds/Rule.hpp"
#include "wali/wpds/fwpds/FWPDS.hpp"
#include <fstream>
#include <sstream>
#include <string>

void doReach()
{
  using wali::Key;
  using wali::wpds::WPDS;
  using wali::wfa::WFA;

  wali::sem_elem_t reachOne( new Reach(true) );
  Key p = wali::getKey("p");
  Key accept = wali::getKey("accept");
  Key n[13];
  for( int i=0 ; i < 13 ; i++ ) {
    std::stringstream ss;
    ss << "n" << i;
    n[i] = wali::getKey( ss.str() );
  }

  //wali::wpds::WPDS myWpds;
  wali::wpds::fwpds::FWPDS myWpds(new wali::witness::WitnessWrapper());
  // foo 
  myWpds.add_rule( p, n[0], p, n[1], reachOne);
  myWpds.add_rule( p, n[1], p, n[2], reachOne);
  myWpds.add_rule( p, n[2], p, reachOne);

  // main
  myWpds.add_rule( p, n[7],  p, n[8], reachOne);
  myWpds.add_rule( p, n[8],  p, n[9], reachOne);
  myWpds.add_rule( p, n[9],  p, n[0], n[10], reachOne);
  myWpds.add_rule( p, n[10], p, n[11], reachOne);
  myWpds.add_rule( p, n[11], p, reachOne);

  myWpds.print( std::cerr ) << '\n';
  std::ofstream fxml( "myWpds.xml" );
  myWpds.marshall( fxml );
  fxml.close();

  // Perform poststar query
  WFA query;
  query.addTrans( p, n[7], accept, reachOne );
  query.set_initial_state( p );
  query.add_final_state( accept );
  query.print( std::cerr << "BEFORE poststar\n" ) << '\n';
  WFA answer;
  myWpds.poststar(query,answer);
  answer.print( std::cerr << "\nAFTER poststar\n" ) << '\n';

}

int main()
{
  doReach();
  std::cerr << "# Trans : " << wali::wfa::Trans::numTrans << '\n';
  std::cerr << "# States : " << wali::wfa::State::numStates << '\n';
  std::cerr << "# Rules : " << wali::wpds::Rule::numRules << '\n';
  std::cerr << "# Configs : " << wali::wpds::Config::numConfigs << '\n';
  std::cerr << "# Reaches : " << Reach::numReaches << '\n';
  return 0;
}

