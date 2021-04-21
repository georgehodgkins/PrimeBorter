#pragma once
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
	
/*
 * This class provides a latency estimate in cycles for a visited BasicBlock.
 * Its return value is meaningless for any higher IR unit than that,
 * because of control flow. The estimates are ROUGH -- but they probably
 * don't need to be very precise for the detection to work, we're looking for big differences.
 * Each IR instruction
 * has my best choice for the corresponding x86 instruction commented next to it.
 * All latencies are taken from Agner Fog's tables for Ice/Tiger Lake:
 * https://www.agner.org/optimize/instruction_tables.pdf starting at p. 313
 */
namespace llvm {

class LatencyVisitor : public InstVisitor<LatencyVisitor> {
	private:
	SmallVector<CallBase*, 4> calls;
	bool saw_ret;
	size_t lat;

	public:
	LatencyVisitor() : calls(), saw_ret(false), lat(0) {}
	bool sawRet () const {return saw_ret;}
	bool hasCall () const {return !calls.empty();}
	CallBase* popCall () {return calls.pop_back_val();}
	size_t getLat() const {return lat;}

	void visitLoadInst (LoadInst& I) {lat += 3;} // MOV r/m
	void visitStoreInst (StoreInst& I) {lat += 2;} // MOV m/r
	// TODO: memory op lats assume line is in L1 -- not sure how to improve this
	void visitAtomicCmpXchgInst (AtomicCmpXchgInst& I) {lat += 22;} // LOCK CMPXCHG m/r
	void visitAtomicRMWInst(AtomicRMWInst& I) {lat += 21;} // LOCK XADD m/r
	void visitBinaryOperator(BinaryOperator& I) {
		// instructions with dest memory operands have significantly higher latencies
		// not the case with src memory operands, interestingly.
		if (I.mayWriteToMemory()) { 
			switch (I.getOpcode()) {
			case Instruction::Add: // ADD m/r
			case Instruction::Sub: // SUB m/r
			case Instruction::And: // AND m/r
			case Instruction::Or: // OR m/r
			case Instruction::Xor: // XOR m/r
				lat += 7; break;
			case Instruction::Shl: // SHL m/r
			case Instruction::LShr: // SHR m/r
			case Instruction::AShr: // SAR m/r
				lat += 2; break;
			case Instruction::Mul:
				errs() << "LV: int mul with dest memory operand: " << I << "\n"; 
				lat += 4; break;
			case Instruction::UDiv:
			case Instruction::SDiv: 
			case Instruction::URem:
			case Instruction::SRem:
				errs() << "LV: int div with dest memory operand: " << I << "\n"; 
				lat += 15; break;
			case Instruction::FAdd: // FADD m
			case Instruction::FSub: // FSUB m 
				errs() << "LV: FP op with dest memory operand: " << I << "\n"; 
				lat += 3; break;
			case Instruction::FMul:
				errs() << "LV: FP op with dest memory operand: " << I << "\n"; 
				lat += 4; break;
			case Instruction::FDiv:
				errs() << "LV: FP op with dest memory operand: " << I << "\n"; 
				lat += 15; break;
			default:
				errs() << "LatencyVisitor: Unrecognized binary op: " << I << "\n";
				lat += 1;
			}
		} else {
			switch (I.getOpcode()) {
			case Instruction::Add: // ADD r/r
			case Instruction::Sub: // SUB r/r
			case Instruction::And: // AND r/r
			case Instruction::Or: // OR r/r
			case Instruction::Xor: // XOR r/r
			case Instruction::Shl: // SHL r/i
			case Instruction::LShr: // SHR r/i
			case Instruction::AShr: // SAR r/i
				lat += 1; break;
			case Instruction::Mul:
				lat += 4; break;
			case Instruction::UDiv: // DIV r64
			case Instruction::SDiv: // IDIV r64
			case Instruction::URem: // DIV r64
			case Instruction::SRem: // IDIV r64
				lat += 15; break;
			case Instruction::FAdd: // FADD r
			case Instruction::FSub: // FSUB r 
				lat += 3; break;
			case Instruction::FMul: // FMUL r
				lat += 4; break;
			case Instruction::FDiv: // FDIV r
				lat += 15; break;
			default:
				errs() << "LatencyVisitor: Unrecognized binary op: " << I << "\n";
				lat += 1;
			}
		}
	}

	void visitBranchInst (BranchInst& I) {lat += (I.isConditional()) ? 2 : 1;} // JMP(xx) i
	void visitCallBase (CallBase& I) {
		if (!I.isInlineAsm()) {
			lat += 3; // CALL r
			calls.push_back(&I);
		}
	}
	// TODO: visitCatchReturnInst, visitCatchSwitchInst, visitCleanupReturnInst
	// cmpInst is broken out into children
	void visitICmpInst (ICmpInst& I) {lat += 1;} // CMP r/r, should be .25 cycles
	void visitFCmpInst (FCmpInst& I) {lat += 3;} // FCOMP r
	// FP vectors have the same insert/extract latency as integer
	void visitExtractElementInst(ExtractElementInst& I) {lat += 3;} // VEXTRACTI128 x/y/i
	void visitFenceInst (FenceInst& I) {
		switch(I.getOrdering()) {
		case AtomicOrdering::Acquire: lat += 5; break; // LFENCE
		case AtomicOrdering::Release: lat += 6; break; // SFENCE
		case AtomicOrdering::AcquireRelease:
		case AtomicOrdering::SequentiallyConsistent: lat += 36; break; // MFENCE
		default: assert(false && "Not a valid ordering here!");
		}
	}
	// TODO: visitFuncletPadInst
	// ADD r/r + 2x CMP r/r, since MPX BND* insns aren't in the tables. Should be .75 cycles.
	void visitGetElementPtrInst(GetElementPtrInst& I) {lat += 1;} 
	void visitIndirectBrInst(IndirectBrInst& I) {lat += 2;} // JMP r
	void visitInsertElementInst(InsertElementInst& I) {lat += 3;} // VINSERTI128 y/y/x/i
	// TODO: visitInsertValueInst
	void visitLandingPadInst(LandingPadInst& I) {} // ENDBR for a exception, no real op
	void visitPHINode(PHINode& I) {} // no real op
	void visitResumeInst(ResumeInst& I) {} // more exception stuff.
	void visitReturnInst(ReturnInst& I) {lat += 2; saw_ret = true;} // RET or RET i
	void visitSelectInst(SelectInst& I) {lat += 1;} // ternary operator: CMP + CMOV (.5 + .5)
	// TODO: visitShuffleVectorInst
	void visitUnaryOperator(UnaryOperator& I) {
		// The only implemented unary op seems to be FP negation
		assert(I.getOpcode() == Instruction::FNeg);
		lat += 1; // FCHS
	}
	void visitCastInst(CastInst& I) {} // TODO: assumes all casts are reinterps
	void visitUnreachableInst (UnreachableInst& I) {} // probably fine
	void visitAllocaInst(AllocaInst& I) { // stack variable alloc: PUSH m
		static DataLayout DL = DataLayout(I.getModule());
		if (I.isArrayAllocation()) {
			auto V = I.getArraySize();
			if ( ConstantInt* C = dyn_cast<ConstantInt>(V) ) {
				lat += (size_t) C->getLimitedValue();
			} else {
				errs() << "LatencyVisitor: Non-fixed size AllocaInst!";
				lat += 1;
			}
		} else lat += 1;
	}
	
	// fall-through (default block in switch-case, basically)
	void visitInstruction(Instruction& I) {
		errs() << "LatencyVisitor: unrecognized instruction " << I << "\n";
		lat += 1;
	}
};

} // namespace llvm
