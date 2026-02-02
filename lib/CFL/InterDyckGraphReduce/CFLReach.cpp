#include "CFL/InterDyckGraphReduce/CFLReach.h"
//#include <bitset>
#include <deque>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <unordered_map>
#include <vector>

using namespace std;

int debug = 0;
int usearray = 1;

// should del the following two

// ifstream in("work");
// ifstream in("filelist/pldi2007/linux");
// ifstream in("dotfile/fop");
string version = "CFLreach";

// unsigned S_edge = 0;

double elapsed = 0.0;

void arrayreach(CFLHashMap &cm, vector<CFLGrammar> &CGVec) {

  struct timeval begin, end;
  unsigned NodeNum = cm.GetVtxNum();
  unsigned sedges = 0;
  //    unsigned edgelabels=9;

  queue<ReachabilityItem> worklist;
  gettimeofday(&begin, NULL);

  // cout<<"edge is "<<cm.GetEdgNum()<<'\n';

  // insert origian edge to reachQ
  for (unsigned i = 0; i < NodeNum; i++) {

    unordered_map<unsigned, Matrix1> outnodes;
    cm.CheckOutEdges(i, outnodes);

    for (unordered_map<unsigned, Matrix1>::iterator itj = outnodes.begin();
         itj != outnodes.end(); ++itj) {
      unsigned j = (itj->first);
      unordered_map<unsigned, char> color;
      cm.CheckOutColor(i, j, color);

      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        // cout<<"lala"<<'\n';
        // cout<<"[debug] "<<i<<" "<<j<<" c "<<c->first<<'\n';
        ReachabilityItem item(i, j, c->first);

        worklist.push(item);
      }

      // Matrix *q = p + i * NodeNum + j;
      // cout<<q->colors.count()<<'\n';
    }
  }

  // X = e phrase
  vector<CFLGrammar>::iterator it;
  for (it = CGVec.begin(); it != CGVec.end(); it++) { // for every grammar
    // cout<<"rules "<<(*it).IsEpsilonRule()<<'\n';

    if ((*it).IsEpsilonRule()) {
      // cout<<(*it).GetLTerm()<<'\n';
      // cout<<"is epsilone"<<'\n';
      unsigned edgTy = (*it).GetLTerm();
      for (unsigned i = 0; i < NodeNum; i++) {

        cm.InsertEdge(i, i, edgTy);
        if (edgTy == S_edge)
          sedges++;

        ReachabilityItem item(i, i, edgTy);
        worklist.push(item);
      }
    }
  }
  // cout<<"size is "<<cm.GetEdgNum()<<'\n';

  while (!worklist.empty()) {
    // cout << " " << worklist.front().GetEdgeTy();

    for (it = CGVec.begin(); it != CGVec.end(); it++) { // for every grammar
      unsigned i = worklist.front().GetVtx1();
      unsigned j = worklist.front().GetVtx2();
      unsigned wedg = worklist.front().GetEdgeTy();
      unsigned edgTy = (*it).GetLTerm();

      // it->PrintGrammar();

      // cout<<"processing "<<i<<" "<<j<<" c "<<wedg<<'\n';
      if ((*it).IsSingleRule() && !(*it).IsEpsilonRule()) { // handle A->B
        if ((*it).GetRTerm1() == wedg) {

          if (!cm.HasEdgeBetween(i, j, edgTy)) {

            cm.InsertEdge(i, j, edgTy);
            if (edgTy == S_edge)
              sedges++;

            // cout<<"insert "<<i<<" "<<j<<" c "<<edgTy<<'\n';
            ReachabilityItem item(i, j, edgTy);
            worklist.push(item);
          }
        }
      }

      else if ((*it).GetTermNum() == 3) {
        if ((*it).GetRTerm2() == wedg) { // handle A -> C B
          //	    for(unsigned vtx = 0; vtx < NodeNum; vtx++ ){

          unordered_map<unsigned, Matrix1> innodes;
          cm.CheckInEdges(i, innodes);

          for (unordered_map<unsigned, Matrix1>::iterator itj = innodes.begin();
               itj != innodes.end(); ++itj) {
            unsigned vtx = (itj->first);

            if (cm.HasEdgeBetween(vtx, i, (*it).GetRTerm1()) &&
                (!cm.HasEdgeBetween(vtx, j, edgTy))) {

              cm.InsertEdge(vtx, j, edgTy);

              ReachabilityItem item(vtx, j, edgTy);
              // cout<<"insert "<<vtx<<" "<<j<<" c "<<edgTy<<'\n';
              if (edgTy == S_edge)
                sedges++;

              // if(edgTy == M){
              // cout<<vtx<<" and "<<i<<" has "<<(*it).GetRTerm1()<<" while
              // "<<i<<" and "<<j<<" has "<<wedg<<" to form "<<edgTy<<'\n';
              // }

              worklist.push(item);
            }
          }
        }
        if ((*it).GetRTerm1() == wedg) { // handle A -> B C
          // for(unsigned vtx = 0; vtx < NodeNum; vtx++ ){

          unordered_map<unsigned, Matrix1> outnodes;
          cm.CheckOutEdges(j, outnodes);

          for (unordered_map<unsigned, Matrix1>::iterator itj =
                   outnodes.begin();
               itj != outnodes.end(); ++itj) {
            unsigned vtx = (itj->first);

            if (cm.HasEdgeBetween(j, vtx, (*it).GetRTerm2()) &&
                (!cm.HasEdgeBetween(i, vtx, edgTy))) {

              cm.InsertEdge(i, vtx, edgTy);
              if (edgTy == S_edge)
                sedges++;

              // cout<<"insert "<<i<<" "<<vtx<<" c "<<edgTy<<'\n';
              ReachabilityItem item(i, vtx, edgTy);
              // if(edgTy == M){
              // cout<<i<<" and "<<j<<" has "<<wedg<<" while "<<j<<" and
              // "<<vtx<<" has "<<(*it).GetRTerm2()<<" to form "<<edgTy<<'\n';
              // }
              worklist.push(item);
            }
          }
        }
      }
    }
    worklist.pop();

    // cout<<"poped"<<'\n';
  }

  gettimeofday(&end, NULL);
  elapsed += ((end.tv_sec - begin.tv_sec) +
              ((end.tv_usec - begin.tv_usec) / 1000000.0));
  // double elapsed = (end.tv_sec - begin.tv_sec) + ((end.tv_usec -
  // begin.tv_usec) / 1000000.0); cout<<"runtime: "<<elapsed<<'\n';

  // FindTreeNodes(cm);
  if (debug) {
    // cout<<"size " <<cm.GetEdgNum()<<'\n';

    unsigned sssize = 0;

    for (unsigned i = 0; i < NodeNum; i++) {
      for (unsigned j = i; j < NodeNum; j++) {

        if (i != j && (cm.HasEdgeBetween(i, j, S_edge) ||
                       cm.HasEdgeBetween(j, i, (unsigned)S_edge))) {
          sssize = sssize + 2;
          // msize++;
          cout << "i " << i << " j " << j << '\n';
          // cout<<"have "<<j<<'\n';
        }
        if (i == j)
          sssize++;

        // Matrix *q = p + i * NodeNum + j;
        // cout<<q->colors.count()<<'\n';
      }
    }
    // cout << "node: "<<cm.GetVtxNum()<<" longest length is "
    // <<maxlength<<'\n';

    // cout<<"msize "<<msize<<'\n';
    // cout<<"psize "<<psize<<'\n';
    cout << "s size " << sssize << '\n';
  }

  cout << "total S " << sedges << '\n';
}

int arrayversion() {

  cout << version << '\n';
  string line;

  while (getline(in, line)) { // for every file

    unsigned NodeNum;
    SimpleDotParser dotparser;

    unordered_map<string, unsigned> NodeID;
    unordered_map<string, unsigned> EdgeID;

    vector<CFLGrammar> CGVec;

    cout << "Processing " << line << '\n';
    // cout<<"doing "<<line<<'\n';
    NodeNum = dotparser.BuildNodeMap(line, NodeID);
    // cout<<"doing "<<line<<" of size "<<NodeNum<<'\n';
    // NodeNum = dotparser.BuildMatrix(line, NodeID);

    // CFLBitTable bt(NodeNum);
    // cout<<"node "<<NodeNum<<'\n';
    // CFLMatrix cm(NodeNum);
    // unsigned long abc = 100000;
    // cout<<"lala" << abc*abc<<'\n';
    // CFLMatrix cm(NodeNum);
    CFLHashMap cm1(NodeNum);

    /*if(NodeNum>nodemax){
      cout<<"max: "<<nodemax<<" new: "<<NodeNum<<"   "<<line<<'\n';
      nodemax = NodeNum;
      }*/

    dotparser.BuildHashTable(line, NodeID, EdgeID, CGVec, cm1);
    // dotparser.BuildMatrix(line, NodeID, cm1);
    // dotparser.BuildBitTable(line, NodeID, cm1);

    cout << "Node: " << cm1.GetVtxNum() << " Edge " << cm1.GetEdgNum() << '\n';
    cout << "Grammar: " << CGVec.size() << '\n';
    // cout<<"para: "<<EdgeID.size()<<'\n';

    arrayreach(cm1, CGVec);
  }
  // SimpleDotParser dotparser;
  // dotparser.ParsingFile("./data/CINT2000/254.gap/plist.o.lala.dot");

  cout << "Runtime: " << elapsed << '\n';

  MEM_USAGE();

  return 0;
}

int main() {

  // readgrammar();

  arrayversion();

  return 0;
}