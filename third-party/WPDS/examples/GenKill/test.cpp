#include "VarSet.h"
using std::cout;
using std::cerr;
using std::endl;
using std::string;

int main() {
  VarSet v, w, x, y;

  cout << VarSet::UniverseSet() << '\n';
  cout << VarSet::EmptySet() << '\n';

  v = VarSet::EmptySet();
  w = VarSet::EmptySet();
  cout << VarSet::Diff(v,w) << '\n';
  cout << VarSet::Diff(w,v) << '\n';
  cout << VarSet::Union(v,w) << '\n';
  cout << VarSet::Union(w,v) << '\n';
  cout << VarSet::Intersect(v,w) << '\n';
  cout << VarSet::Intersect(w,v) << '\n';
  cout << v << '\n';
  cout << w << '\n';
  cout << VarSet::Eq(v,w) << '\n';
  x = VarSet::Union(VarSet::Diff(v,w), VarSet::Union(VarSet::Diff(w,v), VarSet::Intersect(v,w)));
  y = VarSet::Union(v,w);
  cout << x << " == " << y << ": " << VarSet::Eq(x,y) << '\n';


  v.Insert("a" );
  v.Insert("b" );
  v.Insert("c" );
  v.Insert("d" );
  w = mkVarSet("c", "d", "e", "f");
  cout << VarSet::Diff(v,w) << '\n';
  cout << VarSet::Diff(w,v) << '\n';
  cout << VarSet::Union(v,w) << '\n';
  cout << VarSet::Union(w,v) << '\n';
  cout << VarSet::Intersect(v,w) << '\n';
  cout << VarSet::Intersect(w,v) << '\n';
  cout << v << '\n';
  cout << w << '\n';
  cout << VarSet::Eq(v,w) << '\n';
  x = VarSet::Union(VarSet::Diff(v,w), VarSet::Union(VarSet::Diff(w,v), VarSet::Intersect(v,w)));
  y = VarSet::Union(v,w);
  cout << x << " == " << y << ": " << VarSet::Eq(x,y) << '\n';

  return(0);
}
