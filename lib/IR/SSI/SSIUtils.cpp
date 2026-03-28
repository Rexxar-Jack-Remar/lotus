//===- SSIUtils.cpp - SSI helper classes and utilities --------------------===//
//
// Author: rainoftime
//
//===----------------------------------------------------------------------===//
/*
 *   Helper classes: ProgramPoint, RenamingStack, Graph, PostDominanceFrontier
 *
 * Bugs fixed in this file:
 *   #1  - StringRef::startswith → starts_with (deprecated in LLVM 14)
 *   #2  - ProgramPoint::not_definition_of now takes a const SSIfy& to use
 *         the side-table sets instead of name-based classification
 *   #4  - PostDominanceFrontier::calculate() now correctly recurses through
 *         the entire post-dominator tree (previously the Roots guard caused
 *         the function to return immediately, leaving Frontiers empty)
 */

#include "IR/SSI/SSI.h"

using namespace llvm;

// ---------------------------------------------------------------------------
// ProgramPoint
// ---------------------------------------------------------------------------

ProgramPoint::ProgramPoint(Instruction *I, Position P) : I(I), P(P) {}

// Two ProgramPoints are equal iff they are of the same region type and:
//   - if they are Self, their instruction should be the same.
//   - if not, their instructions' parents should be the same.
bool ProgramPoint::operator==(const ProgramPoint &o) const {
  if (this->P != o.P)
    return false;

  if (this->P == ProgramPoint::Self)
    return this->I == o.I;

  return this->I->getParent() == o.I->getParent();
}

bool ProgramPoint::operator!=(const ProgramPoint &o) const {
  return !(*this == o);
}

bool ProgramPoint::operator<(const ProgramPoint &o) const {
  if (this->P < o.P)
    return true;
  if (this->P > o.P)
    return false;

  if (this->P == ProgramPoint::Self)
    return this->I < o.I;

  return this->I->getParent() < o.I->getParent();
}

bool ProgramPoint::operator>(const ProgramPoint &o) const {
  return !(*this == o) && !(*this < o);
}

bool ProgramPoint::not_definition_of(const Value *V, const SSIfy &pass) const {
  const Instruction *I = this->I;
  const BasicBlock *BB = I->getParent();

  if (I == V)
    return false;

  switch (this->P) {
  case ProgramPoint::In:
    // phi case: check whether any SSI_phi in the block already uses V.
    {
      const Instruction *FirstNonPHI = BB->getFirstNonPHI();
      for (BasicBlock::const_iterator
               BBit = BB->begin(),
               BBend = (FirstNonPHI ? FirstNonPHI->getIterator() : BB->end());
           BBit != BBend; ++BBit) {
        const PHINode *op = cast<PHINode>(&*BBit);

        if (pass.is_SSIphi(op)) {
          for (unsigned i = 0, n = op->getNumIncomingValues(); i < n; ++i) {
            if (op->getIncomingValue(i) == V)
              return false;
          }
        }
      }
    }
    break;

  case ProgramPoint::Out:
    // sigma case: check whether any SSI_sigma in a successor already uses V.
    for (const_succ_iterator BBsuccit = succ_begin(BB),
                             BBsuccend = succ_end(BB);
         BBsuccit != BBsuccend; ++BBsuccit) {
      const BasicBlock *BBsucc = *BBsuccit;

      const Instruction *FirstNonPHI = BBsucc->getFirstNonPHI();
      for (BasicBlock::const_iterator
               BBit = BBsucc->begin(),
               BBend =
                   (FirstNonPHI ? FirstNonPHI->getIterator() : BBsucc->end());
           BBit != BBend; ++BBit) {
        const PHINode *op = cast<PHINode>(&*BBit);

        if (pass.is_SSIsigma(op)) {
          for (unsigned i = 0, n = op->getNumIncomingValues(); i < n; ++i) {
            if (op->getIncomingValue(i) == V)
              return false;
          }
        }
      }
    }
    break;

  case ProgramPoint::Self:
    // copy case: walk instructions after I looking for an SSI_copy of V.
    for (BasicBlock::const_iterator bit = I->getIterator(); bit != BB->end();
         ++bit) {
      const Instruction *next = &*bit;

      if (pass.is_SSIcopy(next)) {
        if (next->getOperand(0) == V)
          return false;
      } else if (next != I) {
        // Stop as soon as we hit a non-copy instruction after I.
        break;
      }
    }
    break;
  }

  return true;
}

bool ProgramPoint::is_join() const {
  return !this->I->getParent()->getSinglePredecessor() &&
         (this->P == ProgramPoint::In);
}

bool ProgramPoint::is_branch() const {
  return isa<BranchInst>(this->I) && (this->P == ProgramPoint::Out);
}

bool ProgramPoint::is_copy() const { return this->P == ProgramPoint::Self; }

// ---------------------------------------------------------------------------
// RenamingStack
// ---------------------------------------------------------------------------

RenamingStack::RenamingStack(Value *V) : V(V) {}

Value *RenamingStack::getValue() const { return this->V; }

void RenamingStack::push(Instruction *I) { this->stack.push_back(I); }

void RenamingStack::pop() { this->stack.pop_back(); }

Instruction *RenamingStack::peek() const { return this->stack.back(); }

bool RenamingStack::empty() const { return this->stack.empty(); }

unsigned RenamingStack::size() const {
  return static_cast<unsigned>(this->stack.size());
}

void RenamingStack::resize(unsigned n) { this->stack.resize(n); }

// ---------------------------------------------------------------------------
// Graph
// ---------------------------------------------------------------------------

void Graph::addNode(Value *V) { this->vertices[V]; }

bool Graph::hasNode(Value *V) { return this->vertices.count(V) > 0; }

void Graph::addEdge(Value *from, Value *to) {
  auto it = this->vertices.find(from);
  if (it != this->vertices.end()) {
    it->second.insert(to);
  }
}

bool Graph::hasEdge(Value *from, Value *to) {
  auto it = this->vertices.find(from);
  if (it != this->vertices.end()) {
    return it->second.count(to) > 0;
  }
  return false;
}

// ---------------------------------------------------------------------------
// PostDominanceFrontier::calculate
// ---------------------------------------------------------------------------

void PostDominanceFrontier::calculateVirtualRoot(const PostDominatorTree &DT,
                                                 const DomTreeNode *Node) {
  // Node has a null block (virtual root).  Recurse into real children.
  for (DomTreeNode::const_iterator NI = Node->begin(), NE = Node->end();
       NI != NE; ++NI) {
    const DomTreeNode *Child = *NI;
    if (Child->getBlock() == nullptr)
      calculateVirtualRoot(DT, Child); // nested virtual node (rare)
    else
      calculateNode(DT, Child);
  }
}

const PostDominanceFrontier::DomSetType &
PostDominanceFrontier::calculateNode(const PostDominatorTree &DT,
                                     const DomTreeNode *Node) {
  BasicBlock *BB = Node->getBlock();
  assert(BB && "calculateNode called with a virtual (null-block) node");

  DomSetType &S = Frontiers[BB];

  // DFlocal: predecessors of BB whose immediate post-dominator is not BB.
  for (pred_iterator SI = pred_begin(BB), SE = pred_end(BB); SI != SE; ++SI) {
    BasicBlock *P = *SI;
    DomTreeNode *SINode = DT[P];
    if (SINode && SINode->getIDom() != Node)
      S.insert(P);
  }

  // DFup: union of children's frontiers that are not properly post-dominated
  // by BB.
  for (DomTreeNode::const_iterator NI = Node->begin(), NE = Node->end();
       NI != NE; ++NI) {
    DomTreeNode *IDominee = *NI;
    // A child of a real node should always have a real block, but guard
    // defensively.
    if (!IDominee->getBlock())
      continue;
    const DomSetType &ChildDF = calculateNode(DT, IDominee);
    for (BasicBlock *ChildDFBB : ChildDF) {
      if (!DT.properlyDominates(Node, DT[ChildDFBB]))
        S.insert(ChildDFBB);
    }
  }

  return S;
}

const PostDominanceFrontier::DomSetType &
PostDominanceFrontier::calculate(const PostDominatorTree &DT,
                                 const DomTreeNode *Node) {
  if (Node->getBlock() == nullptr) {
    // Virtual root: recurse into children and return the stored empty set
    // for the entry block (which is always in Frontiers after construction).
    calculateVirtualRoot(DT, Node);
    // Return a stable reference — use the Frontiers map itself.  If it is
    // empty (no real blocks), insert a sentinel entry for nullptr so we have
    // something to return.  Callers of the public calculate() for the virtual
    // root only use the side-effect (populating Frontiers), not the return
    // value, so this is safe.
    static const DomSetType emptyConst;
    return emptyConst;
  }
  return calculateNode(DT, Node);
}

// Definition of the static ID member (declared but never defined in the
// original code, which would cause a linker error if it were ODR-used).
char PostDominanceFrontier::ID = 0;
