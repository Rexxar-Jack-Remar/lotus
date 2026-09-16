#pragma once

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::cfl::dynamic_dyck::detail {
using namespace std;

struct Matrix1 {
  unordered_map<unsigned, char> colors;
};

class CFLHashMap {
public:
  CFLHashMap(unsigned a) : invec(a), outvec(a), Size(a), nodedegree(a) {
    // vector< unordered_map<unsigned, char>* > abb[a];
    // for (unsigned i = 0; i < a; ++i)
    // nodevec[i] = new unordered_map<unsigned,char>(a);

    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             invec.begin();
         iter != invec.end(); ++iter) {
      *iter = new unordered_map<unsigned, Matrix1>;
    }
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             outvec.begin();
         iter != outvec.end(); ++iter) {
      *iter = new unordered_map<unsigned, Matrix1>;
    }
    for (vector<unsigned>::iterator iter = nodedegree.begin();
         iter != nodedegree.end(); ++iter) {
      *iter = 0;
    }
  }
  CFLHashMap(CFLHashMap &other, unsigned eidSize) {
    unsigned a = other.Size;
    invec = vector<unordered_map<unsigned, Matrix1> *>(a);
    outvec = vector<unordered_map<unsigned, Matrix1> *>(a);
    nodedegree = vector<unsigned>(a);
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             invec.begin();
         iter != invec.end(); ++iter) {
      *iter = new unordered_map<unsigned, Matrix1>;
    }
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             outvec.begin();
         iter != outvec.end(); ++iter) {
      *iter = new unordered_map<unsigned, Matrix1>;
    }
    for (vector<unsigned>::iterator iter = nodedegree.begin();
         iter != nodedegree.end(); ++iter) {
      *iter = 0;
    }
    this->Size = other.Size;
    for (unsigned j = 0; j < this->Size; j++) {
      unordered_map<unsigned, Matrix1> innodes;
      other.CheckInEdges(j, innodes);
      for (unordered_map<unsigned, Matrix1>::iterator itw = innodes.begin();
           itw != innodes.end(); ++itw) {
        unsigned i = itw->first;
        unordered_map<unsigned, char> color = (itw->second).colors;
        for (unordered_map<unsigned, char>::iterator c = color.begin();
             c != color.end(); ++c) {
          this->InsertEdge(i, j, c->first);
        }
      }
    }
  }
  ~CFLHashMap() {

    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             invec.begin();
         iter != invec.end(); ++iter) {
      delete *iter;
    }

    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             outvec.begin();
         iter != outvec.end(); ++iter) {
      delete *iter;
    }
  }

  // delete[] p;}

  unsigned GetVtxNum() const { return Size; }
  // unsigned GetSizeAfterDel() const {return SizeAfterDel;}
  unsigned GetEdgNum() {
    unsigned num = 0;
    // for (unsigned i = 0; i< Size; i++){
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             outvec.begin();
         iter != outvec.end(); ++iter) {

      for (unordered_map<unsigned, Matrix1>::iterator j = (**iter).begin();
           j != (**iter).end(); ++j) {
        num += j->second.colors.size();
      }
    }
    return num;
    //}
  }

  void InsertEdge(unsigned i, unsigned j, unsigned edgelabel) {
    // Bug fix: duplicates must not inflate the degree used by mainproc.
    if (HasEdgeBetween(i, j, edgelabel))
      return;

    if (outvec[i]->count(j) <= 0) { // can't find to
      Matrix1 tmp_q;
      tmp_q.colors[edgelabel] = 1;
      (*outvec[i])[j] = tmp_q;
    } else {
      (*outvec[i])[j].colors[edgelabel] = 1;
    }

    if (invec[j]->count(i) <= 0) { // can't find to
      Matrix1 tmp_q;
      tmp_q.colors[edgelabel] = 1;
      (*invec[j])[i] = tmp_q;
    } else {
      (*invec[j])[i].colors[edgelabel] = 1;
    }
    nodedegree[i]++;
    nodedegree[j]++;
  }

  void DeleteEdge(unsigned i, unsigned j, unsigned edgelabel) {
    // Bug fix: absent deletions must not underflow degree counters.
    if (!HasEdgeBetween(i, j, edgelabel))
      return;
    (*outvec[i])[j].colors.erase(edgelabel);
    (*invec[j])[i].colors.erase(edgelabel);
    if ((*outvec[i])[j].colors.empty())
      outvec[i]->erase(j);
    if ((*invec[j])[i].colors.empty())
      invec[j]->erase(i);
    nodedegree[i]--;
    nodedegree[j]--;
  }

  int HasEdgeBetween(unsigned i, unsigned j) {

    if (outvec[i]->count(j) <= 0) { // can't find to
      return 0;
    } else {
      return 1;
    }
  }

  int HasEdgeBetween(unsigned i, unsigned j, unsigned edgelabel) {

    if (outvec[i]->count(j) <= 0) { // can't find to
      return 0;
    } else {

      if ((*outvec[i])[j].colors.count(edgelabel))
        return 1;
      else
        return 0;
    }
  }

  void CheckOutEdges(unsigned i, unordered_map<unsigned, Matrix1> &outnodes) {

    outnodes = (*outvec[i]);
  }

  void CheckInEdges(unsigned i, unordered_map<unsigned, Matrix1> &innodes) {

    innodes = (*invec[i]);
  }

  void CheckInColor(unsigned j, unsigned i, unordered_map<unsigned, char> &c) {
    c = (*invec[i])[j].colors;
  }
  void CheckOutColor(unsigned i, unsigned j, unordered_map<unsigned, char> &c) {
    c = (*outvec[i])[j].colors;
  }
  // unsigned FindOutDegree(unsigned i);
  // unsigned FindInDegree(unsigned j);
  // void DeleteNode(unsigned i);
  // long ReturnTreeNodeAndDelete();
  // int DeleteAllEdgesAndJ(unsigned i, unsigned j); //returns 1 if there are
  // new A edges installed  -- needs improvment.
  unsigned GetNodeDegree(unsigned node) { return nodedegree[node]; }

  // Library adaptation: extend the fixed artifact graph without recomputation.
  void AddVertex() {
    invec.push_back(new unordered_map<unsigned, Matrix1>);
    outvec.push_back(new unordered_map<unsigned, Matrix1>);
    nodedegree.push_back(0);
    ++Size;
  }

  // Library initialization and cycle recovery retain the same adjacency type.
  void CopyFrom(CFLHashMap &other) {
    for (unsigned node = 0; node < Size; ++node) {
      invec[node]->clear();
      outvec[node]->clear();
      nodedegree[node] = 0;
    }
    for (unsigned source = 0; source < other.Size; ++source)
      for (const auto &target : *other.outvec[source])
        for (const auto &color : target.second.colors)
          InsertEdge(source, target.first, color.first);
  }

private:
  vector<unordered_map<unsigned, Matrix1> *> invec;
  vector<unordered_map<unsigned, Matrix1> *> outvec;
  unsigned Size;
  vector<unsigned> nodedegree;
  // unsigned SizeAfterDel;
};

} // namespace lotus::cfl::dynamic_dyck::detail
