#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include <list>
#include <unordered_map>
#include <utility>

#define MAX_SEARCH_DIST 1500000 // 1 ms at 1.5 GHz

namespace llvm {

class PrimeBortDetectorPass : public ModulePass {

	public:
	typedef std::list<CallInst*> CI_list;
	typedef DenseMap<Function*,
			std::pair<SmallVector<CallInst*, 4>, SmallVector<CallInst*, 4> > > CandidateMap;
	bool runOnModule (Module &M);
	PreservedAnalyses run (Module &M, ModuleAnalysisManager &AM);
	PrimeBortDetectorPass();
	PrimeBortDetectorPass(const PrimeBortDetectorPass&);
	static char ID;
	static StringRef name() {return "primebort";}
	
	void getAnalysisUsage(AnalysisUsage &AU) const override {
		AU.addRequired<ScalarEvolutionWrapperPass>();
		AU.addRequired<LoopInfoWrapperPass>();
		AU.setPreservesAll();
	}

	struct TxInfo {
		CallInst* entry;
		Function* ancestor;
		SmallVector<CallInst*, 4> exits;
		SmallVector<CallInst*, 4> entryChain;
		SmallVector<SmallVector<CallInst*, 4>, 4> exitChains;
		SmallVector<size_t, 4> txLat;
		SmallVector<size_t, 4> rtLat;
	};

	private:
	struct BBLatEntry {
		unsigned tag;
		std::pair<size_t, bool> prev;

		BBLatEntry(const unsigned t, const std::pair<size_t, bool> p) 
			: tag(t), prev(p) {}
	};
	DenseMap<BasicBlock*, BBLatEntry> BBLatCache;
	unsigned tagCounter;
	unsigned newCacheTag () {
		unsigned r = ++tagCounter;
		if (r == 0) BBLatCache.clear();
		return r;
	}

	CI_list txCommitCallers;
	DenseMap<CallInst*, CallInst*> txCommitCallees;

	CI_list txBeginCallers;
	DenseMap<CallInst*, CallInst*> txBeginCallees;

	DenseMap<CallInst*, TxInfo> foundTx;

	// comparator to order CallInsts by their parent function
	static bool compCallInstByFunction(const CallInst*, const CallInst*);
	// returns the intersection of two graphs, removing those elements from the operands
	CandidateMap findCandidates (CI_list&, CI_list&);
	// computes and returns the next level of a caller graph
	CI_list levelUpCallerGraph(Function*, CI_list&, DenseMap<CallInst*, CallInst*>&);
	// match tx entry points with reachable exit points in the same function
	void boundTxInFunc(BasicBlock*, const SmallVectorImpl<CallInst*>&, TxInfo&);
	// estimates total latency for a loop
	size_t estimateTotalLoopLat(const Loop*, BasicBlock*&, const unsigned, const bool);
	// estimator that can climb up the call graph
	size_t estimateLatThroughCallers(Instruction*, const CallInst*,
			const size_t, const bool);
	// estimate longest/shortest path between two instructions given call chains
	// up to their common ancestor
	size_t estimatePathFromChains(const SmallVectorImpl<CallInst*>&,
			const SmallVectorImpl<CallInst*>&, const bool);
	// wrappers for estimatePathFromChains
	size_t estimateLongestPath(const SmallVectorImpl<CallInst*>&,
			const SmallVectorImpl<CallInst*>&);
	size_t estimateShortestPath(const SmallVectorImpl<CallInst*>&,
			const SmallVectorImpl<CallInst*>&);
	// implementation for the above fns
	std::pair<size_t, bool> estimatePathLat(Instruction*, const Instruction*,
			const size_t, const unsigned, const bool, const bool, const bool);
};

PrimeBortDetectorPass* createPrimeBortDetectorPass();
}
