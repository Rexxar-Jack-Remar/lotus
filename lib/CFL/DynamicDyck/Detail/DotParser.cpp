#include "CFL/DynamicDyck/Detail/DotParser.h"

#include <fstream>
#include <sstream>

namespace lotus::cfl::dynamic_dyck::detail {
pair<string, string> SimpleDotParser::ReturnNodePair(string &src,
                                                     const string &delimiter) {
  string from, to;

  StripExtra(src);
  // Bug fix: find the whole arrow, not '-' in a negative or textual ID.
  string::size_type delimiterstart = src.find(delimiter);
  //  istringstream fromstr(src.substr(0, delimiterstart));
  // istringstream tostr(src.substr(delimiterstart + delimiter.size()));

  from = src.substr(0, delimiterstart);
  to = src.substr(delimiterstart + delimiter.size());
  auto trim = [](string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
      text.clear();
      return;
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);
  };
  trim(from);
  trim(to);

  // fromstr>>from;
  // tostr>>to;

  return make_pair(from, to);

  //  string::size_type found = 0, start = 0;

  // string substring;

  /*
  do{

    found = src.substr(start).find_first_of(delimiter);
    //cout<<start<<endl;
    if (found == string::npos){
      dst.push_back(src.substr(start));
    }else{
      dst.push_back(src.substr(start, found));
      start = start + found + delimiter.size();
    }

    //substring = src.substr(found + 1);
    //cout<<"f " <<found<< " st " <<start<<endl;

    //cout<<"starting "<<start<<"after found = "<<found<<endl;




  }while(found != string::npos);

  */
}

unsigned SimpleDotParser::BuildNodeMap(
    const string &infile,
    unordered_map<string, unsigned> &NodeID) { // return node num.
  string line;
  ifstream in(infile.c_str());

  while (getline(in, line)) {

    if (IsEdge(line)) {

      string from, to;
      // EdgeTy edgTy = GetEdgeTy(line);
      // cout<<"line "<< line<< "of type"<<GetEdgeTy(line)<<endl;

      pair<string, string> nodes = ReturnNodePair(line, "->");
      from = nodes.first;
      to = nodes.second;
      // cout<<"from "<<from<<" to "<<to<<endl;

      if (NodeID.find(from) == NodeID.end()) { // can't find from
        unsigned id = NodeID.size();
        // cout<<from<<" should assign "<<id<<endl;
        NodeID[from] = id;
      }

      if (NodeID.find(to) == NodeID.end()) { // can't find to
        unsigned id = NodeID.size();
        // cout<<to<<" should assign "<<id<<endl;
        NodeID[to] = id;
      }
    }
  }

  return NodeID.size();
}

void SimpleDotParser::BuildMyHashTable(const string &infile,
                                       unordered_map<string, unsigned> &NodeID,
                                       unordered_map<string, unsigned> &EdgeID,
                                       CFLHashMap &cm) {

  string line;

  ifstream in(infile.c_str());

  // edgeTy edg;
  while (getline(in, line)) {
    // cout<<line<< "haha  " <<endl;
    if (IsEdge(line)) {

      string from, to;
      // EdgeTy edgTy = GetEdgeTy(line);
      string edgelabel = GetEdgeLabel(line);
      // int togrammar=0;
      string actuallabel = edgelabel.substr(1);

      // build edge map
      if (EdgeID.find(edgelabel) == EdgeID.end()) {
        unsigned id = EdgeID.size();
        EdgeID[edgelabel] = id;
        // togrammar=1;
      }

      // Matrix *q;
      // cout<<"line "<< line<< "of type"<<GetEdgeTy(line)<<endl;
      // cout<<"label "<<edgelabel<<" hash "<<EdgeID[edgelabel]<<endl;
      pair<string, string> nodes = ReturnNodePair(line, "->");
      if (edgelabel.find("cp") == 0) {
        from = nodes.second;
        to = nodes.first;
      } else {
        from = nodes.first;
        to = nodes.second;
      }
      cm.InsertEdge(NodeID[from], NodeID[to], EdgeID[actuallabel]);
    }
  }
}

} // namespace lotus::cfl::dynamic_dyck::detail
