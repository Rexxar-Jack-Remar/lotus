//
// Created by ahmed on 1/13/2022.
//

#pragma once

#include "Dataflow/DemandAPA/Support.h"

struct SparseTable {
  int LOGN;
  vi A;
  vvi SpT;
  vvi mnIdx;
  int totMem;
  SparseTable(const vi &_A) {
    totMem = 0;
    A = _A;
    int n = A.size();
    LOGN = log2(n) + 1;
    SpT.assign(n, vi(LOGN << 1));
    mnIdx.assign(n, vi(LOGN << 1));
    totMem = 2 * n * (LOGN << 1);
    for (int i = 0; i < n; i++)
      SpT[i][0] = i, mnIdx[i][0] = i;
    for (int j = 1; (1 << j) <= n; ++j)
      for (int i = 0; i + (1 << j) - 1 < n; ++i)
        if (A[SpT[i][j - 1]] < A[SpT[i + (1 << (j - 1))][j - 1]])
          SpT[i][j] = SpT[i][j - 1], mnIdx[i][j] = mnIdx[i][j - 1];
        else
          SpT[i][j] = SpT[i + (1 << (j - 1))][j - 1],
          mnIdx[i][j] = mnIdx[i + (1 << (j - 1))][j - 1];
  }
  SparseTable() {}
  int query(int i, int j) {
    int k = floor(log2(j - i + 1));
    if (A[SpT[i][k]] <= A[SpT[j - (1 << k) + 1][k]])
      return SpT[i][k];
    else
      return SpT[j - (1 << k) + 1][k];
  }
  int queryIdx(int i, int j) {
    int k = floor(log2(j - i + 1));
    if (A[SpT[i][k]] <= A[SpT[j - (1 << k) + 1][k]])
      return mnIdx[i][k];
    else
      return mnIdx[j - (1 << k) + 1][k];
  }
};

