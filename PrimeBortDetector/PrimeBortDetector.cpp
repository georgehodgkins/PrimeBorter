#include "llvm/Transforms/PrimeBortDetector/PrimeBortDetector.h"
#include "llvm/Support/Debug.h"
#define DEBUG_TYPE "primebort"
#include <cassert>
#include "LatencyVisitor.h"

// maximum number of instructions to search past a tx start for a corresponding commit
#define INST_SEARCH_LIMIT 8192 

using namespace llvm;

//INITIALIZE_PASS(PrimeBortDetectorPass, "primebort", "Prime+Abort detector", false, false)
static RegisterPass<PrimeBortDetectorPass> reg ("primebort", "Prime+Abort detector");

namespace llvm {

char PrimeBortDetectorPass::ID = 0;

PrimeBortDetectorPass *createPrimeBortDetectorPass() {return new PrimeBortDetectorPass;}

using CI_list = PrimeBortDetectorPass::CI_list;

PrimeBortDetectorPass::PrimeBortDetectorPass() : ModulePass(ID) {}

#define COPY(x) x(src.x)
PrimeBortDetectorPass::PrimeBortDetectorPass(const PrimeBortDetectorPass& src) : 
		ModulePass(ID),
		COPY(txCommitCallers), COPY(txCommitCallees), COPY(txCommitCallerLevels),
		COPY(txBeginCallers), COPY(txBeginCallees), COPY(txBeginCallerLevels),
		COPY(pairedTxBegin), COPY(pairedTxCommit) {}

PreservedAnalyses PrimeBortDetectorPass::run(Module &M, ModuleAnalysisManager &AM) {
	runOnModule(M);
	return PreservedAnalyses::all();
}


bool PrimeBortDetectorPass::runOnModule(Module &M) {
	LLVM_DEBUG(dbgs() << "Start Prime+Abort detector pass";);

	Function* txBegin = M.getFunction("llvm.x86.xbegin");
	Function* txCommit = M.getFunction("llvm.x86.xend");

	if (txBegin) {
		assert(txCommit);

		/*
		 * For each call to txBegin, find an ancestor function
		 * (direct or indirect caller) that is also an ancestor
		 * of txCommit
		 */

		// check graph one level at a time
		// TODO: this code assumes a common ancestor will be at the same level
		// for both, which is likely, but not certain
		CI_list prev_blevel, prev_clevel, new_blevel, new_clevel;
		do {
			// get next graph level
			new_blevel = levelUpCallerGraph(txBegin, prev_blevel,
									txBeginCallees);
			new_clevel = levelUpCallerGraph(txCommit, prev_clevel,
								txCommitCallees);

			// find the intersection of the lists and move those nodes to a new list
			auto candidates = diffCallerGraphs(new_blevel, new_clevel);

			// get call chains to txBegin and txCommit for each common ancestor found
			SmallVector<CallInst*, 4> strand;
			auto C = candidates.second.begin();
			for (auto B = candidates.first.begin(); B != candidates.first.end(); ++B) {
				CallInst* CI = *B;
				do {
					strand.push_back(CI);
					auto f_it = txBeginCallees.find(CI);
					assert(f_it != txBeginCallees.end());
					CI = f_it->second;
				} while (CI);
				pairedTxBegin.push_back(std::move(strand));
				assert(C != candidates.second.end());
				CI = *C;
				do {
					strand.push_back(CI);
					auto f_it = txCommitCallees.find(CI);
					assert(f_it != txCommitCallees.end());
					CI = f_it->second;
				} while (CI);
				pairedTxCommit.push_back(std::move(strand));
				++C;
			}

			// remainder is non-common ancestors
			prev_blevel = new_blevel;
			prev_clevel = new_clevel;
		// end when no non-common ancestors were found on this iteration
		} while (!prev_blevel.empty() && !prev_clevel.empty());

		assert(prev_blevel.empty() && prev_clevel.empty());
		assert(pairedTxBegin.size() == pairedTxCommit.size());

		/*
		 * For each paired tx, estimate the longest path through the tx and
		 * the shortest path back to the beginning from the exit.
		 */

		for (unsigned i = 0; i < pairedTxBegin.size(); ++i) {
			size_t tx = estimateLongestPath(pairedTxBegin[i].back(),
					pairedTxCommit[i].back());
			size_t rtn = estimateShortestPath(pairedTxCommit[i].back(),
					pairedTxBegin[i].back());
			txLat.push_back(tx);
			rtnLat.push_back(rtn);
		}

		LLVM_DEBUG(
			dbgs() << "Found " << pairedTxBegin.size() << " tx:\n\n";
			for (unsigned i = 0; i < pairedTxBegin.size(); ++i) {
				dbgs() << "txBegin call chain:\n";
				for (auto it = pairedTxBegin[i].begin(); it != pairedTxBegin[i].end(); ++it)
					dbgs() << **it << " @ " << (*it)->getFunction() << '\n';
				dbgs() << "\txCommit call chain:\n";
				for (auto it = pairedTxCommit[i].begin(); it != pairedTxCommit[i].end(); ++it)
					dbgs() << **it << " @ " << (*it)->getFunction() << '\n';
				dbgs() << "Estimated longest tx lat: " << txLat[i] << '\n';
				dbgs() << "Estimated shortest rtn lat: " << rtnLat[i] << "\n\n";
			}
		);
	}

	// does not modify code
	return false;
}

bool PrimeBortDetectorPass::compCallInstByFunction(const CallInst* A, const CallInst* B) {
	return A->getFunction() < B->getFunction();
}

std::pair<CI_list, CI_list>
PrimeBortDetectorPass::diffCallerGraphs(CI_list& A, CI_list& B) {
	auto A_it = A.begin();
	auto B_it = B.begin();
	std::pair<CI_list, CI_list> intersect;

	while (A_it != A.end() && B_it != B.end()) {
		if (compCallInstByFunction(*A_it, *B_it)) { // A < B
			++A_it;
		} else if (compCallInstByFunction(*B_it, *A_it)) { // B < A
			++B_it;
		} else { // equal
			auto A_old = A_it;
			auto B_old = B_it;
			++A_it;
			++B_it;
			intersect.first.splice(intersect.first.end(), A, A_old);
			intersect.second.splice(intersect.second.end(), B, B_old);
		}
	}

	assert(intersect.first.size() == intersect.second.size());
	return intersect;
}

CI_list PrimeBortDetectorPass::levelUpCallerGraph(Function* root, CI_list& prev_level,
		DenseMap<CallInst*, CallInst*>& links) {

	CI_list new_level;
	if (prev_level.empty()) {
		assert(links.empty());
		for (auto U = root->user_begin(); U != root->user_end(); ++U) {
			if (isa<CallInst>(*U)) {
				CallInst* CI = cast<CallInst>(*U);
				new_level.push_back(CI);
				auto emplit = links.try_emplace(CI, nullptr);
				assert(emplit.second);
			}
		}
	} else {
		for (auto I = prev_level.begin(); I != prev_level.end(); ++I) {
			Function* F = (*I)->getFunction();
			for (auto U = F->user_begin(); U != F->user_end(); ++U) {
				if (isa<CallInst>(*U)) {
					CallInst* CI = cast<CallInst>(*U);
					auto emplit = links.try_emplace(CI, *I);
					assert(emplit.second);
					new_level.push_back(CI);
				}
			}	
		}
	}
	
	new_level.sort(compCallInstByFunction);
	return new_level;
}

// setting this high is actually a decent heuristic, because
// non-canonical loops are pretty suspicious in a tx
#define FALLBACK_ITER_COUNT 128
size_t PrimeBortDetectorPass::estimateTotalLoopLat (const Loop* L, BasicBlock*& entry,
		const bool longest) {	
	// get exit BBs and loop analysis results
	SmallVector<BasicBlock*, 4> exits;
	L->getExitBlocks(exits);
	ScalarEvolution& SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();

	// find the highest/lowest total latency considering iterations and path length
	if (SE.isBackedgeTakenCountMaxOrZero(L)) { // no good iteration estimate
		size_t sel_lat = (longest) ? 0 : SIZE_MAX;
		BasicBlock* sel_bb = NULL;
		for (auto BB = exits.begin(); BB != exits.end(); ++BB) {
			size_t lat = estimatePathLat(entry, (*BB)->getTerminator(), 0, longest, false);
			if ((longest && lat > sel_lat)
					|| (!longest && lat < sel_lat)) {
				sel_lat = lat;
				sel_bb = *BB;
			}
		}

		// return the selected exit to the caller
		assert(sel_bb);
		entry = sel_bb;

		if (SE.getSmallConstantMaxTripCount(L) != 0)
			return sel_lat*SE.getSmallConstantMaxTripCount(L);
		else 
			return sel_lat*FALLBACK_ITER_COUNT;

	} else { // iteration estimates exist, use them
		size_t ret = (longest) ? 0 : SIZE_MAX;
		BasicBlock* sel_bb = NULL;
		for (auto BB = exits.begin(); BB != exits.end(); ++BB) {
			unsigned iter = SE.getSmallConstantTripCount(L, *BB);
			size_t lat = estimatePathLat(entry, (*BB)->getTerminator(), 0, longest, false);
			size_t tlat = lat * iter;
			if ((longest && tlat > ret)
					|| (!longest && tlat < ret)) {
				ret = tlat;
				sel_bb = *BB;
			}
		}
		// return the selected exit to the caller
		assert(sel_bb);
		entry = sel_bb;
		return ret;
	}
}

size_t PrimeBortDetectorPass::estimateLongestPath(Instruction* start, 
		const Instruction* dest) {
	return estimatePathLat(start->getParent(), dest, 0, true, true);
}

size_t PrimeBortDetectorPass::estimateShortestPath(Instruction* start,
		const Instruction* dest){
	return estimatePathLat(start->getParent(), dest, 0, false, true);
}

// TODO: does not properly explore exit paths from loops in some cases
// not a huge issue since loops with multiple exits are unusual
size_t PrimeBortDetectorPass::estimatePathLat (BasicBlock* BB, const Instruction* dest,
		const size_t prev_lat, const bool longest, const bool handleLoops) {

	if (prev_lat >= MAX_SEARCH_DIST) return prev_lat;

	LatencyVisitor LV;
	LoopInfo& LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
	Loop* L = LI.getLoopFor(BB);

	size_t here_lat = 0;
	if (dest->getParent() == BB) { // we have reached our destination
		for (auto it = BB->begin(); &*it != dest; ++it) {
			LV.visit(*it);
		}
		return LV.getLat();
	} else if (handleLoops && L) { // this BB is in a loop, handle the loop as a chunk
		// this updates BB to be the exit node for the chosen path
		here_lat = estimateTotalLoopLat(L, BB, longest);
	} else {
		LV.visit(BB);
		// add latency for called functions
		while (LV.hasCall()) {
			CallBase* CB = LV.popCall();
			Function* F = CB->getCalledFunction();
			// TODO: ignores indirect calls
			here_lat += estimatePathLat(&F->getEntryBlock(), CB->getNextNonDebugInstruction(),
					here_lat, longest, true);
		}
	}

	// recurse on each successor, and select the longest/shortest path 
	size_t more_lat = (longest) ? 0 : SIZE_MAX;
	if (!LV.sawRet()) { // stop following if function returned
		Instruction* T = BB->getTerminator(); 
		for (unsigned i = 0; i < T->getNumSuccessors(); ++i) {
			size_t succ_lat = estimatePathLat(T->getSuccessor(i), dest, here_lat, longest,
					true);
			if (longest && succ_lat > more_lat) more_lat = succ_lat;
			if (!longest && succ_lat < more_lat) more_lat = succ_lat;
		}
	} else more_lat = 0; // don't add SIZE_MAX when returning from a function

	return here_lat + more_lat;	
}
			
} // namespace llvm

