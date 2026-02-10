#pragma once

#include "IR/SVFG/SVFG.h"
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/STLExtras.h>

namespace llvm {

struct SVFGPairToNodeRef {
  lotus::analysis::SVFGNode *
  operator()(const std::pair<const uint32_t, lotus::analysis::SVFGNode *> &it) const {
    return it.second;
  }
};

struct SVFGPairToConstNodeRef {
  const lotus::analysis::SVFGNode *
  operator()(
      const std::pair<const uint32_t, lotus::analysis::SVFGNode *> &it) const {
    return it.second;
  }
};

template <> struct GraphTraits<lotus::analysis::SVFGNode *> {
  using NodeRef = lotus::analysis::SVFGNode *;
  using EdgeIter = std::vector<lotus::analysis::SVFGEdge *>::const_iterator;
  using ChildIteratorType =
      decltype(llvm::map_iterator(EdgeIter(), [](lotus::analysis::SVFGEdge *E) {
        return E->getDstNode();
      }));

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().begin(),
                              [](lotus::analysis::SVFGEdge *E) {
                                return E->getDstNode();
                              });
  }

  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().end(),
                              [](lotus::analysis::SVFGEdge *E) {
                                return E->getDstNode();
                              });
  }
};

template <> struct GraphTraits<const lotus::analysis::SVFGNode *> {
  using NodeRef = const lotus::analysis::SVFGNode *;
  using EdgeIter = std::vector<lotus::analysis::SVFGEdge *>::const_iterator;
  using ChildIteratorType = decltype(
      llvm::map_iterator(EdgeIter(), [](lotus::analysis::SVFGEdge *E) {
        return static_cast<const lotus::analysis::SVFGNode *>(E->getDstNode());
      }));

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().begin(),
                              [](lotus::analysis::SVFGEdge *E) {
                                return static_cast<const lotus::analysis::SVFGNode *>(
                                    E->getDstNode());
                              });
  }

  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().end(),
                              [](lotus::analysis::SVFGEdge *E) {
                                return static_cast<const lotus::analysis::SVFGNode *>(
                                    E->getDstNode());
                              });
  }
};

template <> struct GraphTraits<lotus::analysis::SVFG *> : GraphTraits<lotus::analysis::SVFGNode *> {
  using nodes_iterator =
      decltype(llvm::map_iterator(lotus::analysis::SVFG::iterator(),
                                  SVFGPairToNodeRef()));

  static lotus::analysis::SVFGNode *getEntryNode(lotus::analysis::SVFG *G) {
    return (G->begin() == G->end()) ? nullptr : G->begin()->second;
  }

  static nodes_iterator nodes_begin(lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->begin(), SVFGPairToNodeRef());
  }

  static nodes_iterator nodes_end(lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->end(), SVFGPairToNodeRef());
  }
};

template <>
struct GraphTraits<const lotus::analysis::SVFG *>
    : GraphTraits<const lotus::analysis::SVFGNode *> {
  using nodes_iterator =
      decltype(llvm::map_iterator(lotus::analysis::SVFG::const_iterator(),
                                  SVFGPairToConstNodeRef()));

  static const lotus::analysis::SVFGNode *
  getEntryNode(const lotus::analysis::SVFG *G) {
    return (G->begin() == G->end()) ? nullptr : G->begin()->second;
  }

  static nodes_iterator nodes_begin(const lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->begin(), SVFGPairToConstNodeRef());
  }

  static nodes_iterator nodes_end(const lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->end(), SVFGPairToConstNodeRef());
  }
};

} // namespace llvm
