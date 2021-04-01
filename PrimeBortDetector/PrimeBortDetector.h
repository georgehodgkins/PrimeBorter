#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include <list>

#define MAX_SEARCH_DIST 1500000 // 1 ms at 1.5 GHz

namespace llvm {

class PrimeBortDetectorPass : public ModulePass {

	public:
	typedef std::list<CallInst*> CI_list;
	bool runOnModule (Module &M);
	PreservedAnalyses run (Module &M, ModuleAnalysisManager &AM);
	PrimeBortDetectorPass();
	PrimeBortDetectorPass(const PrimeBortDetectorPass&);
	static char ID;
	static StringRef name() {return "primebort";}
	
	void getAnalysisUsage(AnalysisUsage &AU) {
		AU.addRequired<ScalarEvolutionWrapperPass>();
		AU.addRequired<LoopInfoWrapperPass>();
	}

	private:
	DenseMap<BasicBlock*, size_t> BBLatCache;

	CI_list txCommitCallers;
	DenseMap<CallInst*, CallInst*> txCommitCallees;
	SmallVector<CI_list::iterator, 4> txCommitCallerLevels;

	CI_list txBeginCallers;
	DenseMap<CallInst*, CallInst*> txBeginCallees;
	SmallVector<CI_list::iterator, 4> txBeginCallerLevels;

	// TODO: organize in a struct
	SmallVector<SmallVector<CallInst*, 4>, 0> pairedTxBegin;
	SmallVector<SmallVector<CallInst*, 4>, 0> pairedTxCommit;
	
	SmallVector<size_t, 4> txLat;
	SmallVector<size_t, 4> rtnLat;

	// comparator to order CallInsts by their parent function
	static bool compCallInstByFunction(const CallInst*, const CallInst*);
	// returns the intersection of two graphs, removing those elements from the operands
	std::pair<CI_list, CI_list> diffCallerGraphs(CI_list&, CI_list&);
	// computes and returns the next level of a caller graph
	CI_list levelUpCallerGraph(Function*, CI_list&, DenseMap<CallInst*, CallInst*>&);
	// estimates total latency for a loop
	size_t estimateTotalLoopLat(const Loop*, BasicBlock*&, const bool);
	// estimate longest/shortest path between two instructions
	size_t estimateLongestPath(Instruction*, const Instruction*);
	size_t estimateShortestPath(Instruction*, const Instruction*);
	// implementation for the above fns
	size_t estimatePathLat(BasicBlock*, const Instruction*, const size_t, const bool,
			const bool);
};

PrimeBortDetectorPass* createPrimeBortDetectorPass();
}
