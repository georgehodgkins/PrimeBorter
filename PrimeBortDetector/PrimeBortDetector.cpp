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
using CandidateMap = PrimeBortDetectorPass::CandidateMap;
using TxInfo = PrimeBortDetectorPass::TxInfo;

PrimeBortDetectorPass::PrimeBortDetectorPass() : ModulePass(ID) {}

#define COPY(x) x(src.x)
PrimeBortDetectorPass::PrimeBortDetectorPass(const PrimeBortDetectorPass& src) : 
		ModulePass(ID),
		COPY(txCommitCallers), COPY(txCommitCallees),
		COPY(txBeginCallers), COPY(txBeginCallees), COPY(foundTx) {}

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
		CI_list prev_blevel, prev_clevel, new_blevel, new_clevel;
		do {
			// get next graph level
			new_blevel = levelUpCallerGraph(txBegin, prev_blevel,
									txBeginCallees);
			new_clevel = levelUpCallerGraph(txCommit, prev_clevel,
								txCommitCallees);

			// find tx entries and exits in the same function
			auto candidates = findCandidates(new_blevel, new_clevel);

			// match entries to exits
			for (auto it = candidates.begin(); it != candidates.end(); ++it) {
				auto F = it->second;
				const SmallVectorImpl<CallInst*>& entries = F.first;
				const SmallVectorImpl<CallInst*>& exits = F.second;
				// each entry is its own transaction, with one or more exits
				for (CallInst* entry : entries) {
					TxInfo info;
					info.entry = entry;
					info.ancestor = it->first;
					// find any of these exits that are reachable and record them in info
					boundTxInFunc(entry->getParent(), exits, info);
					assert(!info.exits.empty() && 
							"PrimeBort: No reachable tx exits found!");
					foundTx[entry] = info;
				}
			}

			// remainder is non-common ancestors
			prev_blevel = new_blevel;
			prev_clevel = new_clevel;
		// end when no non-common ancestors were found on this iteration
		} while (!prev_blevel.empty() && !prev_clevel.empty());

		assert(prev_blevel.empty() && prev_clevel.empty());
	
		// get call chains to entry and exit for each found tx
		for (auto it = foundTx.begin(); it != foundTx.end(); ++it) {
			TxInfo& info = it->second;
			CallInst* CI = info.entry;
			do {
				info.entryChain.push_back(CI);
				CI = txBeginCallees[CI];
			} while (CI);
			for (unsigned i = 0; i < info.exits.size(); ++i) {
				CI = info.exits[i];
				info.exitChains.emplace_back();
				assert(info.exitChains.size() == i+1);
				do {
					info.exitChains[i].push_back(CI);
					CI = txCommitCallees[CI];
				} while (CI);
			}
		}

		/*
		 * For each paired tx, estimate the longest path through the tx and
		 * the shortest path back to the beginning from the exit.
		 */

		for (auto it = foundTx.begin(); it != foundTx.end(); ++it) {
			TxInfo& info = it->second;
			for (unsigned i = 0; i < info.exits.size(); ++i) {
				size_t txLat = estimateLongestPath(info.entryChain, info.exitChains[i]);
				size_t rtLat = estimateShortestPath(info.exitChains[i], info.entryChain);
				info.txLat.push_back(txLat);
				info.rtLat.push_back(rtLat);
				assert(info.txLat.size() == info.rtLat.size() && info.txLat.size() == i+1);
			}
		}

LLVM_DEBUG(
		dbgs() << "FOUND " << foundTx.size() << " TRANSACTIONS:\n=====\n";
		for (auto it = foundTx.begin(); it != foundTx.end(); ++it) {
			TxInfo& info = it->second;
			dbgs() << "Common Func: " << info.ancestor->getName() <<
				"\nEntry point: " << *info.entry << " -->" << *(info.entryChain.back()) <<
				"\nExit\t\t\t\t\t\ttxLat\trtLat\n";

			for (unsigned i = 0; i < info.exits.size(); ++i) { 
				dbgs() << *info.exits[i] << " -->" << *(info.exitChains[i].back()) 
				<< "\t" << info.txLat[i] << '\t' << info.rtLat[i] << '\n';
			}

			dbgs() << "=====\n";
		}
);
	}

	// does not modify code
	return false;
}

bool PrimeBortDetectorPass::compCallInstByFunction(const CallInst* A, const CallInst* B) {
	return A->getFunction() < B->getFunction();
}


CandidateMap PrimeBortDetectorPass::findCandidates(CI_list& A, CI_list& B) {
	auto A_it = A.begin();
	auto B_it = B.begin();
	CandidateMap intersect;
	// values to be removed at the end
	SmallVector<CI_list::iterator, 8> rmA;
	SmallVector<CI_list::iterator, 8> rmB;

	while (A_it != A.end() && B_it != B.end()) {
		if (compCallInstByFunction(*A_it, *B_it)) { // A < B
			++A_it;
		} else if (compCallInstByFunction(*B_it, *A_it)) { // B < A
			++B_it;
		} else { // equal
			// there may be multiple in either A or B that match the function,
			// we want all of them
			Function* F = (*A_it)->getFunction();
			intersect[F].first.push_back(*A_it);
			rmA.push_back(A_it);
			while ( ++A_it != A.end() && (*A_it)->getFunction() == F) {
				intersect[F].first.push_back(*A_it);
				rmA.push_back(A_it);
			}

			intersect[F].second.push_back(*B_it);
			rmB.push_back(B_it);
			while (++B_it != B.end() && (*B_it)->getFunction() == F) {
				intersect[F].second.push_back(*B_it);
				rmB.push_back(B_it);
			}
		}
	}

	// remove intersection from sets
	while (!rmA.empty()) {A.erase(rmA.pop_back_val());}
	while (!rmB.empty()) {B.erase(rmB.pop_back_val());}

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

void PrimeBortDetectorPass::boundTxInFunc(BasicBlock* current,
			const SmallVectorImpl<CallInst*>& exits, TxInfo& info) {
	// clear marked BBs for each new entry point
	static TxInfo* infoaddr_last = &info;
	static DenseMap<BasicBlock*, bool> visited;
	if (&info != infoaddr_last) {
		visited.clear();
		infoaddr_last = &info;
	}
	// mark BBs to avoid multiple visits
	if (visited[current] == true) return;
	else visited[current] = true;
	
	// if we have reached an exit add it to exit list and return
	for (CallInst* exit : exits) {
		assert(exit->getFunction() == current->getParent());
		if (current == exit->getParent()) {
			info.exits.push_back(exit);
			return;
		}
	}
	const Instruction* T = current->getTerminator();
	// if we hit a return without an exit just return
	if (isa<ReturnInst>(T)) {
LLVM_DEBUG(
		dbgs() << "PrimeBort: Hit return without tx commit in common caller: "
		<< current->getParent() << " @ " << current << "\n";
);
		return;
	}
	// otherwise, recurse on successors
	for (unsigned i = 0; i < T->getNumSuccessors(); ++i) 
		boundTxInFunc(T->getSuccessor(i), exits, info);

	return;
}

size_t PrimeBortDetectorPass::estimatePathFromChains(
		const SmallVectorImpl<CallInst*>& startChain,
		const SmallVectorImpl<CallInst*>& destChain,
		const bool longest) {
	size_t lat = 0;
	assert(startChain.front()->getFunction() == destChain.front()->getFunction());
	
	// get latency in each function in start chain
	for (unsigned i = startChain.size()-1; i > 0; --i) {
		auto retp = estimatePathLat(startChain[i]->getParent()->getFirstNonPHIOrDbg(),
				NULL, lat, true, true, false);
		assert(!retp.second);
		lat += retp.first;
	}

	// get latency between calls in common ancestor,
	// moving up in the call graph if necessary
	lat += estimateLatThroughCallers(startChain.front(), destChain.front(),
			lat, longest);

	// get latency in each function in dest chain
	std::pair<size_t, bool> retp;
	for (unsigned i = 1; i < destChain.size(); ++i) {
		retp = estimatePathLat(
				destChain[i-1]->getCalledFunction()->getEntryBlock().getFirstNonPHIOrDbg(),
				destChain[i], lat, true, true, false);
		lat += retp.first;
	}
	assert(retp.second);

	return lat;
}

size_t PrimeBortDetectorPass::estimateShortestPath(
		const SmallVectorImpl<CallInst*>& startChain,
		const SmallVectorImpl<CallInst*>& destChain) {
	return estimatePathFromChains(startChain, destChain, false);	
}

size_t PrimeBortDetectorPass::estimateLongestPath(
		const SmallVectorImpl<CallInst*>& startChain,
		const SmallVectorImpl<CallInst*>& destChain) {
	return estimatePathFromChains(startChain, destChain, true);	
}

size_t PrimeBortDetectorPass::estimateLatThroughCallers (
		Instruction* start, const CallInst* dest,
		const size_t prev_lat, const bool longest) {
	
	if (prev_lat >= MAX_SEARCH_DIST) return prev_lat;

	assert(start->getFunction() == dest->getFunction());
	Function* F = start->getFunction();

	auto retp = estimatePathLat(start, dest, prev_lat, longest, true, true);
	// return if dest is reachable at this level
	if (retp.second) return retp.first;

	// otherwise, recurse upwards in the call graph
	size_t here_lat = retp.first;
	size_t more_lat = (longest) ? 0 : SIZE_MAX;
	for (auto U = F->use_begin(); U != F->use_end(); ++U) {
		if (isa<CallInst>(*U)) {
			CallInst* CI = cast<CallInst>(*U);
			assert(CI->getNextNonDebugInstruction() != NULL);
			size_t c_lat = estimateLatThroughCallers(CI->getNextNonDebugInstruction(),
					CI, prev_lat + here_lat, longest);
			if ((longest && c_lat > more_lat) ||
					(!longest && c_lat < more_lat)) {
				more_lat = c_lat;
			}
		}
	}

	return here_lat + more_lat;
}

// setting this high is actually a decent heuristic, because
// non-canonical loops are pretty suspicious in a tx
#define FALLBACK_ITER_COUNT 128
size_t PrimeBortDetectorPass::estimateTotalLoopLat (const Loop* L,
		BasicBlock*& entry, const bool longest) {	
	// get exit BBs and loop analysis results
	SmallVector<BasicBlock*, 4> exits;
	L->getExitBlocks(exits);
	ScalarEvolution& SE = 
		getAnalysis<ScalarEvolutionWrapperPass>(*(entry->getParent())).getSE();

	// find the highest/lowest total latency considering iterations and path length
	if (SE.isBackedgeTakenCountMaxOrZero(L)) { // no good iteration estimate
		size_t sel_lat = (longest) ? 0 : SIZE_MAX;
		BasicBlock* sel_bb = NULL;
		for (auto BB = exits.begin(); BB != exits.end(); ++BB) {
			auto retp = estimatePathLat(entry->getFirstNonPHIOrDbg(),
					(*BB)->getTerminator(), 0, longest, false, true);
			if ((longest && retp.first > sel_lat)
					|| (!longest && retp.first < sel_lat)) {
				sel_lat = retp.first;
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
			auto retp = estimatePathLat(entry->getFirstNonPHIOrDbg(),
					 (*BB)->getTerminator(), 0, longest, false, true);
			size_t tlat = retp.first * iter;
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

// TODO: does not properly explore exit paths from loops in some cases
// not a huge issue since loops with multiple exits are unusual
std::pair<size_t, bool>
PrimeBortDetectorPass::estimatePathLat (Instruction* start, const Instruction* dest,
		const size_t prev_lat, const bool longest, const bool handleLoops,
		const bool preferHits) {

	if (prev_lat >= MAX_SEARCH_DIST) return std::make_pair(prev_lat, false);

	BasicBlock* BB = start->getParent();
	LatencyVisitor LV;
	LoopInfo& LI = getAnalysis<LoopInfoWrapperPass>(*(BB->getParent())).getLoopInfo();
	Loop* L = LI.getLoopFor(BB);

	size_t here_lat = 0;
	bool hitDest = false;
	if (dest && dest->getParent() == BB) { // we have reached our destination, probably
		for (Instruction* I = start; I && I != BB->getTerminator();
				I = I->getNextNonDebugInstruction()) {
			LV.visit(I);
			if (I == dest) {
				hitDest = true;
				break;
			}
		}
	} else if (handleLoops && L) { // this BB is a loop entry, handle the loop as a chunk
		// this updates BB to be the exit node for the chosen path
		here_lat = estimateTotalLoopLat(L, BB, longest);
	} else {
		LV.visit(BB);
	}

	here_lat = LV.getLat();

	// add latency for functions called in this BB
	while (LV.hasCall()) {
		CallBase* CB = LV.popCall();
		Function* F = CB->getCalledFunction();
		if (F && !(F->empty())) { // ignore intrinsics
			// TODO: ignores indirect calls
			auto retp = estimatePathLat(F->getEntryBlock().getFirstNonPHIOrDbg(),
					NULL, prev_lat + here_lat, longest, true, false);
			here_lat += retp.first;
		}
	}

	if (hitDest) return std::make_pair(here_lat, true);

	// recurse on each successor, and select the longest/shortest path,
	// optionally preferring hits 
	size_t more_lat = (longest) ? 0 : SIZE_MAX;
	if (!isa<ReturnInst>(BB->getTerminator())) { // stop following if block returns
		const Instruction* T = BB->getTerminator(); 
		for (unsigned i = 0; i < T->getNumSuccessors(); ++i) {
			auto retp = estimatePathLat(T->getSuccessor(i)->getFirstNonPHIOrDbg(),
					dest, prev_lat + here_lat, longest, true, preferHits);
			// split up the conditional for readability
			// this is the base condition: whether this is the longest/shortest path seen
			bool selPath =
				(longest && retp.first > more_lat) ||
				(!longest && retp.second < more_lat);
			// if preferHits is true, only use miss values
			// if we have not seen a hit yet
			if ((retp.second || !hitDest || !preferHits) &&
					// always choose value from first hit if hits are preferred
					((retp.second != hitDest) || selPath)) { // finding shortest
				more_lat = retp.first;
				hitDest = retp.second;
			}
		}
	} else more_lat = 0; // don't add SIZE_MAX when returning from a function

	return std::make_pair(here_lat + more_lat, hitDest);	
}
			
} // namespace llvm

