#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <list>

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
	
	private:

	CI_list txCommitCallers;
	DenseMap<CallInst*, CallInst*> txCommitCallees;
	SmallVector<CI_list::iterator, 4> txCommitCallerLevels;

	CI_list txBeginCallers;
	DenseMap<CallInst*, CallInst*> txBeginCallees;
	SmallVector<CI_list::iterator, 4> txBeginCallerLevels;

	SmallVector<SmallVector<CallInst*, 4>, 0> pairedTxBegin;
	SmallVector<SmallVector<CallInst*, 4>, 0> pairedTxCommit;

	// comparator to order CallInsts by their parent function
	static bool compCallInstByFunction(const CallInst*, const CallInst*);
	// returns the intersection of two graphs, removing those elements from the operands
	std::pair<CI_list, CI_list> diffCallerGraphs(CI_list&, CI_list&);
	// computes and returns the next level of a caller graph
	CI_list levelUpCallerGraph(Function*, CI_list&, DenseMap<CallInst*, CallInst*>&);
};

PrimeBortDetectorPass* createPrimeBortDetectorPass();
}
