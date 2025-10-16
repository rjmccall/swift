//===--- StackNesting.cpp - Utility for stack nesting  --------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/Test.h"
#include "llvm/Support/Debug.h"

using namespace swift;

/// Run the given function exactly once on each of the reachable blocks in
/// a SIL function. Blocks will be visited in a post-order consistent with
/// dominance, which is to say, after all dominating blocks but otherwise
/// in an unspecified order.
///
/// The function is passed a state value, which it can freely mutate. The
/// initial value of the state will be the same as the value left in the
/// state for an unspecified predecessor. For the entry block of the
/// function, this is the initial state passed to runInDominanceOrder.
///
/// Essentially, runInDominanceOrder finds an arbitrary simple path to
/// the block and runs the callback function for each block in that path
/// in order. As long as the callback:
/// - only looks at instructions in the current block and the blocks
///   it dominates,
/// - has no dependencies outside of the state (which must have "value
///   semantics"), and
/// - can handle the arbitrariness of the choice of path,
/// then the callback can act as if only the work done along the current
/// path has happened and ignore the impact of arbitrary visitation order.
///
/// This function assumes you don't change the CFG during its operation.
template <class Fn, class State>
void runInDominanceOrder(SILFunction &F, State &&state, const Fn &fn) {
  // The set of blocks that have ever been enqueued onto the worklist.
  // (We actually skip the worklist a lot, but *abstractly* they're
  // enqueued, and everything but the entry block does get added to this
  // set.)
  BasicBlockSet visitedBlocks(&F);

  // The next basic block to operate on. We always operate on `state`.
  SILBasicBlock *curBB = F.getEntryBlock();

  // We need to copy `state` whenever we enqueue a block onto the worklist.
  // We'll move-assign it back to `state` when we dequeue it.
  using StateValue = std::remove_reference_t<State>;
  SmallVector<std::pair<SILBasicBlock *, StateValue>> worklist;

  while (true) {
    // Run the function on the current block, updating the current state.
    fn(curBB, state);

    // Enqueue the successors.
    SILBasicBlock *nextBB = nullptr;
    for (SILBasicBlock *succBB : curBB->getSuccessorBlocks()) {
      // If this insertion returns true, we've already enqueued the
      // successor block, so we can skip it. This is fast enough because
      // of BasicBlockSet that there's no point in avoiding it for
      // single-predecessor blocks.
      if (!visitedBlocks.insert(succBB))
        continue;

      // If we haven't found a successor to visit yet, pick this one.
      if (!nextBB) {
        nextBB = succBB;

      // Otherwise, add it to the worklist, copying the current state.
      } else {
        worklist.emplace_back(succBB, /*copied*/ state);
      }
    }

    // If there's a viable direct successor, just continue along this
    // path, editing the current state in-place.
    if (nextBB) {
      curBB = nextBB;
      continue;
    }

    // Otherwise, if the worklist is empty, we're done.
    if (worklist.empty()) {
      return;
    }

    // Otherwise, pull the next item off the worklist and overwrite the
    // current state with the state we saved for it before.
    auto &nextItem = worklist.back();
    curBB = nextItem.first;
    state = std::move(nextItem.second);
    worklist.pop_back();
  }
}

/// Returns the stack allocation instruction for a stack deallocation
/// instruction.
static SILInstruction *getAllocForDealloc(SILInstruction *dealloc) {
  SILValue op = dealloc->getOperand(0);
  while (auto *mvi = dyn_cast<MoveValueInst>(op)) {
    op = mvi->getOperand();
  }
  return op->getDefiningInstruction();
}

/// Create a dealloc for a particular allocation.
///
/// This is expected to work for all allocations that don't have
/// properly-nested deallocations. It's fine to have a kind of allocation
/// that you can't do this for, as long as as it's always explicitly
/// deallocated on all paths. This pass doesn't change any allocations
/// or deallocations that are properly nested already.
///
/// Only allocations whose deallocations return true from canMoveDealloc
/// need to support this.
static void createDealloc(SILBuilder &B, SILLocation loc, SILInstruction *alloc) {
  switch (alloc->getKind()) {
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::AllocStackInst:
    assert((isa<AllocStackInst>(alloc) ||
            cast<PartialApplyInst>(alloc)->isOnStack()) &&
           "wrong instruction");
    B.createDeallocStack(loc, cast<SingleValueInstruction>(alloc));
    return;
  case SILInstructionKind::BeginApplyInst: {
    auto *bai = cast<BeginApplyInst>(alloc);
    assert(bai->isCalleeAllocated());
    B.createDeallocStack(loc, bai->getCalleeAllocationResult());
    return;
  }
  case SILInstructionKind::AllocRefDynamicInst:
  case SILInstructionKind::AllocRefInst:
    assert(cast<AllocRefInstBase>(alloc)->canAllocOnStack());
    B.createDeallocStackRef(loc, cast<AllocRefInstBase>(alloc));
    return;
  case SILInstructionKind::AllocPackInst:
    B.createDeallocPack(loc, cast<AllocPackInst>(alloc));
    return;
  case SILInstructionKind::BuiltinInst: {
    auto *bi = cast<BuiltinInst>(alloc);
    auto &ctx = alloc->getFunction()->getModule().getASTContext();

    switch (*bi->getBuiltinKind()) {
    case BuiltinValueKind::StackAlloc:
    case BuiltinValueKind::UnprotectedStackAlloc: {
      auto identifier =
        ctx.getIdentifier(getBuiltinName(BuiltinValueKind::StackDealloc));
      B.createBuiltin(loc, identifier,
                      SILType::getEmptyTupleType(ctx),
                      SubstitutionMap(), {bi});
      return;
    }
    default:
      llvm_unreachable("unknown stack allocation builtin");
    }
  }
  case SILInstructionKind::AllocPackMetadataInst:
    B.createDeallocPackMetadata(loc, cast<AllocPackMetadataInst>(alloc));
    return;
  default:
    llvm_unreachable("unknown stack allocation");
  }
}

namespace {
class ActiveAllocation {
  llvm::PointerIntPair<SILInstruction*, 1, bool> valueAndIsPending;

public:
  ActiveAllocation(SILInstruction *value) : valueAndIsPending(value, false) {}

  SILInstruction *getValue() const {
    return valueAndIsPending.getPointer();
  }

  bool isPending() const {
    return valueAndIsPending.getInt();
  }

  void setPending() {
    assert(!isPending());
    valueAndIsPending.setInt(true);
  }
};

struct State {
  // The active allocations and whether they're pending deallocation.
  SmallVector<ActiveAllocation, 4> allocations;

#ifndef NDEBUG
  SWIFT_ATTRIBUTE_NORETURN
  void abortForUnknownAllocation(SILInstruction *alloc,
                                 SILInstruction *dealloc) {
    llvm::errs() << "fatal error: StackNesting could not find record of "
                    "allocation for deallocation:\n  "
                 << *dealloc
                 << "Allocation might not be jointly post-dominated. "
                    "Current stack:\n";
    for (auto i : indices(allocations)) {
      llvm::errs() << "[" << i << "] "
                   << (allocations[i].isPending() ? "(pending) " : "")
                   << *allocations[i].getValue();
    }
    llvm::errs() << "Complete function:\n";
    alloc->getFunction()->dump();
    abort();
  }
#endif
};

} // end anonymous namespace

using IndexForAllocationMap = llvm::DenseMap<SILInstruction*, size_t>;

/// Flag that a particular allocation is pending.
static void setAllocationAsPending(State &state, SILInstruction *alloc,
                                   SILInstruction *dealloc,
                                   IndexForAllocationMap &indexForAllocation) {
  auto stack = MutableArrayRef(state.allocations);
  assert(!stack.empty());

  // Just ignore the top entry in all of this; we know it doesn't match
  // the allocation.
  assert(stack.back().getValue() != alloc);
  stack = stack.drop_back();

  // Ultimately, we're just calling setPending() on the entry matching
  // `alloc` in the allocations stack. All the complexity has to do with
  // trying to avoid super-linear behavior while also trying very hard
  // to avoid actually using indexForAllocation for simple cases.

  // It's very common for allocations to never be improperly nested,
  // so we don't want to eagerly add allocations to indexForAllocation
  // when we encounter them. This means we can't rely on it having
  // an entry for `alloc` now.

  // `alloc` is very likely to be close to the top of the stack. Just do
  // a short linear scan there first. This might be slightly slower than
  // a hash lookup in the worst case, but usually it means we can avoid
  // adding any entries to indexForAllocation at all. Even for this case
  // where nesting is broken, that's still worthwhile to do.
  const size_t linearScanLimit = 8;
  auto linearScanEntries = stack.take_back(linearScanLimit);
  for (auto &entry : linearScanEntries) {
    if (entry.getValue() == alloc) {
      entry.setPending();
      return;
    }
  }

  // Okay, so much for that, time for the hashtable.

#ifndef NDEBUG
  if (stack.size() <= linearScanLimit) {
    state.abortForUnknownAllocation(alloc, dealloc);
  }
#endif

  // We don't need to consider entries that we've already linearly scanned.
  stack = stack.drop_back(linearScanLimit);

  // Check if the entry's already in the hashtable.
  if (auto it = indexForAllocation.find(alloc); it != indexForAllocation.end()) {
    auto index = it->second;
    assert(stack[index].getValue() == alloc);
    stack[index].setPending();
    return;
  }

  // Fill in any missing entries in indexForAllocations.
  //
  // The invariant we maintain is that there may be allocations at the
  // top of the stack that aren't hashed, but once we reach a hashed
  // entry, everything beneath it is hashed. The first half of this
  // is necessary because we don't eagerly add allocations to the table,
  // but it's also what makes it okay that we skip the entries we
  // linearly scanned. The second half of this means that, if we start
  // adding entries from the top down, we can stop hashing once we find
  // that the entries we're adding are redundant. That's what keeps this
  // O(N).
  //
  // All of this caching is relying on us (1) never revisiting a block
  // and (2) never changing the active-allocations stack except via push
  // and pop.

  // Look for the target allocation index in this loop rather than doing
  // a hash lookup at the end.
  std::optional<size_t> foundIndexForAlloc;

  for (size_t onePast = stack.size(); onePast != 0; --onePast) {
    size_t entryIndex = onePast - 1;
    auto entryAlloc = stack[entryIndex].getValue();

    // Remember this if it's the allocation we're looking for.
    if (entryAlloc == alloc) {
      foundIndexForAlloc = entryIndex;
    }

    // Add this entry to the hashtable. Stop hashing as soon as this fails.
    auto insertResult = indexForAllocation.insert({entryAlloc, entryIndex});
    if (!insertResult.second) {
      continue;
    }
  }

#ifndef NDEBUG
  if (!foundIndexForAlloc) {
    state.abortForUnknownAllocation(alloc, dealloc);
  }
#endif

  stack[*foundIndexForAlloc].setPending();
}

/// Pop and emit deallocations for any allocations on top of the
/// active allocations stack that are pending deallocation.
///
/// This operation is called whenever we pop an allocation; it
/// restores the invariant that the top of the stack is never in a
/// pending state.
static void emitPendingDeallocations(State &state,
                                     SILInstruction *insertAfterDealloc,
                                     bool &madeChanges) {
  // The builder we use for inserting deallocations. Initialized lazily
  // to insert after the initial dealloc. We have to reuse the same
  // builder so that, if we pop multiple deallocations, we order them
  // correctly w.r.t each other.
  std::optional<SILBuilderWithScope> builder;

  while (!state.allocations.empty() &&
         state.allocations.back().isPending()) {
    auto entry = state.allocations.pop_back_val();
    SILInstruction *alloc = entry.getValue();

    // Create a builder if necessary.
    if (!builder) {
      // We want to use the location of (and inherit debug scopes from)
      // the initial dealloc that we're inserting after.
      builder.emplace(/*insertion point*/
                        std::next(insertAfterDealloc->getIterator()),
                      /*inherit scope from*/insertAfterDealloc);
    }

    createDealloc(*builder, insertAfterDealloc->getLoc(), alloc);
    madeChanges = true;
  }
}

/// The main entrypoint for clients.
///
/// We use a straightforward, single-pass algorithm:
///
///   enum AllocationStatus {
///     case allocated
///     case pendingDeallocation
///     case undeallocatable
///   }
///   struct State {
///     var stack = [(StackAllocationInst, AllocationStatus)]()
///   }
///   F.searchBlocksForJointPostDominance(initialState: State()) {
///     (block, state) in
///     for inst in block.instructions {
///       if inst.isStackAllocation {
///         state.stack.push((inst, .allocated))
///       } else if inst.isStackDeallocation {
///         let allocation = inst.stackAllocation
///         if state.stack.top == (A, .allocated) {
///           _ = state.stack.pop()
///           let builder = SILBuilder(insertAfter: I)
///           while !state.stack.isEmpty &&
///                 state.stack.top!.1 == .pendingDeallocation {
///             builder.createStackDeallocation(for: state.stack.pop().0)
///           }
///         } else {
///           I.remove()
///           let curStatus = state.findStatus(A)
///           assert(curStatus != .pendingDeallocation)
///           if curStatus == .allocated) {
///             state.setStatus(A, .pendingDeallocation)
///           }
///         }
///       }
///     }
///   }
///
/// The expectation is that searchBlocksForJointPostDominance performs
/// a depth-first search, passing a state that reflects the current
/// simple path from the entry block that is being explored. However,
/// there's a twist to this from a standard DFS; see below.
///
/// For the most part, the value in `state` at the end of processing
/// a block is the initial value of `state` at the start of processing
/// its successor as visited by the DFS, but this also has a twist;
/// see below.
///
/// The state consists of (1) an active stack of allocations which
/// haven't yet been deallocated on this path and (2) an active
/// status for each allocation.
///
/// It has four invariants:
///
/// 1. If A dominates B, A is below B on the allocation stack (is
///    closer to the front of the array).
/// 2. If a stack item has undeallocatable status, every item
///    below it also has undeallocatable status.
/// 3. The top of the stack never has pending status.
/// 4. If an allocation has pending status, there is an allocation
///    which it dominates that is still on the stack and which has
///    allocated status.
///
/// The twist about the search order and state relates to the
/// non-coherence of SIL's joint post-dominance requirement for stack
/// allocations. Specifically, SIL permits the current state of the
/// stack to vary on different edges into a dead-end region; this just
/// means it is not permitted to pop any of those deallocations from
/// the stack. (More allocations can be pushed and popped, but the
/// existing ones become untouchable.) StackNesting therefore permits
/// its input to be non-coherent: whether an allocation has been
/// deallocated is allowed to vary across the entries to a dead-end
/// region.
///
/// To handle this, the search delays considering edges into a dead-end
/// region until it has seen the last such edge. Furthermore, the
/// initial state of the destination block is set to a conservative
/// merger of the final states of all of the predecessors: if two
/// states differ in any way, the contents of the stack are pared
/// back to the common prefix, and all allocations are placed in the
/// undeallocatable state.
///
/// A proof of correctness follows.
///
/// Bear in mind that a depth-first search of a CFG corresponds to the
/// selection of a spanning tree T of that CFG, so when this proof talks
/// about states under a particular spanning tree, it really means
/// "while performing a depth-first search". The twist doesn't change that.
///
/// This proof contains several proofs by structural induction over the
/// strongly-connected component (SCC) tree:
///    P(entry block) &&
///    (\forall x: SCC .
///        (\forall y: SCC . y ancestor x => P(subgraph up to y)) =>
///            P(subgraph up to x)))
///        => P(complete CFG)
/// This works because the states within a block are defined inductively
/// (below) by the instructions and edges on various paths to that block
/// or its SCC, and those can only involve blocks in that SCC or its
/// ancestors in the SCC tree. The base case of the induction is always
/// the subgraph containing only the entry block, which is its own SCC.
///
/// Notation, conventions, terms, and background theory.
///   A CFG is a directed graph of points. In this graph, every
///   instruction behaves as an edge from the point before it to the
///   point after it, and every control flow edge behaves as an edge
///   from the point at the end of its source block to the point at
///   the end of its destination block. But when we say *edge*, we
///   always mean a control-flow edge between blocks, not this more
///   abstract sense.
///
///   X -> Y is a control flow edge from block X to block Y.
///
///   <I is the CFG point prior to an instruction I.
///   I> is the CFG point after an instruction I.
///   If I is immediately followed by J, then I> and <J are the same point.
///   <B is the CFG point at the start of a block B, which is the
///     same point as <I, where I is the first instruction in the block.
///   B> is the CFG point at the end of a block B, which is the same
///     point as I>, where I is the last instruction in the block.
///
///   A path through the CFG (sometimes called a walk: we are not
///   assuming non-repetition) is a sequence of points in the CFG
///   joined by either instructions or control-flow edges. A path
///   consisting of a single point is called the empty path.
///
///   A path is written as the sequence of the instructions and
///   control-flow edges that it passes through. In this example:
///     (P..., I, A -> B)
///   the path follows the subpath P (which must end at <I), then
///   passes through I, then passes through the control flow edge
///   from block A (which I must be in) to B.
///
///   A strongly-connected component (SCC) is a maximal set of blocks
///   such that, for any two blocks A and B in the set, there is a
///   path from the starting point of A to the starting point of B.
///   It is cyclic if there is a pair of such blocks such that the
///   path is not the empty path; otherwise it must be a singleton set
///   (but it can also cyclic if it is a singleton). The entry block
///   is always in an SCC by itself, and SCCs form a tree. SCC A is
///   an ancestor of SCC B if there is a path from the starting point
///   of some block in A to the starting point of some block in B.
///   A is a proper ancestor of B if it is an ancestor and not the same
///   SCC.
///
///   All of the theorems below are implicitly quantified over an
///   arbitrary spanning tree T unless they're *explicitly* quantified
///   over trees or marked tree-independent.
///
/// Definition.
///   Given a spanning tree T of the CFG:
///
///   An operation sequence is a sequence of states joined by operations
///   (see below). In a well-formed operation sequence:
///   - the initial state is the empty stack;
///   - for each operation on the path, the preceding state satisfies the
///     preconditions of the operation; and
///   - for each operation on the path, the following state is the result
///     of applying the operation to the preceding state.
///
///   Operation sequences are written using just their component operations,
///   since that uniquely determines the values of the states. But you can
///   sensibly talk about the state at any point in the sequence.
///
///   Given a path P from the entry point, the operation sequence OS(P)
///   corresponding to P (under T) is defined recursively as:
///
///   - If P is (), then
///       OS(P) := ()
///
///   - If P is (P'..., I), then let S' be the end state of OS(P').
///     - If I is an allocation A, then
///         OS(P) := (OS(P')..., PUSH@A)
///     - If I is a deallocation D of an allocation A, and A is at the
///       top of the stack in S', then
///         OS(P) := (OS(P')..., POP@D)
///     - If I is a deallocation D of an allocation A, and A is not
///       at the top of the stack in S' and is in the allocated state,
///       then
///         OS(P) := (OS(P')..., PEND@D)
///     - Otherwise
///         OS(P) := (OS(P')...)
///
///   - If P is (P', X -> Y), then let S' be the end state of OS(P').
///     - If Y is in a dead-end region R and X is not, and the
///       predecessor state set of R is not singleton, then
///         OS(P) := (OS(P')..., MERGE@(X->Y))
///     - Otherwise
///         OS(P) := (OS(P')...)
///
///   Note that the path must pass through an instruction or edge,
///   not just reach it, in order to include the corresponding operation.
///
///   (Hopefully it is obvious that this is the behavior of the algorithm
///   along a specific exploration path, at least re: state changes.)
///
///   The state at a point X (under T) is the end state of the operation
///   sequence corresponding to the path to X in T.
///
///   The end state of a block B (under T) is the state at the end point
///   of B (under T).
///
///   The predecessor state of a block B (under T) is the state at the
///   start point of B (under T).
///
///   The predecessor state set of a strongly-connected component C
///   (under T) is the set of end states (under T) of all blocks B that
///   are not in C but have an edge to a block in C.
///
/// The operations are:
///
/// PUSH@A. A is an allocation. The following state is the preceding
///   state, but with A added to the top of the stack with the allocated
///   status.
/// POP@D. D is a deallocation of an allocation A. The preceding state
///   must have A on the top of the stack with the allocated status.
///   The following state is the preceding state removing the top of
///   the stack and any subsequent allocations that are in the pending
///   status.
/// PEND@D. D is a deallocation of an allocation A. The preceding state
///   must have A not on the top of the stack, and A must have the
///   allocated status. The following state is the preceding state but
///    with A having the pending status.
/// MERGE@(X -> Y). Y must be in a dead-end region R, X must not be
///   in R, and the predecessor state set of R (under T) must not be
///   singleton. The following state is the conservative merger of
///   of the predecessor state set.
///
/// Note that the sensitivity of MERGE to T means that whether an
/// operation sequence is well-formed is T-specific. (In fact, it
/// isn't, but this must be proven.)
///
/// Lemma [scc-edge-partition]. (tree-independent)
///   If a path P contains an edge X -> Y, where X and Y are in
///   different SCCs, then all points visited by P prior to that
///   edge are in proper ancestors of YC, the SCC of Y, and all points
///   visited by P after that edge are not in proper ancestors of YC.
/// Pf.
///   By definition, SCC A is an ancestor of SCC B if there exists a
///   path from some block in A to some block in B, and they are proper
///   ancestors if A and B are different SCCs.
///
///   Let PX be the prefix of P prior to the X -> Y edge. For all points
///   W visited by P prior to the edge, let PWX be the suffix of PX
///   beginning at W. Then (PWX..., X -> Y) is a path from W to a
///   block in Y, so W is in an ancestor SCC of YC. If W were in YC,
///   then for every node Z in YC, there would be a path PZW from Z
///   to W and a path PYZ from Y to Z; but then (PZW..., PWX...)
///   would be a path from Z to X, and (X -> Y, PYZ...) would be a
///   path from X to Z, so X would also be in SCC, which it is not.
///   Therefore W is not in YC, and its SCC must be a proper ancestor.
///
///   Let PY be the suffix of P after the X -> Y edge. For all points
///   Z visited by P after the edge, let PYZ be the prefix of PY
///   ending at Z. Then PYZ is a path from Y to Z, so YC must be
///   an ancestor SCC of the SCC of Z, and so the SCC of Z cannot
///   also be a proper ancester of YC or else there would be a cycle
///   in the SCC graph.
///
/// Lemma [no-cyclic-deallocation]. (tree-independent)
///   A cyclic SCC C cannot pass through a deallocation D for an
///   allocation A that is not in C.
/// Pf.
///   Let C, D, and A be given, and let P be a path to D>.
///   The entry block is never a cyclic SCC, so P must pass
///   through exactly one edge into C:
///     P = (P_EX..., X -> Y, P_YD...)
///   such that X is not in C and Y is in C. Then P_YD lies
///   entirely within C. Because C is a cyclic SCC, there must
///   exist a path P_DY from D> to <Y which also lies entirely
///   within C. Then
///     (P_EX..., X -> Y, P_YD..., P_DY..., P_YD...)
///   is a (non-simple) path that two deallocations of A. This
///   path cannot pass through A between the deallocations
///   because that portion of the path lies entirely within C,
///   and A is not in C. This violates the joint post-dominance
///   rule of deallocation.
///
/// Lemma [no-scc-self-path-deallocation]. (tree-independent)
///   A path P from <X to <Y, where X and Y are two blocks
///   (possibly the same) in the same SCC, can never pass
///   through a deallocation of an allocation from a different SCC.
/// Pf.
///   If the SCC is not cyclic, then X and Y must be the same
///   block, and the only possible such path is empty and cannot
///   pass through anything. Otherwise, it is cyclic and
///   [no-cyclic-deallocation] applies.
///
/// Lemma [absence-is-permanent].
///   If a path P in T passes through an allocation A, and there
///   is a point X following A on P where the state at X does not
///   include A, then the state at the end of P must also not
///   include A.
/// Pf.
///   The only operation that can add an allocation that is not
///   present in the preceding state is PUSH, so if the state
///   at the end of P contains A, there must be a PUSH that adds A
///   in the suffix of P beginning at X. This PUSH in OS(P) must
///   correspond to visiting A itself in P, but by assumption, P
///   already passes through A prior to reaching X. This would mean
///   that P visits A twice, but P is a path in the spanning tree T
///  and must be simple.
///
/// Lemma [undeallocatable-is-permanent].
///   If a path P in T passes through an allocation A, and there
///   is a point X following A on P where the state at X gives A
///   undeallocatable status, then the state at the end of P must
///   either not include A or give it undeallocatable status.
/// Pf.
///   POP and PEND do not affect allocations that have
///   undeallocatable status, and MERGE can only either leave
///   it in undeallocatable status or remove it from the
///   the state completely. So the only way that the state
///   at the end of P can include A with a status other than
///   deallocatable is if there is a PUSH that adds it; but
///   as above, this must correspond to P passing through A
///   for a second time on a simple path, which is impossible.
///
/// Lemma [pending-is-permanent].
///   If a path P in T passes through an allocation A, and there
///   is a point X following A on P where the state at X gives A
///   pending status, then the state at the end of P must either
///   not include A, give it pending status, or give it undeallocatable
///   status.
/// Pf.
///   If the state at the end of P includes A and does not give it
///   pending or undeallocatable status, it must give it allocated
///   status. The only operation that can do this when the allocation
///   is absent or has a non-allocated status in the preceding state
///   is PUSH, and as above, this would require P visiting A twice
///   and is impossible.
///
/// Lemma [deallocation-is-permanent].
///   If a path P in T passes through an deallocation D of
///   allocation A, then the state at the end of P either does not
///   include A or gives it a status other than allocated.
/// Pf.
///   Let P and A be given which satisfy the conditions, and let D
///   be a deallocation of D that P passes through. A
///   dominates D, so A must appear earlier on P than D.
///
///   If A is present and has allocated status in the state at <D,
///   then D will correspond either to a POP or PEND operation. The
///   state at D> will therefore either omit A or give it pending
///   status. Otherwise, the state at <D must either omit A or
///   give it either pending or undeallocatable status. By
///   [absence-is-permanent], [undeallocatable-is-permanent],
///   and [pending-is-permanent], the state at the end of the
///   path must either not include A or give it a status
///   other than permanent.
///
/// Lemma [allocated-implies-allocation].
///   If the state S at some point X gives an allocation A allocated
///   status, then the path to X in T passes through A and does not
///   pass through a deallocation of A.
/// Pf.
///   Let P be the path to X. The initial state of OS(P) does not
///   contain A, but in the final state of OS(P) it has allocated
///   status; there must exist an earliest operation O in OS(P)
///   such that the state following O gives A allocated status.
///   Only PUSH can give an allocation allocated status. By the
///   requirements of PUSH, O must correspond to A. Therefore
///   P passes through A.
///
///   If P also passed through a deallocation of A, then the
///   state at the end of P could not include A and give it
///   allocated status by [deallocation-is-permanent]. So P
///   does not pass through a deallocation of A.
///
/// Lemma [pending-implies-deallocation].
///   If the state S at some point X gives an allocation A pending
///   status, then the path to X in T passes through a deallocation
///   of A which corresponds to a PEND operation on the operation
///   sequence for that path.
/// Pf.
///   Let P be the path to X in T. By definition, S is the final state
///   in OS(P). The empty stack does not give A pending status, so
///   there is an earliest operation O in OS(P) such that the following
///   state of O gives A pending status. Only PEND@D can make such a
///   state change. D must be a deallocation of A, and by construction
///   P passes through D.
///
/// Lemma [pop-implies-deallocated].
///   Let O == POP@D be part of the operation sequence corresponding
///   to some path P in T. If O removes an allocation A from its
///   preceding state, then P passes through a deallocation of A.
/// Pf.
///   To be removed by a POP operation, either A must be the top of
///   the stack or A has pending status.
///
///   If A has pending status, let PD be the subpath of P to <D.
///   By [pending-implies-deallocation], P passes through a
///   deallocation of A; since PD is subpath of P, P also passes
///   through this deallocation.
///
///   If A is the top of the stack, then D must itself be a
///   deallocation of A, and by construction P passes through D.
///
/// Lemma [merge-implies-restricting-path].
///   Let O == MERGE@(M -> N) be part of the operation sequence
///   corresponding to some path in T, and let R be the dead-end
///   region containing N. If O removes an allocation A from its
///   preceding state, then there exists a *restricting path* P
///     P := (P'..., X -> Y)
///   such that
///   - P' is a path from the entry point to X>,
///   - P' visits only nodes in ancestor SCCs of R,
///   - Y is in R, and
///   - the end state of X does not include A.
/// Pf.
///   The set of allocations in the successor state of O is the
///   intersection of the sets of allocations in the predecessor
///   state set of R. If A is not in this intersection, there must
///   exist an an intersected set for which A is not in the set.
///   This intersected set is, by definition, the set of allocations
///   in the end state of some block X such that X is not in R but
///   X has an edge to some block Y in R. Let P' be the path to X>
///   in T.
///
///   P' is a path from the entry point to X>. Since X is not in R,
///   but has an edge into it, P' visits only ancestor SCCs of R.
///   There is a node Y in R such that Y is not in R. And the end
///   state of X does not include A. Therefore the path
///   (P'..., X -> Y) satisfies the requirements.
///
/// Lemma [absent-implies-missing-or-deallocation].
///   If the state S at some point X does not include an allocation
///   A, then either there exists a path to X that does not pass
///   through A or there exists a path to X that passes through both
///   A and a deallocation of A.
/// Pf.
///   By structural induction on the SCC tree.
///
///   In all cases, let P be the path from the entry point to X in T.
///   If P does not pass through A, then P satisfies the requirement.
///   Otherwise, let OS be the operation sequence corresponding to P.
///   Since P passes through A, OS includes PUSH@A. The state immediately
///   following PUSH@A must contain A, but the final state S does not,
///   so there must be some final operation in OS for which the
///   preceding state contains A. This operation must be either
///   POP or MERGE because the others never remove allocations from
///   the state. If it is POP, then by [pop-implies-deallocated],
///   there must be a deallocation of A on P, and so P satisfies
///   the requirement. So in the induction we need only consider the
///   case where P passes through A and is removed by a MERGE.
///
///   The base case is that X is in the entry block. P therefore
///   cannot contain any edges, and so P cannot contain a MERGE.
///
///   Now, let X be a point in some SCC C, and assume that the lemma
///   holds for any point in an ancestor of C. We have eliminated
///   cases such that we know that P passes through A and is removed
///   from the state by the operation MERGE@(MS -> MD), such that MD
///   is in some dead-end region R and MS is not. By
///   [merge-implies-restricting-path], there exists a restricting
///   path PR:
///     PR := (PR'..., PRS -> PRD)
///   PRS is not in R, so it is in an ancestor of R, so it is necessarily
///   in an ancestor of the SCC containing X. Because the end state
///   of PRS does not include A, we can apply the inductive hypothesis
///   to find a path PI ending in PRS> that either does not include A
///   or includes both A and a deallocation of A.
///
///   Therefore:
///   - PR' is a path from the entry point to PRS>.
///   - There is an edge PRS -> PRD.
///   - Both PRD and MD are in R, which is a strongly-connected
///     component, so there must exist a path PMD from <PRD to <MD.
///   - The suffix PS of P following MS -> MD is a path from <MD to X.
///
///   The concatenation PC := (PR'..., PRS -> PRD, PMD..., PS...) is
///   therefore a well-formed path from the entry point to X. 
///
///   Because PRD is in R and PRS is not, by [scc-edge-partition]
///   the blocks visited by PR' must be in proper ancestors of R and
///   the blocks visited by (PMD..., PS...) must not be. But A occurs
///   prior to the edge MS -> MD on P, and so by [scc-edge-partition]
///   A must be in a proper ancestor SCC of R. Therefore the subpath
///   (PMD..., PS...) does not visit A.
///
///   We know by the inductive hypothesis that PR' either passes through
///   both A and a deallocation of A or does not pass through A at all.
///   If PR' passes through A and a deallocation of A, then PC passes
///   through them as well because PR' is a subpath of PC; therefore
///   PC satisfies the lemma's requirements in this case. But if PR'
///   doesn't pass through A, then PC doesn't either, because the
///   remaining subpath of PC never visits A; therefore PC also satisfies
///   the lemma's requirements in this case. So the induction step
///   is confirmed, and the lemma holds for all points.
///
/// Lemma [deallocation-preconditions].
///   In the state S at the point before a deallocation D of an
///   allocation A, A is on the stack and does not have pending status.
/// Pf.
///   Let P be the path to <D in T. P must pass through A because
///   allocations must dominate their deallocations. P is a simple
///   path because T is a spanning tree, so P can pass through A
///   at most once. 
///
///   Suppose that S does not include A. Then by
///   [absent-implies-missing-or-deallocation], there is a path
///   P' to <D which either does not pass through A or passes
///   through both A and a deallocation of A. In the latter case,
///   (P'..., D) is a path from the entry point that includes A
///   and two successive deallocations of A, which violates the
///   rules of joint post-dominance. In the former case,
///   (P'..., D) is a path from the entry point that includes
///   a deallocation of A but not A, which violates the rules
///   of dominance. So S must include A.
///
///   Suppose that S gives A pending status. Then by
///   [pending-implies-deallocation], there is a deallocation of A
///   on P. But then (P..., D) is a path from the entry point
///   that includes A and two successive deallocations of A,
///   which violates the rules of joint post-dominance. So S
///   cannot give A pending status.
///
/// Theorem [correctness-invariant-1].
///   If allocation A dominates allocation B, then all points X,
///   if the state S at X contains both A and B, A is below B in
///   the stack.
/// Pf.
///   Let a point X be given, and let P be the path to X in T.
///   If OS(P) includes PUSH@B, P must pass through B. If A
///   dominates B, then P must pass through A before it passes
///   through B, and so PUSH@A must precede PUSH@B in OS(P). There
///   are no operations that reorder elements in the stack, just
///   add and remove at the top, so if A and B are both still
///   present in the stack at X, A must have been pushed first
///   and so will be below B.
///
/// Theorem [correctness-invariant-2].
///   For all points X, if the state S at X contains an allocation
///   A having undeallocatable status, every allocation below A on
///   the stack at S also has undeallocatable status.
/// Pf.
///   Let a well-formed operation sequence be given.
///   Allocations are only given undeallocatable status by
///   MERGE operations, which give it to all allocations
///   left on the stack. Allocations are never subsequently
///   reordered. So the undeallocatable allocations are always
///   a strict prefix of the stack.
///
/// Theorem [correctness-invariant-3].
///   For all points X, if the state S at X is not the empty stack,
///   the top of the stack at S does not have pending status.
/// Pf.
///   No well-formed operation sequence ever leaves the top of
///   the stack having pending status. We prove this by structural
///   induction on the sequence.
///
///   In the base case, the end state of the sequence is the empty
///   stack, which contains no allocations.
///
///   Let an OS := (OS'..., O) be given, and assume the induction
///   hypothesis is true for OS'; therefore the end state of OS'
///   does not give the top of the stack (if any) pending status.
///   Now consider O:
///   - If O is PUSH, the end state of OS gives the top allocation
///     allocated status.
///   - If O is POP, the end state of OS removes the top allocation
///     and then iteratively removes all allocations in the pending
///     status; the top allocation in the end state of OS can never
///     have pending status, or it would have been popped as well.
///   - If O is PEND, then the status of the top allocation in the
///     end state of OS is the same as it was in the end state of
///     OS', which by the induction hypothesis was not pending.
///   - If O is MERGE, then the end state of OS gives every
///     allocation remaining on the stack undeallocatable status,
///     so the top allocation (if any) cannot have pending status.
///
///   Therefore this is true for all well-formed operation
///   sequences, and since S is by definition the end-state of a
///   well-formed operation sequence, S does not give the top of
///   the allocation stack pending status.
///
/// Lemma [simple-path-dominance-transition]. (tree-independent)
///   For all simple paths P and points X, Y, and Z, if X precedes
///   Y on P, Y precedes Z on P, and X does not dominate Y, then
///   X also does not dominate Z.
/// Pf.
///   Let P, X, Y, and Z be given that satisfy the conditions.
///   Let P_YZ be the subpath of P from Y to Z. Because P is a simple
///   path and X occurs earlier on it, X cannot occur on P_YZ.
///   As X does not dominate Y, there must be a path P_EY from the
///   entry point to Y that does not pass through X. Then the path
///   (P_EY..., P_YZ...) is a path from the entry point to Z that
///   does not pass through X, meaning X does not dominate Z.
///
/// Lemma [nested-allocations-are-dominated].
///   If a path P in T passes through an allocation A, then
///   for all points X on P dominated by A, all of the allocations
///   on the stack in the state at X are dominated by A.
/// Pf.
///   Let A, P, and X be given. If an allocation B is on the stack
///   in the state at X, B must precede X in P. Meanwhile, if B
///   is above A on the stack at X, then PUSH@B must appear after
///   PUSH@A in OS(P), so P must pass through B after it passes
///   through A. A must therefore dominate B, or else A would not
///   dominate X by [simple-path-dominance-transition], which
///   contradicts our assumptions.
///
/// Theorem [correctness-invariant-4].
///   For all points X, for every allocation A in the state S at X,
///   if A has pending status in S, then S also contains an
///   allocation B dominated by A which S gives allocated status.
/// Pf.
///   Let T and X be given, and let P be the path to X in T.
///
///   By induction on P. In the base case, P is the empty path,
///   so S is the empty stack. Since it contains no allocations,
///   it satisfies the theorem trivially.
///
///   Now suppose that P is either (PI..., I) or (PI..., X -> Y).
///   Let SI be the state at the end point of PI; the inductive
///   hypothesis tells us that the theorem holds for SI.
///   If S = SI, then the theorem also holds for S. Otherwise,
///   the final component of P must correspond to an operation O
///   which changes the state from SI to S. We proceed by cases:
///
///   O is PUSH@A: S is exactly SI, but with A pushed onto the stack
///     and having allocated status. So let an allocation A' in S
///     be given. If A' = A, then A' does not have pending status in
///     S. Otherwise, SI must also have A' with pending status,
///     and so there is a B in SI dominated by A that has allocated
///     status. B must still exist in S with the same status.
///
///   O is MERGE@(X -> Y): All the allocations in S must have
///     undeallocatable status, so the theorem holds trivially.
///
///   O is PEND@D: Let A be the allocation deallocated by D.
///     A dominates D, so P must pass through A. Therefore, by
///     [nested-allocations-are-dominated], all of the allocations
///     above A on the stack in the state at <D (i.e. SI) are
///     dominated by A. Furthermore, the allocation AT at the top of
///     the stack in SI must have allocated status:
///     - AT does not have pending status in SI by
///       [correctness-invariant-3].
///     - AT does not have undeallocatable status in SI by
///       [correctness-invariant-2], since A has allocated status
///       in SI and is beneath B on the stack.
///     AT != A because the allocation whose status is changed by
///     PEND cannot be the top of the stack. Therefore the status
///     of AT is not changed by O, so it still has allocated status
///     in S.
///
///     Now let an allocation A' in S be given.
///
///     If A' = A: AT satisfies the conditions for the theorem.
///
///     If A' != A: the status of A' is not changed by O, so if
///     A' has pending status in S, it must have pending status
///     in SI. Then by the inductive hypothesis, let B be an
///     allocation in SI dominated by A' that has allocated
///     status in SI. If B != A, then the status of B is not
///     changed in S, so B still satisfies the conditions of
///     the theorem. Otherwise, B = A, so we now know that A'
///     dominates A, and since dominance is transitive, A' must
///     dominate AT. Since AT has allocated status in S, it
///     satisfies the conditions of the theorem.
///
///   O is POP@D: Let A be the allocation deallocated by D.
///     O removes A from S, as well as some number of other
///     allocations that all have pending status in SI.
///
///     Let A' be an allocation in S that has pending status
///     in S. O does not give any allocations pending status,
///     so A' must also have pending status in SI. By the
///     inductive hypothesis, let B be an allocation in SI
///     dominated by A' which has allocated status in SI.
///     Because B has allocated status in SI, it cannot be
///     one of the additional allocations removed by O.
///
///     If B != A, then B still exists in S and has allocated
///     status, and so it satisfies the condition.
///
///     If B = A, then B must dominate <D since D is a
///     deallocation of A. Since A' dominates B and
///     dominance is transitive, A' dominates <D.
///     Therefore A' dominates every allocation above it on
///     the stack in SI by [nested-allocations-are-dominated].
///     Because A' has pending status in SI, and O did not
///     remove A from S, there must be an allocation AT above
///     A' in the stack in SI which not have pending status
///     and therefore blocked POP from removing A'. AT cannot
///     have undeallocatable status in SI by
///     [correctness-invariant-2] because A' is below it on the
///     the stack and has allocated status in SI. O does not
///     change the status of any allocations it leaves on the
///     stack, so AT must still be in S and have allocated status.
///     Since AT is dominated by A', it satisfies the conditions
///     of the theorem.
///
///   Therefore by induction the theorem holds for all points X.
///
/// Lemma [live-undeallocatable].
///   If X is a point that is not in a dead-end region, then
///   the operation sequence for the path to X in T does not
///   include any MERGE operations, and there are no allocations
///   that have undeallocatable status in the state at X.
/// Pf.
///   A MERGE operation can only correspond to an edge into a
///   dead-end region, so if there is a MERGE operation on
///   OS(P), there is an edge X -> Y on P such that Y is in
///   a dead-end region. But then the suffix of P beginning
///   at <Y, concatenated with a path from X to a function
///   exit (which must exist because X is not in a dead-end
///   region), is a path from a block supposedly in a dead-end
///   region to a function exit, which is a contradiction.
///
///   Allocations can only be given undeallocatable status by
///   by a MERGE operation, so if there are no MERGE
///   operations on OS(P), then the state at X cannot give
///   an allocation undeallocatable status.
///
/// Theorem [live-scc-coherence].
///   If an SCC C is not in a dead-end region, then the end
///   states of all blocks with edges to blocks in C must be
///   the same.
/// Pf.
///   Let C be given, alone with two edges X1 -> Y1 and
///   X2 -> Y2 such that X1 and X2 are not in C and X2 and
///   Y2 are in C. Let P1_I and P2_I be the paths to X1> and
///   X2> in T, respectively.
///
///   Because C is not a dead-end region, there must exist paths
///   P1_F and P2_F from <Y1 and <Y2 to a function exit,
///   respectively. Then
///     P1 := (P1_I..., X1 -> Y1, P1_F...)
///     P2 := (P2_I..., X2 -> Y2, P2_F...)
///   are terminating initial paths.
///
///   Because C is an SCC, there must also exist paths P_Y1Y2
///   and P_Y2Y1 from <Y1 to <Y2 and from <Y2 to <Y1, respectively,
///   which stay entirely within C. Then
///     P1' := (P1_I..., X1 -> Y1, P_Y1Y2..., P2_F...)
///     P2' := (P2_I..., X2 -> Y2, P_Y2Y1..., P1_F...)
///   are also terminating initial paths.
///
///   For any allocation A that is not in C, P_Y1Y2 and P_Y2Y1
///   cannot contain a deallocation of A by
///   [no-scc-self-path-deallocation].
///
///   Now let S1 and S2 be the states at X1> and X2>, respectively.
///   States are sequences of unique allocations with assigned
///   statues. Such sequences may differ in the sets of allocations
///   they include, the statuses they assign to those allocations,
///   or the order of the allocations in the sequence.
///
///   S1 and S2 cannot differ in order. Suppose that A is below B
///   in S1 but B is below A in S2. Then
///     P1_I = (P1_I_EA..., A, P1_I_AB..., B, P1_I_BX1...)
///     P2_I = (P2_I_EB..., B, P2_I_BA..., A, P2_I_AX2...)
///   But then (P1_I_EA..., A, P1_I_AB..., B, P2_I_BA..., A)
///   would be a cyclic path. Since P1_I and P2_I are paths in
///   the same acyclic graph T, this is impossible.
///
///   So if S1 and S2 are not equal, they must disagree about the
///   presence or status of some allocation A. Let A be the
///   topmost such allocation in S1. Note that A is in a proper
///   ancestor SCC of C, so the paths P1_F, P2_F, P_Y1Y2, and P_Y2Y1
///   cannot pass through A again because they can only visit
///   nodes in descendents of C.
///
///   Neither S1 nor S2 can give any allocations undeallocatable
///   status, by [live-undeallocatable].
///
///   Suppose that one state gives A allocated status and the other
///   does not (possibly by not including it at all). Without loss
///   of generality, we assume that S1 makes A allocated:
///
///     By [allocated-implies-allocation], P1_I passes through A
///     and does not pass through a deallocation of A. In order
///     for P1 to satisfy joint post-dominance, P1F must therefore
///     pass through exactly one deallocation of A and not pass
///     through A.
///
///     Keep in mind that, if there is a simple path P2_I' to X2>
///     that passes through a deallocation of A, joint post-dominance
///     is therefore broken for the path
///       (P2_I'..., X2 -> Y2, P_Y2Y1..., P1_F...)
///     because it passes through two deallocations of A without
///     an intervening allocation: A must precede the deallocation
///     of A on P2_I' by dominance; P2_I' cannot pass through A
///     later because it is a simple path; and A cannot be on
///     the later components because A is in a proper ancestor
///     SCC of C, and those paths visit only C and its
///     descendents.
///
///     Now, S2 either does not include A or gives it pending status.
///     If it gives A pending status, then P2_I passes through a
///     deallocation of A by [pending-implies-deallocation], which
///     contradicts the argument above. So S2 must not include A,
///     which means there is a path P2_I' to X2> which either does
///     not pass through A or passes through both A and a
///     deallocation of A. The latter causes a contradiction by
///     the argument above, and the former violates dominance
///     because there is a path to a deallocation of A which does
///     not pass through A. So this kind of disagreement is
///     impossible.
///
///   The only remaining way for S1 and S2 to differ is if one
///   gives A pending status and the other does not include it.
///   Again, without loss of generality, we assume that S1 makes
///   A pending and S2 does not include it:
///
///     By [correctness-invariant-4], because S1 is pending, there
///     must exist an allocation B dominated by A such that
///     S1 includes B and gives it allocated status. Since A was
///     the topmost allocation in S1 for which S1 differs with S2,
///     S2 must also include B and give it allocated status.
///     But this means that both P1_I and P2_I must pass through B.
///     Since P1_I and P2_I are paths in the same spanning tree and
///     pass through a common point B, they must share a common
///     prefix until at least B>, and the state at B> under T is
///     the same for both paths. There is no series of operations
///     which can leave B on the stack but remove an allocation
///     below it on P2, or add B to the middle of the stack on P1.
///     (FIXME: prove) So this kind of disagreement is also
///     impossible.
///
///  Therefore S1 = S2.
///
/// Lemma [block-invariance].
///   Given two spanning trees T1 and T2 and a point X, if the
///   state at the starting point E of the block containing X
///   is the same under T1 and T2, then the state at X is the same
///   under T1 and T2.
/// Pf.
///   Let P1 and P2 be the paths to X in T1 and T2, respectively.
///   P1 and P2 can be different, but the subpath P_EX from
///   E to X must be the same. By induction on P_EX.
///
///   In the base case, P_EX is empty and so X = E. By assumption,
///   the state at E is the same under T1 and T2.
///
///   In the inductive case, P_EX cannot end in an edge because
///   it is a path internal to a single basic block. Therefore let
///   P_EX := (P_EX'..., I). The inductive hypothesis tells us that
///   the state at <I is the same under T1 and T2; let this state
///   be SI. I is an instruction, so under T1 and T2, I either does
///   not correspond to an operation O, or O must be either
///   PUSH@I, POP@I, or PEND@I. This decision depends only on the
///   preceding state; the operation selection rule only depends on
///   the spanning tree more broadly when deciding whether a MERGE
///   is needed for an edge. Therefore the same operation (or lack
///   of one) is selected under T1 and T2, which means the
///   following state is the same.
///
/// Theorem [span-invariance].
///   Given two spanning trees T1 and T2, for all points X, the
///   state at X under T1 is the same as the state at X under T2.
/// Pf.
///   Let T1 and T2 be given. By structural induction on the SCC
///   tree.
///
///   In the base case, X must be a point in the entry block, since
///   it is its own SCC. By definition, the state at the entry point
///   is the empty stack for both T1 and T2. By [block-invariance],
///   the state at X is the same in T1 and T2.
///
///   Now let X be a point in a non-entry block, and assume as the
///   inductive hypothesis that the theorem holds for all points in
///   the proper ancestor SCCs of the SCC C that contains X.
///
///   Let P1 be the path to X in T1 and P2 be the path to X in T2, and
///     P1 = (P1_I..., M1 -> N1, P1_F...)
///     P2 = (P2_I..., M2 -> N2, P2_F...)
///   where M1 and M2 are not in C and N1 and N2 are in C.
///
///   Let ES be the set of edges into C. By definition, the
///   source blocks of these edges are in propr ancestor SCCs of C,
///   and so the states at the end of the source blocks are the
///   same under T1 and T2. The predecessor state set of C is
///   therefore also the same under T1 and T2. This means that,
///   if C is a dead-end region, the decision about whether edges
///   into C correspond to a MERGE operation is the same under T1
///   and T2, and the conservative merger of the predecessor state
///   sets is also the same under T1 and T2.
///
///   If C contains only a single block B:
///     X must be a point in B, and N1 = N2 = B.
///
///     If C is a dead-end region, then because the conservative merger
///     of the predecessor state sets of C is the same under T1 and T2,
///     the state at <B is the same under T1 and T2. Then by
///     [block-invariance], the state at X is also the same under
///     T1 and T2.
///
///     Otherwise, C is a live region. By definition, the state at <B
///     in T1 is the state at M1> in T1, and the state at <B in T2 is
///     the state at M2> in T2. M1 and M2 are in proper ancestor SCCs
///     of C, so by the inductive hypothesis, the state at M2> in T2
///     is the same as the state at M2> in T1. Since C is a live region,
///     by [live-scc-coherence], the state at M2> in T1 is the same as
///     the state at M1> in T1. Therefore the state at <B is the same
///     in both T1 and T2, and by [block-invariance], the state at X is
///     also the same under T1 and T2.
///
///   Otherwise, C is a cyclic SCC:
///     TBD
///
///
/// Theorem.
///   The algorithm preserves the joint post-dominance of
///   allocations and deallocations.
/// 
/// Proof: TBD
StackNesting::Changes StackNesting::fixNesting(SILFunction *F) {
  bool madeChanges = false;

  // The index in the allocation stack for each allocation.
  // This function never uses this directly; it's just a cache for
  // setAllocationAsPending.
  IndexForAllocationMap indexForAllocation;

  // Visit each block of the function in an order consistent with dominance.
  // The state represents the stack of active allocations; it starts
  // with an empty stack because so does the function.
  runInDominanceOrder(*F, State(), [&](SILBasicBlock *B, State &state) {

    // We can't use a foreach loop because we sometimes remove the
    // current instruction or add instructions (that we don't want to
    // visit) after it. Advancing the iterator immediately within the
    // loop is sufficient to protect against both.
    for (auto II = B->begin(), IE = B->end(); II != IE; ) {
      SILInstruction *I = &*II;
      ++II;

      // Invariant: the top of the stack is never pending.
      assert(state.allocations.empty() ||
             !state.allocations.back().isPending());

      // Push allocations onto the current stack in the non-pending state.
      if (I->isAllocatingStack()) {
        state.allocations.push_back(I);
        continue;
      }

      // Ignore instructions other than allocations and deallocations.
      if (!I->isDeallocatingStack()) {
        continue;
      }

      // Get the allocation for the deallocation.
      SILInstruction *dealloc = I;
      SILInstruction *alloc = getAllocForDealloc(dealloc);

#ifndef NDEBUG
      if (state.allocations.empty()) {
        state.abortForUnknownAllocation(alloc, dealloc);
      }
#endif

      // If the allocation is the top of the allocations stack, we can
      // leave it alone.
      if (alloc == state.allocations.back().getValue()) {
        // Pop off our record of the allocation.
        state.allocations.pop_back();

        // Emit any pending deallocations that are on top of the stack.
        emitPendingDeallocations(state, /*after*/ dealloc, madeChanges);

        continue;
      }

      // Otherwise, just remove the deallocation and set the allocation
      // as having a pending deallocation on this path.
      dealloc->eraseFromParent();
      madeChanges = true;
      setAllocationAsPending(state, alloc, dealloc, indexForAllocation);
    }
  });

  // We never make changes to the CFG.
  return (madeChanges ? Changes::Instructions : Changes::None);
}

namespace swift::test {
static FunctionTest MyNewTest("stack_nesting_fixup",
                              [](auto &function, auto &arguments, auto &test) {
                                StackNesting::fixNesting(&function);
                                function.print(llvm::outs());
                              });
} // end namespace swift::test
