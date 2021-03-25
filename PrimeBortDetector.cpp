#include "PrimeBortDetector.h"
#include "llvm/Support/Debug.h"
#define DEBUG_TYPE "primebort"
#include <cassert>

// maximum number of instructions to search past a tx start for a corresponding commit
#define INST_SEARCH_LIMIT 8192 


namespace llvm {

using CI_list = PrimeBortDetectorPass::CI_list;

PreservedAnalyses PrimeBortDetectorPass::run (Module &M, ModuleAnalysisManager &AM) {
	
	// get transaction intrinsics
	// TODO: support asm
	Function* txBegin = M.getFunction("llvm.x86.int_x86_xbegin");
	Function* txCommit = M.getFunction("llvm.x86.int_x86_xend");

	if (txBegin) {
		assert(txCommit);

		/*
		 * For each call to txBegin, find an ancestor function
		 * (direct or indirect caller) that is also an ancestor
		 * of txCommit
		 */

		// check graph one level at a time
		// TODO: this code assumes the ancestor will be at the same level
		// for both, which is likely but not certain
		do {
			// get next graph level
			CI_list new_blevel = levelUpCallerGraph(txBegin, txBeginCallers,
									txBeginCallees, txBeginCallerLevels);
			CI_list new_clevel = levelUpCallerGraph(txCommit, txCommitCallers,
								txCommitCallees, txCommitCallerLevels);

			// find nodes on both lists and remove them from the original list
			CI_list candidates = diffCallerGraphs(txBeginCallers, txCommitCallers);

			// get call chains to txBegin and txCommit for each common ancestor found
			SmallVector<CallInst*, 4> strand;
			for (auto C = candidates.begin(); C != candidates.end(); ++C) {
				CallInst* CI = *C;
				do {
					strand.push_back(CI);
					auto f_it = txBeginCallees.find(CI);
					assert(f_it != txBeginCallees.end());
					CI = f_it->second;
				} while (CI);
				pairedTxBegin.push_back(std::move(strand));
				CI = *C;
				do {
					strand.push_back(CI);
					auto f_it = txCommitCallees.find(CI);
					assert(f_it != txCommitCallees.end());
					CI = f_it->second;
				} while (CI);
				pairedTxCommit.push_back(std::move(strand));
			}

			// put remainder of graph level (non-common ancestors)
			// on the graph for the next round of searching
			txCommitCallers.splice(txCommitCallers.end(), new_clevel);
			txBeginCallers.splice(txBeginCallers.end(), new_blevel);
		// end when no non-common ancestors were added to the graph on this pass
		} while (txCommitCallers.end() != txCommitCallerLevels.back() &&
				 txBeginCallers.end() != txBeginCallerLevels.back());

		assert(txCommitCallers.end() == txCommitCallerLevels.back() &&
				txBeginCallers.end() == txBeginCallerLevels.back());
		assert(pairedTxBegin.size() == pairedTxCommit.size());

		LLVM_DEBUG(
			dbgs() << "Found " << pairedTxBegin.size() << " tx:\n\n";
			for (unsigned i = 0; i < pairedTxBegin.size(); ++i) {
				dbgs() << "txBegin call chain:\n";
				for (auto it = pairedTxBegin[i].begin(); it != pairedTxBegin[i].end(); ++it)
					dbgs() << **it << " @ " << (*it)->getFunction() << '\n';
				dbgs() << "\ntxCommit call chain:\n";
				for (auto it = pairedTxCommit[i].begin(); it != pairedTxCommit[i].end(); ++it)
					dbgs() << **it << " @ " << (*it)->getFunction() << '\n';
				dbgs() << '\n';
			}
		);
	}

	return PreservedAnalyses::all();
}

bool PrimeBortDetectorPass::compCallInstByFunction(const CallInst* A, const CallInst* B) {
	return A->getFunction() < B->getFunction();
}

CI_list PrimeBortDetectorPass::diffCallerGraphs(CI_list& A, CI_list& B) {
	auto A_it = A.begin();
	auto B_it = B.begin();
	CI_list intersection;

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
			intersection.splice(intersection.end(), A, A_old);
			B.erase(B_old);
		}
	}

	return intersection;
}

CI_list PrimeBortDetectorPass::levelUpCallerGraph(Function* root, CI_list& graph,
		DenseMap<CallInst*, CallInst*>& links, SmallVectorImpl<CI_list::iterator>& levels) {

	CI_list new_level;
	if (graph.empty()) {
		assert(links.empty());
		assert(levels.empty());
		levels.push_back(graph.end());
		for (auto U = root->user_begin(); U != root->user_end(); ++U) {
			if (isa<CallInst>(*U)) {
				CallInst* CI = cast<CallInst>(*U);
				new_level.push_back(CI);
				auto emplit = links.try_emplace(CI, nullptr);
				assert(emplit.second);
			}
		}
	} else {
		CI_list::iterator lb = levels.back();
		assert(lb != graph.end());
		levels.push_back(graph.end());
		for (auto I = lb; I != levels.back(); ++I) {
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

} // namespace llvm

