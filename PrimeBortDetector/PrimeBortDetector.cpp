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
	LLVM_DEBUG(dbgs() << "Start Prime+Abort detector pass\n");

	// get leaf callable objects
	SmallVector<Function*, 4> txBegin; 
	SmallVector<Function*, 4> txCommit;
	populateLeafSets(M, txBegin, txCommit);

	if (!txBegin.empty()) {
		assert(!txCommit.empty());

		/*
		 * For each call to txBegin, find an ancestor function
		 * (direct or indirect caller) that is also an ancestor
		 * of txCommit
		 */

		// check graph one level at a time until all call sites are matched or we hit the
		// top of the graph
		CI_list prev_blevel, prev_clevel, new_blevel, new_clevel, rem_blevel, rem_clevel;
		do {
			// get next graph level
			new_blevel = levelUpCallerGraph(txBegin, prev_blevel,
									txBeginCallees);
			new_clevel = levelUpCallerGraph(txCommit, prev_clevel,
								txCommitCallees);

			// find tx entries and exits in the same function and add them to foundTx
			// return matched CallInst that were removed from the lists
			auto prunes = findCandidates(new_blevel, new_clevel);

			// remove remnants that were matched at this level
			pruneRemnant(prunes.first, rem_blevel, txBeginCallees);
			pruneRemnant(prunes.second, rem_clevel, txCommitCallees);

			// old levels go to remnant sets
			rem_blevel.splice(rem_blevel.end(), prev_blevel);
			rem_clevel.splice(rem_clevel.end(), prev_clevel);

			// un-matched portion of new levels become old levels
			prev_blevel = new_blevel;
			prev_clevel = new_clevel; 

		} while (!(prev_blevel.empty() || prev_clevel.empty()));
		
		// repeat process to match remnants
		if (!prev_blevel.empty()) rem_blevel.splice(rem_blevel.end(), prev_blevel);
		if (!prev_clevel.empty()) rem_clevel.splice(rem_clevel.end(), prev_clevel);
		if (!rem_blevel.empty() || !prev_clevel.empty()) {
			
			auto prunes = findCandidates(rem_blevel, rem_clevel);
			pruneRemnant(prunes.first, rem_blevel, txBeginCallees);
			pruneRemnant(prunes.second, rem_clevel, txCommitCallees);

			while (!rem_blevel.empty()) {
				const CI_list::iterator orig_end = rem_blevel.end();
				for (auto it = rem_blevel.begin(); it != orig_end; ++it) {
					// TODO: this is inefficient here
					CallInst* CI = *it;
					auto f_it = candidateMap.find(CI->getFunction());
					if (f_it != candidateMap.end()) {
						f_it->second.first.push_back(CI);
						continue;
					}
					for (auto U = CI->user_begin(); U != CI->user_end(); ++U) {
						if (isa<CallInst>(*U)) {
							CallInst* T = cast<CallInst>(*U);
							auto f_it = candidateMap.find(T->getFunction());
							if (f_it != candidateMap.end()) {
								f_it->second.first.push_back(T);
							} else {
								rem_blevel.push_back(T);
							}
						}
					}
				}
				rem_blevel.erase(rem_blevel.begin(), orig_end);
			}
			while (!rem_clevel.empty()) {
				const CI_list::iterator orig_end = rem_clevel.end();
				for (auto it = rem_clevel.begin(); it != orig_end; ++it) {
					// TODO: this is inefficient here
					CallInst* CI = *it;
					auto f_it = candidateMap.find(CI->getFunction());
					if (f_it != candidateMap.end()) {
						f_it->second.second.push_back(CI);
						continue;
					}
					for (auto U = CI->use_begin(); U != CI->use_end(); ++U) {
						if (isa<CallInst>(*U)) {
							CallInst* T = cast<CallInst>(*U);
							auto f_it = candidateMap.find(T->getFunction());
							if (f_it != candidateMap.end()) {
								f_it->second.second.push_back(T);
							} else {
								rem_clevel.push_back(T);
							}
						}
					}
				}
				rem_clevel.erase(rem_clevel.begin(), orig_end);
			}
		}

LLVM_DEBUG(
		for (CallInst* CI : rem_blevel) 
			dbgs() << "Unmatched " << *CI << " @ " << *(CI->getFunction()) << '\n';
		for (CallInst* CI : rem_clevel) 
			dbgs() << "Unmatched " << *CI << " @ " << *(CI->getFunction()) << '\n';
);
		
		// match entries to exits
		for (auto it = candidateMap.begin(); it != candidateMap.end(); ++it) {
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
				if (info.exits.empty()) {
					LLVM_DEBUG(dbgs() << "Entry point " << *entry << " in function " << 
						entry->getFunction()->getName() << "has no reachable exits!";);
				} else {
					foundTx.push_back(info);
				}
			}
		}

		// get call chains to entry and exit for each found tx
		for (auto it = foundTx.begin(); it != foundTx.end(); ++it) {
			TxInfo& info = *it;
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
		 * For each tx entry found, estimate the longest path through the tx and
		 * the shortest path back to the beginning for all reachable exits.
		 */

		for (auto it = foundTx.begin(); it != foundTx.end(); ++it) {
			TxInfo& info = *it;
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
			TxInfo& info = *it;
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

#define PUSH_IF_EXISTS(vec, val) \
	do { \
		auto v = val; \
		if (v != NULL) vec.push_back(v); \
	} while (false)

void PrimeBortDetectorPass::populateLeafSets(const Module& M,
		SmallVector<Function*, 4>& begin, SmallVector<Function*, 4>& commit) {

	PUSH_IF_EXISTS(begin, M.getFunction("llvm.x86.xbegin"));
	PUSH_IF_EXISTS(commit, M.getFunction("llvm.x86.xend"));

	PUSH_IF_EXISTS(begin, M.getFunction("pthread_mutex_lock"));
	PUSH_IF_EXISTS(begin, M.getFunction("pthread_rwlock_rdlock"));
	PUSH_IF_EXISTS(begin, M.getFunction("pthread_rwlock_wrlock"));
	PUSH_IF_EXISTS(commit, M.getFunction("pthread_mutex_unlock"));
	PUSH_IF_EXISTS(commit, M.getFunction("pthread_rwlock_unlock"));
}

void PrimeBortDetectorPass::pruneRemnant(CI_list& prune, CI_list& rem,
		const DenseMap<CallInst*, CallInst*>& links) {
	if (rem.empty()) return;
	// find chains for each element of original prune list
	// add chains to prune list
	const size_t osz = prune.size();
	auto it = prune.begin();
	for (size_t i = 0; i < osz; ++i) {
		const auto f_it = links.find(*(it++));
		assert(f_it != links.end());
		CallInst* CI = f_it->second; 
		while (CI) {
			prune.push_back(CI);
			const auto f_it = links.find(CI);
			assert(f_it != links.end());
			CI = f_it->second;
		}
	}

	// sort by called function 
	prune.sort();
	rem.sort();

	auto P_it = prune.begin();
	auto R_it = rem.begin();
	while (P_it != prune.end() && R_it != rem.end()) {
		if (*P_it < *R_it) ++P_it;
		else if (*R_it < *P_it) ++R_it;
		else {
			// remove all instances of this call from both lists
			const CallInst* CI = *P_it;
			do {
				auto old = R_it++;
				rem.erase(old);
			} while (*R_it == CI);
			do {
				auto old = P_it++;
				prune.erase(old);
			} while (*P_it == CI);
		}
	}
}

bool PrimeBortDetectorPass::compCallInstByFunction(const CallInst* A, const CallInst* B) {
	return A->getFunction() < B->getFunction();
}

std::pair<CI_list, CI_list>
PrimeBortDetectorPass::findCandidates(CI_list& A, CI_list& B) {	
	// values to be removed at the end
	SmallVector<CI_list::iterator, 8> rmA;
	SmallVector<CI_list::iterator, 8> rmB;

	// diff requires sort first
	A.sort(compCallInstByFunction);
	B.sort(compCallInstByFunction);

	// find the intersection of the lists 
	auto A_it = A.begin();
	auto B_it = B.begin();
	while (A_it != A.end() && B_it != B.end()) {
		if (compCallInstByFunction(*A_it, *B_it)) {
			++A_it;
		} else if (compCallInstByFunction(*B_it, *A_it)) {
			++B_it;
		} else { // equal
			// there may be multiple in either A or B that match the function,
			// we want all of them
			Function* F = (*A_it)->getFunction();
			candidateMap[F].first.push_back(*A_it);
			rmA.push_back(A_it);
			while ( ++A_it != A.end() && (*A_it)->getFunction() == F) {
				candidateMap[F].first.push_back(*A_it);
				rmA.push_back(A_it);
			}

			candidateMap[F].second.push_back(*B_it);
			rmB.push_back(B_it);
			while (++B_it != B.end() && (*B_it)->getFunction() == F) {
				candidateMap[F].second.push_back(*B_it);
				rmB.push_back(B_it);
			}
		}
	}

	// remove intersection from sets and return it for remnant pruning
	// lists to return removed values
	CI_list A_tomb, B_tomb;
	while (!rmA.empty()) {A_tomb.splice(A_tomb.end(), A, rmA.pop_back_val());}
	while (!rmB.empty()) {B_tomb.splice(B_tomb.end(), B, rmB.pop_back_val());}
	return std::make_pair(A_tomb, B_tomb);
}

CI_list PrimeBortDetectorPass::levelUpCallerGraph(SmallVectorImpl<Function*>& root, CI_list& prev_level,
		DenseMap<CallInst*, CallInst*>& links) {

	CI_list new_level;
	if (links.empty()) { // get next level from root sets
		assert(prev_level.empty());
		for (unsigned i = 0; i < root.size(); ++i) {
			for (auto U = root[i]->user_begin(); U != root[i]->user_end(); ++U) {
				if (isa<CallInst>(*U)) {
					CallInst* CI = cast<CallInst>(*U);
					new_level.push_back(CI);
					auto emplit = links.try_emplace(CI, nullptr);
					assert(emplit.second || emplit.first->second == nullptr);
				}
			}
		}
	} else { // get next level from previous level
		for (auto I = prev_level.begin(); I != prev_level.end(); ++I) {
			Function* F = (*I)->getFunction();
			for (auto U = F->user_begin(); U != F->user_end(); ++U) {
				if (isa<CallInst>(*U)) {
					CallInst* CI = cast<CallInst>(*U);
					auto emplit = links.try_emplace(CI, *I);
					assert(emplit.second || emplit.first->second == *I);
					new_level.push_back(CI);
				}
			}	
		}
	}
	
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
				NULL, lat, newCacheTag(), longest, true, false);
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
				destChain[i], lat, newCacheTag(), longest, true, false);
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

	auto retp = estimatePathLat(start, dest, prev_lat, newCacheTag(), longest, true, true);
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
		BasicBlock*& entry, const unsigned topLevelTag, const bool longest) {	
	// get exit BBs and loop analysis results
	SmallVector<BasicBlock*, 4> exits;
	L->getExitingBlocks(exits);
	ScalarEvolution& SE = 
		getAnalysis<ScalarEvolutionWrapperPass>(*(entry->getParent())).getSE();

	size_t ret = (longest) ? 0 : SIZE_MAX;
	unsigned fallback_iter = SE.getSmallConstantMaxTripCount(L);
	if (fallback_iter == 0) fallback_iter = FALLBACK_ITER_COUNT;
	BasicBlock* sel_bb = NULL;
	for (auto BB = exits.begin(); BB != exits.end(); ++BB) {
		unsigned iter = SE.getSmallConstantTripCount(L, *BB);
		if (iter == 0) iter = fallback_iter;
		auto retp = estimatePathLat(entry->getFirstNonPHIOrDbg(),
				 (*BB)->getTerminator(), 0, topLevelTag, longest, false, true);
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

// TODO: does not properly explore exit paths from loops in some cases
// not a huge issue since loops with multiple exits are unusual
std::pair<size_t, bool>
PrimeBortDetectorPass::estimatePathLat (Instruction* start, const Instruction* dest,
		const size_t prev_lat, const unsigned topLevelTag,
		const bool longest, const bool handleLoops,
		const bool preferHits) {

	if (prev_lat >= MAX_SEARCH_DIST) return std::make_pair(0, false);

	size_t here_lat = 0;
	BasicBlock* BB = start->getParent();
	BasicBlock* entry_BB = BB; // loop coalescing might change BB
	// pseudo-coalesce loops into a single block by finding the longest
	// path through them and jumping to the corresponding exit
	// unless the destination is in the same loop
	if (handleLoops) {
		LoopInfo& LI = getAnalysis<LoopInfoWrapperPass>(*(BB->getParent())).getLoopInfo();
		Loop* L = LI.getLoopFor(BB);
		Loop* dL = (dest) ? LI.getLoopFor(dest->getParent()) : NULL; 
		if (L && L != dL) {
			here_lat = estimateTotalLoopLat(L, BB, topLevelTag, longest);
		}
	}

	// if the tag matches, the cached value is returned; if not,
	// an empty value is placed in the map and filled out when we return
	//
	// this handles reconvergent paths, and stops endless
	// recursion when using estimateTotalLoopLat
	auto f_it = BBLatCache.find(BB);
	if (f_it != BBLatCache.end()) {
		BBLatEntry& nt = f_it->second;
		if (nt.tag == topLevelTag) {
			return nt.prev;
		} else {
			f_it->second = {topLevelTag, std::make_pair(0, false)};
		}
	} else {
		auto emplit = BBLatCache.try_emplace(BB, topLevelTag, std::make_pair(0, false));
		assert(emplit.second);
	}

	// get latency for the current block
	LatencyVisitor LV;
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
	} else {
		LV.visit(BB);
	}
	here_lat += LV.getLat();

	// add latency for functions called in this BB
	while (LV.hasCall()) {
		CallBase* CB = LV.popCall();
		Function* F = CB->getCalledFunction();
		if (F && !(F->empty())) { // ignore intrinsics
			// TODO: ignores indirect calls
			auto retp = estimatePathLat(F->getEntryBlock().getFirstNonPHIOrDbg(),
					NULL, prev_lat + here_lat, topLevelTag, longest, handleLoops, false);
			here_lat += retp.first;
		}
	}

	if (hitDest) {
		auto ret = std::make_pair(here_lat, true);
		auto f_it = BBLatCache.find(entry_BB);
		assert(f_it != BBLatCache.end());
		f_it->second = {topLevelTag, ret};
		return ret;
	}

	// recurse on each successor, and select the longest/shortest path,
	// optionally preferring hits 
	size_t more_lat = (longest) ? 0 : SIZE_MAX;
	if (!isa<ReturnInst>(BB->getTerminator())) { // stop following if block returns
		const Instruction* T = BB->getTerminator(); 
		for (unsigned i = 0; i < T->getNumSuccessors(); ++i) {
			auto retp = estimatePathLat(T->getSuccessor(i)->getFirstNonPHIOrDbg(),
					dest, prev_lat + here_lat, topLevelTag, longest, handleLoops, preferHits);
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

	auto ret = std::make_pair(here_lat + more_lat, hitDest);
	f_it = BBLatCache.find(entry_BB);
	assert(f_it != BBLatCache.end());
	f_it->second = {topLevelTag, ret};
	return ret;	
}
			
} // namespace llvm

