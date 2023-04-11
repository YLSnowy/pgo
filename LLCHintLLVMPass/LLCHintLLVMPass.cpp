#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/ProfileData/SampleProfReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO/SampleProfile.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;
using namespace sampleprof;

bool AutoFDOMapping;
int block_count = 0;

static cl::opt<std::string> PrefetchFile("input-file", cl::desc("Specify input filename for mypass"), cl::value_desc("filename"));

cl::list<std::string> LBR_dist("dist", cl::desc("Specify offset value from LBR"), cl::OneOrMore);

SmallVector<Instruction*, 10> IndirectLoads;
SmallVector<Instruction*, 20> IndirectInstrs;
SmallVector<Instruction*, 10> IndirectPhis;
Instruction* IndirectLoad;
int64_t IndirectPrefetchDist;

namespace {
// struct LLCHintLLVMPass : public ModulePass {
struct LLCHintLLVMPass : public FunctionPass {
	bool doInitialization(Module& M) override;
	bool runOnFunction(Function& F) override;
	void getAnalysisUsage(AnalysisUsage& AU) const override
	{
		AU.addRequired<ScalarEvolutionWrapperPass>();
		AU.addPreserved<ScalarEvolutionWrapperPass>();
		AU.addRequired<LoopInfoWrapperPass>();
		AU.addRequiredTransitive<CallGraphWrapperPass>();
		// AU.addPreserved<CallGraphWrapperPass>();
		// AU.addRequired<DominatorTree>();
	}
	bool SearchAlgorithm(Instruction* I, LoopInfo& LI, Instruction*& Phi, SmallVector<Instruction*, 10>& Loads, SmallVector<Instruction*, 20>& Instrs, SmallVector<Instruction*, 10>& Phis);
	bool InjectPrefeches(Instruction* curLoad, LoopInfo& LI, SmallVector<llvm::Instruction*, 10>& CapturedPhis, SmallVector<llvm::Instruction*, 10>& CapturedLoads, SmallVector<Instruction*, 20>& CapturedInstrs, int64_t prefetchDist, bool ItIsIndirectLoad);
	bool InjectPrefechesOnePhiPartOne(Instruction* curLoad, LoopInfo& LI, SmallVector<llvm::Instruction*, 10>& CapturedPhis, SmallVector<llvm::Instruction*, 10>& CapturedLoads, SmallVector<Instruction*, 20>& CapturedInstrs, int64_t prefetchDist, bool ItIsIndirectLoad);
	bool InjectPrefechesOnePhiPartTwo(Instruction* I, LoopInfo& LI, Instruction* Phi, SmallVector<Instruction*, 20>& DepInstrs, int64_t prefetchDist);
	CmpInst* getCompareInstrADD(Loop* L, Instruction* nextInd);
	CmpInst* getCompareInstrGetElememntPtr(Loop* L, Instruction* nextInd);
	PHINode* getCanonicalishInductionVariable(Loop* L);
	bool CheckLoopCond(Loop* L);
	Instruction* GetIncomingValue(Loop* L, llvm::Instruction* curPN);
	ConstantInt* getValueAddedToIndVar(Loop* L, Instruction* nextInd);
	ConstantInt* getValueAddedToIndVarInLoopIterxxx(Loop* L);
	Value* getLoopEndCondxxx(Loop* L);
	bool IsDep(Instruction* I, LoopInfo& LI, Instruction*& Phi, SmallVector<Instruction*, 10>& DependentLoads, SmallVector<Instruction*, 20>& DependentInstrs, SmallVector<Instruction*, 10>& DPhis);

public:
	static char ID;
//	LoopInfo *LI;
	LLCHintLLVMPass()
		: FunctionPass(ID)
	{
	}
	Module* M = 0;
private:
	std::unique_ptr<llvm::sampleprof::SampleProfileReader> Reader;

}; // struct

using Hints = SampleRecord::CallTargetMap;
ErrorOr<Hints> getHints(const llvm::Instruction& Inst, const llvm::sampleprof::FunctionSamples* TopSamples)
{
	if (const auto& Loc = Inst.getDebugLoc()) {
		if (const auto* Samples = TopSamples->findFunctionSamples(Loc)) {
			return Samples->findCallTargetMapAt(FunctionSamples::getOffset(Loc), Loc->getBaseDiscriminator());
		}
	}
	return std::error_code();
}
} // namespace

char LLCHintLLVMPass::ID = 0;

bool LLCHintLLVMPass::doInitialization(Module& M)
{
	if (PrefetchFile.empty()) {
		errs() << "PrefetchFile is Empty!\n";
		return false;
	}

	LLVMContext& Ctx = M.getContext();
	ErrorOr<std::unique_ptr<SampleProfileReader>> ReaderOrErr = SampleProfileReader::create(PrefetchFile, Ctx);
	if (std::error_code EC = ReaderOrErr.getError()) {
		std::string Msg = "Could not open profile: " + EC.message();
		Ctx.diagnose(DiagnosticInfoSampleProfile(PrefetchFile, Msg, DiagnosticSeverity::DS_Warning));
		return false;
	}

	Reader = std::move(ReaderOrErr.get());
	Reader->read();

	for (auto& F : M) {
		const llvm::sampleprof::FunctionSamples* SamplesReaded = Reader->getSamplesFor(F);
		if (SamplesReaded) {
			AutoFDOMapping = true;
		}
	}

	return true;
}


bool LLCHintLLVMPass::SearchAlgorithm(Instruction* I, LoopInfo& LI, Instruction*& Phi, SmallVector<Instruction*, 10>& Loads, SmallVector<Instruction*, 20>& Instrs, SmallVector<Instruction*, 10>& Phis)
{
	bool PhiFound = false;
	Use* OperandList = I->getOperandList();
	Use* NumOfOperands = OperandList + I->getNumOperands();
	Loop* curInstrLoop = LI.getLoopFor(I->getParent());
	SmallVector<Instruction*, 10> NeedToSearch;

	for (Use* op = OperandList; op < NumOfOperands; op++) {
		if (PHINode* CurOpIsPhiNode = dyn_cast<PHINode>(op->get())) {
			Phi = CurOpIsPhiNode;
			if (!(std::find(Phis.begin(), Phis.end(), CurOpIsPhiNode) != Phis.end())) {
				Phis.push_back(CurOpIsPhiNode);
			}
			PhiFound = true;
		} else if (LoadInst* curOperandIsLoad = dyn_cast<LoadInst>(op->get())) {
			if (!(std::find(Loads.begin(), Loads.end(), curOperandIsLoad) != Loads.end())) {
				Loads.push_back(curOperandIsLoad);
			}
			NeedToSearch.push_back(curOperandIsLoad);

		} else if (Instruction* OtherTypeInstr = dyn_cast<Instruction>(op->get())) {
			Loop* OtherTypeInstrLoop = LI.getLoopFor(OtherTypeInstr->getParent());
			if (OtherTypeInstrLoop == curInstrLoop) {
				NeedToSearch.push_back(OtherTypeInstr);
			}
		}
	}
	for (size_t index = 0; index < NeedToSearch.size(); index++) {
		Instrs.push_back(NeedToSearch[index]);
		bool temp = SearchAlgorithm(NeedToSearch[index], LI, Phi, Loads, Instrs, Phis);
		PhiFound = true;
	}
	return PhiFound;
}

bool LLCHintLLVMPass::IsDep(Instruction* I, LoopInfo& LI, Instruction*& Phi, SmallVector<Instruction*, 10>& DependentLoadsToCurLoad, SmallVector<Instruction*, 20>& DependentInstrsToCurLoad, SmallVector<Instruction*, 10>& Phis)
{
	bool PhiFound = false;
	Use* OperandList = I->getOperandList();
	Use* NumOfOperands = OperandList + I->getNumOperands();
	Loop* curInstrLoop = LI.getLoopFor(I->getParent());

	for (Use* op = OperandList; op < NumOfOperands; op++) {
		if (PHINode* CurOpIsPhiNode = dyn_cast<PHINode>(op->get())) {
			Loop* PhiNodeLoop = LI.getLoopFor(CurOpIsPhiNode->getParent());
			if (PhiNodeLoop == curInstrLoop) {
				Phi = CurOpIsPhiNode;
				DependentInstrsToCurLoad.push_back(CurOpIsPhiNode);
				Phis.push_back(CurOpIsPhiNode);
				PhiFound = true;
			}
		} else if (LoadInst* curOperandIsLoad = dyn_cast<LoadInst>(op->get())) {
			Loop* LoadInstrLoop = LI.getLoopFor(curOperandIsLoad->getParent());
			if (LoadInstrLoop == curInstrLoop) {
				DependentLoadsToCurLoad.push_back(curOperandIsLoad);
				DependentInstrsToCurLoad.push_back(curOperandIsLoad);
				if (IsDep(curOperandIsLoad, LI, Phi, DependentLoadsToCurLoad, DependentInstrsToCurLoad, Phis)) {
					PhiFound = true;
				}
			}
		} else if (Instruction* OtherTypeInstr = dyn_cast<Instruction>(op->get())) {
			Loop* OtherTypeInstrLoop = LI.getLoopFor(OtherTypeInstr->getParent());
			if (OtherTypeInstrLoop == curInstrLoop) {
				DependentInstrsToCurLoad.push_back(OtherTypeInstr);
				if (IsDep(OtherTypeInstr, LI, Phi, DependentLoadsToCurLoad, DependentInstrsToCurLoad, Phis)) {
					PhiFound = true;
				}
			}
		}
	}
	return PhiFound;
}

ConstantInt* LLCHintLLVMPass::getValueAddedToIndVarInLoopIterxxx(Loop* L)
{
	SetVector<Instruction*> BBInsts;
	auto B = L->getExitingBlock();
	int count = 0;
	if (!B)
		return nullptr;
	for (Instruction& J : *B) {
		Instruction* I = &J;
		BBInsts.insert(I);
		count++;
	}
	bool Changed = false;
	for (int i = BBInsts.size() - 1; i >= 0; i--) {
		CmpInst* CI = dyn_cast<CmpInst>(BBInsts[i]);
		if (CI) {
			Instruction* AddI = dyn_cast<Instruction>(BBInsts[i - 1]);
			if (AddI->getOpcode() == Instruction::Add) {
				if (L->makeLoopInvariant(AddI->getOperand(1), Changed)) {
					if (ConstantInt* constInt = dyn_cast<ConstantInt>(AddI->getOperand(1))) {
						return constInt;
					}
				}
				if (L->makeLoopInvariant(AddI->getOperand(0), Changed)) {
					if (ConstantInt* constInt = dyn_cast<ConstantInt>(AddI->getOperand(1))) {
						return constInt;
					}
				}
			}
		}
	}
	return nullptr;
}

PHINode* LLCHintLLVMPass::getCanonicalishInductionVariable(Loop* L)
{
	BasicBlock* H = L->getHeader();
	BasicBlock *Incoming = nullptr, *Backedge = nullptr;
	pred_iterator PI = pred_begin(H);
	assert(PI != pred_end(H) && "Loop must have at least one backedge!");
	Backedge = *PI++;
	if (PI == pred_end(H)) {
		return nullptr; // dead loop
	}
	Incoming = *PI++;
	if (PI != pred_end(H)) {
		return nullptr; // multiple backedges?
	}
	if (L->contains(Incoming)) {
		if (L->contains(Backedge))
			return nullptr;
		std::swap(Incoming, Backedge);
	} else if (!L->contains(Backedge)) {
		return nullptr;
	}
	for (BasicBlock::iterator I = H->begin(); isa<PHINode>(I); ++I) {
		PHINode* PN = cast<PHINode>(I);
		if (Instruction* Inc = dyn_cast<Instruction>(PN->getIncomingValueForBlock(Backedge))) {
			if (Inc->getOpcode() == Instruction::Add && Inc->getOperand(0) == PN) {
				if (dyn_cast<ConstantInt>(Inc->getOperand(1))) {
					return PN;
				}
			}
		}
	}
	return nullptr;
}

Value* LLCHintLLVMPass::getLoopEndCondxxx(Loop* L)
{
	SetVector<Instruction*> BBInsts;
	auto B = L->getExitingBlock();
	int count = 0;
	if (!B)
		return nullptr;
	for (Instruction& J : *B) {
		Instruction* I = &J;
		BBInsts.insert(I);
		count++;
	}
	bool Changed = false;
	for (int i = BBInsts.size() - 1; i >= 0; i--) {
		CmpInst* CI = dyn_cast<CmpInst>(BBInsts[i]);
		if (CI) {
			if (L->makeLoopInvariant(CI->getOperand(1), Changed)) {
				return CI->getOperand(1);
			}
			if (L->makeLoopInvariant(CI->getOperand(0), Changed)) {
				return CI->getOperand(0);
			}
		}
	}
	return nullptr;
}

CmpInst* LLCHintLLVMPass::getCompareInstrADD(Loop* L, Instruction* nextInd)
{
	SetVector<Instruction*> BBInsts;
	auto B = L->getExitingBlock();
	int count = 0;

	if (!B)
		return nullptr;
	for (Instruction& J : *B) {
		Instruction* I = &J;
		BBInsts.insert(I);
		count++;
	}
	for (int i = BBInsts.size() - 1; i >= 0; i--) {
		CmpInst* CI = dyn_cast<CmpInst>(BBInsts[i]);
		if (CI && (CI->getOperand(0) == nextInd || CI->getOperand(1) == nextInd) && nextInd->getOpcode() == Instruction::Add) {
			return CI;
		}
	}

	return nullptr;
}

CmpInst* LLCHintLLVMPass::getCompareInstrGetElememntPtr(Loop* L, Instruction* nextInd)
{
	SetVector<Instruction*> BBInsts;
	auto B = L->getExitingBlock();
	int count = 0;

	if (!B)
		return nullptr;
	for (Instruction& J : *B) {
		Instruction* I = &J;
		BBInsts.insert(I);
		count++;
	}
	for (int i = BBInsts.size() - 1; i >= 0; i--) {
		CmpInst* CI = dyn_cast<CmpInst>(BBInsts[i]);
		if (CI && (CI->getOperand(0) == nextInd || CI->getOperand(1) == nextInd) && nextInd->getOpcode() == Instruction::GetElementPtr) {
			return CI;
		}
	}

	return nullptr;
}

bool LLCHintLLVMPass::CheckLoopCond(Loop* L)
{
	bool OKtoPrefetch = false;
	BasicBlock* H = L->getHeader();
	BasicBlock *Incoming = nullptr, *Backedge = nullptr;
	pred_iterator PI = pred_begin(H);
	assert(PI != pred_end(H) && "Loop must have at least one backedge!");
	Backedge = *PI++;
	if (PI == pred_end(H)) {
		return OKtoPrefetch; // dead loop
	}
	Incoming = *PI++;
	if (PI != pred_end(H)) {
		return OKtoPrefetch; // multiple backedges?
	}

	if (L->contains(Incoming)) {
		if (L->contains(Backedge)) {
			return OKtoPrefetch;
		}
		std::swap(Incoming, Backedge);
	} else if (!L->contains(Backedge)) {
		return OKtoPrefetch;
	}
	OKtoPrefetch = true;
	return OKtoPrefetch;
}

Instruction* LLCHintLLVMPass::GetIncomingValue(Loop* L, llvm::Instruction* curPN)
{
	BasicBlock* H = L->getHeader();
	BasicBlock* Backedge = nullptr;
	pred_iterator PI = pred_begin(H);
	Backedge = *PI++;

	for (BasicBlock::iterator I = H->begin(); isa<PHINode>(I); ++I) {
		PHINode* PN = cast<PHINode>(I);
		if (PN == curPN) {
			if (Instruction* IncomingInstr = dyn_cast<Instruction>(PN->getIncomingValueForBlock(Backedge))) {
				return IncomingInstr;
			}
		}
	}
	return nullptr;
}

ConstantInt* LLCHintLLVMPass::getValueAddedToIndVar(Loop* L, Instruction* nextInd)
{
	bool Changed = false;
	if (L->makeLoopInvariant(nextInd->getOperand(1), Changed)) {
		if (ConstantInt* constInt = dyn_cast<ConstantInt>(nextInd->getOperand(1))) {
			return constInt;
		}
	}
	if (L->makeLoopInvariant(nextInd->getOperand(0), Changed)) {
		if (ConstantInt* constInt = dyn_cast<ConstantInt>(nextInd->getOperand(1))) {
			return constInt;
		}
	}
	return nullptr;
}

bool LLCHintLLVMPass::InjectPrefeches(Instruction* curLoad, LoopInfo& LI, SmallVector<llvm::Instruction*, 10>& CapturedPhis, SmallVector<llvm::Instruction*, 10>& CapturedLoads, SmallVector<Instruction*, 20>& CapturedInstrs, int64_t prefetchDist, bool ItIsIndirectLoad)
{

	Loop* IndirectLoadLoop;
	if (ItIsIndirectLoad) {
		IndirectLoad = curLoad;
		IndirectLoads = CapturedLoads;
		IndirectInstrs = CapturedInstrs;
		IndirectPhis = CapturedPhis;
		IndirectPrefetchDist = prefetchDist;
		IndirectLoadLoop = LI.getLoopFor(IndirectLoad->getParent());
	}

	bool done = false;
	bool PrefetchGetElem = false;
	Instruction* phi = nullptr;
	Loop* curLoadLoop = LI.getLoopFor(curLoad->getParent());
	bool donePrefetchingForPhi = false;

	if (CapturedPhis.size() == 1) {
		phi = CapturedPhis[0];
		ValueMap<Instruction*, Value*> Transforms;
		IRBuilder<> Builder(curLoad);
		Loop* PhiLoop = LI.getLoopFor(phi->getParent());

		for (auto& curDep : CapturedInstrs) {
			if (Transforms.count(curDep)) {
				continue;
			}
			if (curDep == phi) {
				if (PhiLoop == curLoadLoop) {
					// 1) figure out (ADD, MUL, GETELEMPTR)
					// 2) capture all exit conditions of BB
					// 3) figure out how to prefetch
					if (CheckLoopCond(PhiLoop)) {
						if (GetIncomingValue(PhiLoop, phi)) {
							Instruction* IncInstr = GetIncomingValue(PhiLoop, phi);
							if (IncInstr->getOpcode() == Instruction::Add && IncInstr->getOperand(0) == phi) {
								errs() << "ADD\n";
								Instruction* NewInstr;
								Instruction* mod;
								if (dyn_cast<ConstantInt>(IncInstr->getOperand(1))) {
									if (getCompareInstrADD(PhiLoop, IncInstr)) {
										Value* EndCond = nullptr;
										bool Changed = false;
										CmpInst* compareInstr = getCompareInstrADD(PhiLoop, IncInstr);
										if (PhiLoop->makeLoopInvariant(compareInstr->getOperand(0), Changed)) {
											EndCond = compareInstr->getOperand(0);
										} // makeLoopInvariant(0)
										if (PhiLoop->makeLoopInvariant(compareInstr->getOperand(1), Changed)) {
											EndCond = compareInstr->getOperand(1);
										} // makeLoopInvariant(1)
										ConstantInt* UpdateInd = getValueAddedToIndVar(PhiLoop, IncInstr);
										if (UpdateInd) {
											if (UpdateInd->isNegative()) {
												int64_t curPrefetchDist = 0 - prefetchDist;
												NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(curDep, curDep->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty((curDep->getParent())->getContext()), curPrefetchDist) : ConstantInt::get(Type::getInt32Ty((curDep->getParent())->getContext()), curPrefetchDist)));

												if (EndCond != nullptr) {
													if (EndCond->getType() != NewInstr->getType()) {
														Instruction* cast = CastInst::CreateIntegerCast(EndCond, NewInstr->getType(), true);
														Builder.Insert(cast);
														Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SGT, cast, NewInstr);
														mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, cast, NewInstr));
													} // if(EndCond->getType() != NewInstr->getType())
													else {
														Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SGT, EndCond, NewInstr);
														mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, EndCond, NewInstr));
													} // else(EndCond->getType() == NewInstr->getType())
												}	  // if(EndCond!=nullptr)
												Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
												donePrefetchingForPhi = true;
											} // isNegative()
											else {
												NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(curDep, curDep->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty((curDep->getParent())->getContext()), prefetchDist) : ConstantInt::get(Type::getInt32Ty((curDep->getParent())->getContext()), prefetchDist)));
												if (EndCond != nullptr) {
													if (EndCond->getType() != NewInstr->getType()) {
														Instruction* cast = CastInst::CreateIntegerCast(EndCond, NewInstr->getType(), true);
														Builder.Insert(cast);
														Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SLT, cast, NewInstr);
														mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, cast, NewInstr));
													} // if(EndCond->getType() != NewInstr->getType())
													else {
														Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SLT, EndCond, NewInstr);
														mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, EndCond, NewInstr));
													} // else(EndCond->getType() == NewInstr->getType())
													Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, mod));
												} /// if(EndCond!=nullptr)
												else {
													Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
												}
												donePrefetchingForPhi = true;
											} // else(Positive)
										}
									} // getCompareInstrADD
								}	  // getOperand(1)
							}		  // ADD
							if (IncInstr->getOpcode() == Instruction::Mul && IncInstr->getOperand(0) == phi) {
								errs() << "Mul\n";
								if (dyn_cast<ConstantInt>(IncInstr->getOperand(1))) {
									// errs()<< "   Operand#1: "<< *(dyn_cast<ConstantInt>(IncInstr->getOperand(1))) << "\n";
								} // getOperand(1)
							}	  // MUL

							if (IncInstr->getOpcode() == Instruction::GetElementPtr && IncInstr->getOperand(0) == phi) {
								GetElementPtrInst* NewInstr;
								Instruction* mod;
								SmallVector<Instruction*, 10> NextPhiDependencies;
								NextPhiDependencies.push_back(IncInstr);
								if (dyn_cast<ConstantInt>(IncInstr->getOperand(1))) {
									if (getCompareInstrGetElememntPtr(PhiLoop, IncInstr)) {
										Value* EndCond;
										bool Changed = false;
										CmpInst* compareInstr = getCompareInstrGetElememntPtr(PhiLoop, IncInstr);
										if (PhiLoop->makeLoopInvariant(compareInstr->getOperand(0), Changed)) {
											EndCond = compareInstr->getOperand(0);
											NextPhiDependencies.push_back(dyn_cast<Instruction>(compareInstr->getOperand(0)));
										} // makeLoopInvariant(0)
										else if (PhiLoop->makeLoopInvariant(compareInstr->getOperand(1), Changed)) {
											EndCond = compareInstr->getOperand(1);
											NextPhiDependencies.push_back(dyn_cast<Instruction>(compareInstr->getOperand(1)));
										} // makeLoopInvariant(1)
										else if (compareInstr->getOperand(1) != IncInstr && compareInstr->getOperand(0) == IncInstr) {
											EndCond = compareInstr->getOperand(1);
											NextPhiDependencies.push_back(dyn_cast<Instruction>(compareInstr->getOperand(1)));
										} // else if
										else if (compareInstr->getOperand(0) != IncInstr && compareInstr->getOperand(0) == IncInstr) {
											EndCond = compareInstr->getOperand(0);
											NextPhiDependencies.push_back(dyn_cast<Instruction>(compareInstr->getOperand(0)));
										} // else if

										NextPhiDependencies.push_back(compareInstr);
										ConstantInt* UpdateInd = getValueAddedToIndVar(PhiLoop, IncInstr);
										// Instruction* tempInst;
										Value* cmp;
										for (size_t index = 0; index < NextPhiDependencies.size(); index++) {
											if (NextPhiDependencies[index]->getOpcode() == Instruction::GetElementPtr) {
												if ((NextPhiDependencies[index]->getOperand(0) == curDep || NextPhiDependencies[index]->getOperand(1) == curDep)) {
													NewInstr = dyn_cast<GetElementPtrInst>(Builder.CreateInBoundsGEP(curDep, curDep->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty((curDep->getParent())->getContext()), prefetchDist) : ConstantInt::get(Type::getInt32Ty((curDep->getParent())->getContext()), prefetchDist)));
													Transforms.insert(std::pair<Instruction*, GetElementPtrInst*>(curDep, NewInstr));
													donePrefetchingForPhi = true;
													PrefetchGetElem = true;
												}
											}
										}
									} // getCompareInstrGetElememntPtr(PhiLoop, IncInstr)
									else {
										NewInstr = dyn_cast<GetElementPtrInst>(Builder.CreateInBoundsGEP(phi, ConstantInt::get(Type::getInt64Ty((curDep->getParent())->getContext()), prefetchDist)));
										Transforms.insert(std::pair<Instruction*, GetElementPtrInst*>(phi, NewInstr));
										donePrefetchingForPhi = true;
										PrefetchGetElem = true;
									}
								} // getOperand(1)
							}	  // Getelementptr
						}		  // if(GetIncomingValue(PhiLoop, phi))
					}			  // if(CheckLoopCond(PhiLoop))

					else {
						return done;
					}
				} // if(PhiLoop == curLoadLoop)
			}	  // if(curDep == phi)
		}		  // for(auto &curDep : CapturedInstrs)
		if (donePrefetchingForPhi) {
			for (int index = CapturedInstrs.size() - 1; index >= 0; index--) {
				auto& curDep = CapturedInstrs[index];
				if (!dyn_cast<PHINode>(curDep)) {
					Instruction* NewInstr = curDep->clone();
					Use* OpListNewInstr = NewInstr->getOperandList();
					int64_t NewInstrsNumOp = NewInstr->getNumOperands();
					for (int64_t index = 0; index < NewInstrsNumOp; index++) {
						Value* op = OpListNewInstr[index].get();
						if (dyn_cast<GetElementPtrInst>(op)) {
							GetElementPtrInst* opIsInstr = dyn_cast<GetElementPtrInst>(op);
							if (Transforms.count(opIsInstr)) {
								NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
							}
						} else if (Instruction* opIsInstr = dyn_cast<Instruction>(op)) {
							if (Transforms.count(opIsInstr)) {
								NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
							}
						}
					}
					NewInstr->insertBefore(curLoad);
					Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
				} // if(phi!=curDep)
			}	  // for(int index=CapturedInstrs.size()-1 ; index>=0; index--)

			if (!PrefetchGetElem) {
				Type* I32 = Type::getInt32Ty((curLoad->getParent())->getContext());
				Type* I8 = Type::getInt8PtrTy(((curLoad->getFunction())->getParent())->getContext());
				Function* PrefetchFunc = Intrinsic::getDeclaration((curLoad->getFunction())->getParent(), Intrinsic::prefetch, I8);
				Instruction* oldGep = dyn_cast<Instruction>(curLoad->getOperand(0));
				Instruction* gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
				Instruction* cast = dyn_cast<Instruction>(Builder.CreateBitCast(gep, Type::getInt8PtrTy(((curLoad->getFunction())->getParent())->getContext())));
				Value* ar[] = { cast, ConstantInt::get(I32, 0), ConstantInt::get(I32, 3), ConstantInt::get(I32, 1) };
				CallInst* call = CallInst::Create(PrefetchFunc, ar);
				call->insertBefore(curLoad);
			} else {
				Type* I32 = Type::getInt32Ty((curLoad->getParent())->getContext());
				Function* PrefetchFunc = Intrinsic::getDeclaration((curLoad->getFunction())->getParent(), Intrinsic::prefetch, (curLoad->getOperand(0))->getType());
				Instruction* oldGep = dyn_cast<Instruction>(curLoad->getOperand(0));
				Instruction* gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
				Value* ar[] = { gep, ConstantInt::get(I32, 0), ConstantInt::get(I32, 3), ConstantInt::get(I32, 1) };
				CallInst* call = CallInst::Create(PrefetchFunc, ar);
				call->insertBefore(curLoad);
			}
			if (IndirectLoads.size() > 0) {
				for (size_t index = 0; index < IndirectLoads.size(); index++) {
					auto& curStrideLoad = IndirectLoads[index];
					Loop* curStrideLoadLoop = LI.getLoopFor(curStrideLoad->getParent());
					if (curStrideLoadLoop == IndirectLoadLoop) {
						bool ItIsStrideLoad = false;
						Instruction* StridePhi = nullptr;
						SmallVector<Instruction*, 10> StrideLoads;
						SmallVector<Instruction*, 20> StrideInstrs;
						SmallVector<Instruction*, 10> StridePhis;
						int64_t StridePrefetchDist;
						if (SearchAlgorithm(curStrideLoad, LI, StridePhi, StrideLoads, StrideInstrs, StridePhis)) {
							for (size_t index = 0; index < StridePhis.size(); index++) {
								StrideInstrs.push_back(StridePhis[StridePhis.size() - 1 - index]);
							}
							bool NotFoundAPhi = false;
							for (long unsigned int j = 0; j < StridePhis.size(); j++) {
								if (!(std::find(IndirectPhis.begin(), IndirectPhis.end(), StridePhis[j]) != IndirectPhis.end())) {
									NotFoundAPhi = true;
								} // if(!(std::find(CapturedPhis.begin(),..)))
							}	  // for(long unsigned int j=0; j< StridePhis.size(); j++)
							bool NotFoundAnInstr = false;
							for (long unsigned int j = 0; j < StrideInstrs.size(); j++) {
								if (!(std::find(IndirectInstrs.begin(), IndirectInstrs.end(), StrideInstrs[j]) != IndirectInstrs.end())) {
									NotFoundAnInstr = true;
								} // if(!(std::find(CapturedPhis.begin(),..)))
							}	  // for(long unsigned int j=0; j< StridePhis.size(); j++)
							if (!NotFoundAnInstr && !NotFoundAPhi) {
								ItIsStrideLoad = true;
								StridePrefetchDist = IndirectPrefetchDist * (index + 2);
							} // if(!NotFoundAnInstr && !NotFoundAPhi)
							if (ItIsStrideLoad) {
								if (InjectPrefeches(curStrideLoad, LI, StridePhis, StrideLoads, StrideInstrs, StridePrefetchDist, false)) {
									done = true;
								} // if(InjectPrefeches(...))
							}	  // if(ItIsStrideLoad)
						}		  // if(SearchAlgorithm)
					}			  // if(curStrideLoadLoop == curLoadLoop)
				}				  // for(int index=CapturedLoads.size()-1 ; index>=0; index--)
			}					  // if(IndirectLoads.size()>0)

			done = true;
		} // if(donePrefetchingForPhi)
	}	  // if(Phi.size()==1)
	else {
		if (prefetchDist > 1000) {
			prefetchDist = prefetchDist / 1000;
			Loop* curPLoop;
			Loop* curLoop;
			std::vector<llvm::Instruction*> trans_new_instructions;
			std::vector<llvm::Instruction*> old_trans_new_instructions;
			std::vector<llvm::Instruction*> new_instructions;
			llvm::ValueToValueMapTy vmap;
			ValueMap<Instruction*, Value*> Transforms;
			Instruction* last;
			// Instruction* InsideLoopPhi;
			Instruction* cmp;
			Instruction* x;

			for (auto p : CapturedPhis) {
				curPLoop = LI.getLoopFor(p->getParent());
				curLoop = LI.getLoopFor(curLoad->getParent());
				// if(curPLoop ==curLoop)
				// InsideLoopPhi=p;
				if (curPLoop != curLoop) {
					phi = p;
					curPLoop = LI.getLoopFor(p->getParent());
					auto PEB = curPLoop->getExitingBlock();
					Value* EndCond;
					if (PEB) {
						SmallVector<llvm::Instruction*, 8> DepPhiInsts;
						SetVector<Instruction*> PEBInsts;
						if (PEB) {
							for (Instruction& J : *PEB) {
								Instruction* I = &J;
								PEBInsts.insert(I);
							}
						}
						bool CIexist = false;
						CmpInst* CI;
						for (int i = PEBInsts.size() - 1; i >= 0; i--) {
							// errs()<<"inst "<<i<<": "<< *PEBInsts[i]<<"\n";
							CI = dyn_cast<CmpInst>(PEBInsts[i]);
							if (CI) {
								CIexist = true;
							}
						}
						if (CIexist) {
							for (int i = PEBInsts.size() - 1; i >= 0; i--) {
								if (!dyn_cast<BranchInst>(PEBInsts[i]) && !dyn_cast<CallInst>(PEBInsts[i]) && !dyn_cast<PHINode>(PEBInsts[i]))
									DepPhiInsts.push_back(PEBInsts[i]);
							}
						}

						if (DepPhiInsts.size() > 0) {
							llvm::Instruction* insertPt = phi->getParent()->getFirstNonPHIOrDbg();
							for (int i = DepPhiInsts.size() - 1; i >= 0; i--) {
								auto* inst = DepPhiInsts[i];
								auto* new_inst = inst->clone();
								if (new_inst->getOpcode() == Instruction::Add) {
									Value* val;
									if (new_inst->getOperand(0)->getType()->isIntegerTy(64)) {
										val = ConstantInt::get(Type::getInt64Ty((curLoad->getParent())->getContext()), prefetchDist);
									} else {
										val = ConstantInt::get(Type::getInt32Ty((curLoad->getParent())->getContext()), prefetchDist);
									}
									new_inst->setOperand(1, val);
								}
								new_inst->insertBefore(insertPt);
								new_instructions.push_back(new_inst);
								vmap[inst] = new_inst;
								last = new_inst;
								insertPt = new_inst->getNextNode();
							}

							for (auto* i : new_instructions) {
								llvm::RemapInstruction(i, vmap, RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
								if (dyn_cast<CmpInst>(i))
									cmp = dyn_cast<CmpInst>(i);
							}
						}

						IRBuilder<> Builder(last->getNextNode());
						Instruction* NewInstr;
						NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(phi, phi->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty((curLoad->getParent())->getContext()), prefetchDist) : ConstantInt::get(Type::getInt32Ty((curLoad->getParent())->getContext()), prefetchDist)));
						Transforms.insert(std::pair<Instruction*, Instruction*>(phi, NewInstr));
						x = NewInstr;
						SmallVector<Instruction*, 20> SDepInstrs_insideLoop;
						for (int index = CapturedInstrs.size() - 1; index >= 0; index--) {
							if (LI.getLoopFor(curLoad->getParent()) == LI.getLoopFor(CapturedInstrs[index]->getParent())) {
								SDepInstrs_insideLoop.push_back(CapturedInstrs[index]);
							}
						}
						bool theSLoad = false;
						Instruction* SLoad;
						for (auto& t : SDepInstrs_insideLoop) {
							if (dyn_cast<LoadInst>(t)) {
								theSLoad = true;
								SLoad = t;
							}
						}
						Instruction* Sphi = nullptr;
						SmallVector<Instruction*, 10> SLoads;
						SmallVector<Instruction*, 20> SInstrs;
						SmallVector<Instruction*, 10> SPhis;

						if (theSLoad) {
							if (SearchAlgorithm(SLoad, LI, Sphi, SLoads, SInstrs, SPhis)) {
								for (size_t index = 0; index < SPhis.size(); index++) {
									SInstrs.push_back(SPhis[SPhis.size() - 1 - index]);
								}
								for (int index = SInstrs.size() - 1; index >= 0; index--) {
									auto& curDep = SInstrs[index];
								}
							}
						} // if(theSLoad)

						SmallVector<Instruction*, 20> InstrsToInsert;
						bool phiFound = false;
						int start_index;
						if (theSLoad) {
							InstrsToInsert = SInstrs;
						} else {
							InstrsToInsert = CapturedInstrs;
						}
						for (int index = InstrsToInsert.size() - 1; index >= 0; index--) {
							if (!phiFound) {
								auto& curDep = InstrsToInsert[index];
								Use* OpListNewInstr = curDep->getOperandList();
								int64_t NewInstrsNumOp = curDep->getNumOperands();
								for (int64_t i = 0; i < NewInstrsNumOp; i++) {
									if (OpListNewInstr[i].get() == phi) {
										phiFound = true;
										start_index = index;
									}
								}
							}
						}

						Instruction* last_gap;
						auto& curDep = InstrsToInsert[start_index];
						if (PHINode* pNode = dyn_cast<PHINode>(curDep)) {
							errs() << "\n";
						} else {
							Instruction* NewInstr = curDep->clone();
							Use* OpListNewInstr = NewInstr->getOperandList();
							int64_t NewInstrsNumOp = NewInstr->getNumOperands();
							for (int64_t index = 0; index < NewInstrsNumOp; index++) {
								Value* op = OpListNewInstr[index].get();
								if (dyn_cast<GetElementPtrInst>(op)) {
									GetElementPtrInst* opIsInstr = dyn_cast<GetElementPtrInst>(op);
									if (Transforms.count(opIsInstr)) {
										NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
									}
								} else if (Instruction* opIsInstr = dyn_cast<Instruction>(op)) {
									if (Transforms.count(opIsInstr)) {
										NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
									}
								}
							}
							NewInstr->insertAfter(x);
							last_gap = NewInstr;
							Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
							trans_new_instructions.push_back(NewInstr);
							old_trans_new_instructions.push_back(curDep);
							x = NewInstr;
						}
						bool insert = false;
						for (int index = start_index - 1; index >= 0; index--) {
							insert = false;
							auto& curDep = InstrsToInsert[index];
							if (PHINode* pNode = dyn_cast<PHINode>(curDep)) {
								errs() << "\n";
							} else {
								if (dyn_cast<GetElementPtrInst>(curDep)) {
									Instruction* temp = dyn_cast<GetElementPtrInst>(curDep);
									last_gap = temp;
									if ((std::find(CapturedPhis.begin(), CapturedPhis.end(), temp->getOperand(1)) != CapturedPhis.end())) {
										Value* val = ConstantInt::get(Type::getInt64Ty(((curLoad->getFunction())->getParent())->getContext()), 0);
										Instruction* NewInstr = curDep->clone();
										NewInstr->setOperand(1, val);
										Use* OpListNewInstr = NewInstr->getOperandList();
										int64_t NewInstrsNumOp = NewInstr->getNumOperands();
										for (int64_t index = 0; index < NewInstrsNumOp; index++) {
											Value* op = OpListNewInstr[index].get();
											if (dyn_cast<GetElementPtrInst>(op)) {
												GetElementPtrInst* opIsInstr = dyn_cast<GetElementPtrInst>(op);
												if (Transforms.count(opIsInstr)) {
													NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
													insert = true;
												}
											} else if (Instruction* opIsInstr = dyn_cast<Instruction>(op)) {
												if (Transforms.count(opIsInstr)) {
													NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
													insert = true;
												}
											}
										}
										NewInstr->insertAfter(x);
										Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
										trans_new_instructions.push_back(NewInstr);
										old_trans_new_instructions.push_back(curDep);
										x = NewInstr;
									} else {
										Instruction* NewInstr = curDep->clone();
										Use* OpListNewInstr = NewInstr->getOperandList();
										int64_t NewInstrsNumOp = NewInstr->getNumOperands();
										for (int64_t index = 0; index < NewInstrsNumOp; index++) {
											Value* op = OpListNewInstr[index].get();
											if (dyn_cast<GetElementPtrInst>(op)) {
												GetElementPtrInst* opIsInstr = dyn_cast<GetElementPtrInst>(op);
												if (Transforms.count(opIsInstr)) {
													NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
													insert = true;
												}
											} else if (Instruction* opIsInstr = dyn_cast<Instruction>(op)) {
												if (Transforms.count(opIsInstr)) {
													NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
													insert = true;
												}
											}
										}
										NewInstr->insertAfter(x);
										Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
										trans_new_instructions.push_back(NewInstr);
										old_trans_new_instructions.push_back(curDep);
										x = NewInstr;
									}
								} else {
									Instruction* NewInstr = curDep->clone();
									Use* OpListNewInstr = NewInstr->getOperandList();
									int64_t NewInstrsNumOp = NewInstr->getNumOperands();
									for (int64_t index = 0; index < NewInstrsNumOp; index++) {
										Value* op = OpListNewInstr[index].get();
										if (dyn_cast<GetElementPtrInst>(op)) {
											GetElementPtrInst* opIsInstr = dyn_cast<GetElementPtrInst>(op);
											if (Transforms.count(opIsInstr)) {
												NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
												insert = true;
											}
										} else if (Instruction* opIsInstr = dyn_cast<Instruction>(op)) {
											if (Transforms.count(opIsInstr)) {
												NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
												insert = true;
											}
										}
									}
									NewInstr->insertAfter(x);
									Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
									trans_new_instructions.push_back(NewInstr);
									old_trans_new_instructions.push_back(curDep);
									x = NewInstr;
								}
							}
						}

						Type* I8 = Type::getInt8PtrTy(((curLoad->getFunction())->getParent())->getContext());
						Type* I32 = Type::getInt32Ty((curLoad->getParent())->getContext());
						Function* PrefetchFunc = Intrinsic::getDeclaration((curLoad->getFunction())->getParent(), Intrinsic::prefetch, I8);
						Instruction* oldGep = dyn_cast<Instruction>(last_gap);
						Instruction* gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
						Instruction* cast = dyn_cast<Instruction>(Builder.CreateBitCast(gep, Type::getInt8PtrTy(((curLoad->getFunction())->getParent())->getContext())));
						Value* ar[] = { cast, ConstantInt::get(I32, 0), ConstantInt::get(I32, 3), ConstantInt::get(I32, 1) };
						CallInst* call = CallInst::Create(PrefetchFunc, ar);
						call->insertAfter(cast);


						x = call;




						Loop* curInstrLoop = LI.getLoopFor(cast->getParent()); // 指令所在的基本块所在循环
						BasicBlock* H = curInstrLoop->getHeader();			   // 循环内部的header
						BasicBlock *Incoming = nullptr, *Backedge = nullptr;
						pred_iterator PI = pred_begin(H); // 循环的所有前驱

						errs() << "the number of predecessors of BB is " << pred_size(H) << "\n";
						assert(PI != pred_end(H) && "Loop must have at least one backedge!"); // 正常的循环必然有两个前驱
						Backedge = *PI++;
						if (PI == pred_end(H)) {
							return false; // dead loop // 野循环
						}
						Incoming = *PI++;
						if (PI != pred_end(H)) {
							return false; // multiple backedges? // 多个前驱
						}
						PI = pred_begin(H);
						// BasicBlock* pred = Incoming;
						BasicBlock* pred = *PI;
						SetVector<Instruction*> BBInsts; // 前驱的所有指令
						SetVector<Instruction*> BBInsts2; // 前驱的所有指令
						for (Instruction& J : *pred) {
							Instruction* I = &J;
              // errs() << *I << "\n";
							BBInsts.insert(I); // 存储起来
						}
            // errs() << "==================================\n";
						Loop* lastLoop = LI.getLoopFor(pred); // 找到前驱所在的循环 也就是二重循环

						Loop* finalLoop = curInstrLoop;

						// 以下逻辑同上，找到二重循环的前驱的基本块的所有指令
						// curInstrLoop= LI.getLoopFor(cast->getParent());
						if (lastLoop != nullptr) {
							finalLoop = lastLoop;
							H = lastLoop->getHeader();

							Incoming = nullptr;
							Backedge = nullptr;
							PI = pred_begin(H);
							assert(PI != pred_end(H) && "Loop must have at least one backedge!");
							Backedge = *PI++;
							if (PI == pred_end(H)) {
								return false; // dead loop
							}
							Incoming = *PI++;
							if (PI != pred_end(H)) {
								return false; // multiple backedges?
							}
							PI = pred_begin(H);
							BasicBlock* pred1 = *PI;
							for (Instruction& J : *pred1) {
								Instruction* I = &J;
                // errs() << *I << "\n";
								BBInsts.insert(I);
							}
						}

						// 获取行号和函数名
						MDNode* MN = BBInsts[BBInsts.size() - 1]->getMetadata("dbg");
						unsigned LineNo;
						std::string SrcFileName;
						if (MN) {
							DILocation* Loc = dyn_cast<DILocation>(MN);
							if (Loc) {
								LineNo = Loc->getLine();
								errs() << "LineNo is " << LineNo << "\n";
								SrcFileName = Loc->getFilename().str();

								if (SrcFileName[0] == '.') {
									SrcFileName = SrcFileName.substr(2);
									errs() << SrcFileName << "\n";
								}
							}
						}

						// 获取规范循环变量
						SetVector<Instruction*> BBInsts1;
						auto B = finalLoop->getExitingBlock();
						if (!B)
							return false;
						for (Instruction& J : *B) {
							Instruction* I = &J;
							BBInsts1.insert(I);
						}
						bool Changed = false;
						Value* step = nullptr;
						for (int i = BBInsts1.size() - 1; i >= 0; i--) {
							CmpInst* CI = dyn_cast<CmpInst>(BBInsts1[i]);
							if (CI) {
								Instruction* AddI = dyn_cast<Instruction>(BBInsts1[i - 1]);
								if (AddI->getOpcode() == Instruction::Add) {
									if (finalLoop->makeLoopInvariant(AddI->getOperand(1), Changed)) {
										if (ConstantInt* constInt = dyn_cast<ConstantInt>(AddI->getOperand(1))) {
											step = CI->getOperand(0);
										}
									}
									if (finalLoop->makeLoopInvariant(AddI->getOperand(0), Changed)) {
										if (ConstantInt* constInt = dyn_cast<ConstantInt>(AddI->getOperand(1))) {
											step = CI->getOperand(1);
										}
									}
									// errs() << step->getName() << " " << step->getZExtValue() << "\n";
								}
							}
						}


						BasicBlock* currBB = cast->getParent();
						Module* M = currBB->getModule();
						LLVMContext& Ctx = M->getContext();

/*

            BasicBlock* probe_block = BasicBlock::Create(Ctx, "probe_block", currBB->getParent(), currBB);
            IRBuilder<> builder(probe_block);
            Value* sum = builder.CreateAdd(ConstantInt::get(IntegerType::getInt32Ty(Ctx), 1), ConstantInt::get(IntegerType::getInt32Ty(Ctx), 1));
            // IRBuilder<> builder1(Ctx);
            // builder1.SetInsertPoint(probe_block);
            IRBuilder<> builder2(currBB);
            builder.CreateBr(currBB);

            // 这单代码有问题，因为还需要修改currBB的phiNode指令，累了
*/

						// BasicBlock::iterator b = currBB->begin();
						Module::iterator m = M->begin();
						Function::iterator f = m->begin();
						BasicBlock::iterator b = f->begin();

						Instruction*test = &*(b++);


						// 构造printf指令，S是输出参数
						// auto S = StringRef("the lineno is %d, the loop count is %d\n");
						auto S = StringRef("the distance is %d\n");
						// auto S = StringRef(Twine(LineNo).str());
						// Value* LineNum = ConstantInt::get(IntegerType::getInt32Ty(Ctx), LineNo); // 变为value类型
						auto* CharPointerTy = PointerType::get(IntegerType::getInt8Ty(Ctx), 0);
						auto* PrintfTy = FunctionType::get(IntegerType::getInt32Ty(Ctx), { CharPointerTy }, true);
						auto Printf = M->getOrInsertFunction("printf", PrintfTy);
						auto Arr = ConstantDataArray::getString(Ctx, S);
						GlobalVariable* GV;// = new GlobalVariable(*M, Arr->getType(), true, GlobalValue::PrivateLinkage, Arr, ".str");
						// GV->setAlignment(MaybeAlign(1));
						// CallInst* myprint = CallInst::Create(Printf, { ConstantExpr::getBitCast(GV, CharPointerTy), LineNum, step }, "");

						// 插入到找到的基本块的最后一条指令前面
						// myprint->insertBefore(BBInsts[BBInsts.size() - 1]);
						
						// GlobalVariable* GV1;	
						// GV1 = new GlobalVariable(*M, IntegerType::getInt32Ty(Ctx), false, GlobalValue::ExternalLinkage, 0, "globaltest");
						// GV1->setAlignment(4);
						// Value* mmap = new LoadInst(GV1);

						auto* Int32Ty = IntegerType::getInt32Ty(Ctx);
						auto* Int64Ty = IntegerType::getInt64Ty(Ctx);
						auto* PointerI8Ty = PointerType::get(IntegerType::getInt8Ty(Ctx), 0);
						auto* Const64Ty = ConstantInt::get(IntegerType::getInt64Ty(Ctx), 0);
						auto* Const32Value1 = ConstantInt::get(IntegerType::getInt32Ty(Ctx), 0);

						auto* PointerI8Value = ConstantPointerNull::get(PointerType::get(IntegerType::getInt8Ty(Ctx), 0));

						auto* Const64Value1 = ConstantInt::get(IntegerType::getInt64Ty(Ctx), 1024);
						auto* Const32Value2 = ConstantInt::get(IntegerType::getInt32Ty(Ctx), 3);
						auto* Const64Value2 = ConstantInt::get(IntegerType::getInt64Ty(Ctx), 0);
						auto* Const64Value3 = ConstantInt::get(IntegerType::getInt64Ty(Ctx), 1);
						auto* Const64Value4 = ConstantInt::get(IntegerType::getInt64Ty(Ctx), 2);
						auto* Const64Value5 = ConstantInt::get(IntegerType::getInt64Ty(Ctx), 10);
						auto* Const32Value3 = ConstantInt::get(IntegerType::getInt32Ty(Ctx), 1);
						auto* Const32Value4 = ConstantInt::get(IntegerType::getInt32Ty(Ctx), 66);
						auto* Const32Value5 = ConstantInt::get(IntegerType::getInt32Ty(Ctx), 2);
						auto* Const8Value = ConstantInt::get(IntegerType::getInt8Ty(Ctx), 97);

						auto openTy = FunctionType::get(Int32Ty, { PointerI8Ty, Int32Ty }, true);
						auto mmapTy = FunctionType::get(PointerI8Ty, { PointerI8Ty, Int64Ty, Int32Ty, Int32Ty, Int32Ty, Int64Ty }, true);
						auto closeTy = FunctionType::get(Int32Ty, { Int32Ty }, true);
            auto lseekTy = FunctionType::get(Int64Ty, { Int32Ty, Int64Ty, Int32Ty}, true);
            auto atoiTy = FunctionType::get(Int32Ty, { PointerI8Ty  }, true);
            // auto memcpyTy = FunctionType::get(Type::getVoidTy(Ctx), { PointerI8Ty, PointerI8Ty, Int64Ty, IntegerType::getInt1Ty(Ctx) }, true);

						S = StringRef("myTest.txt");
						Arr = ConstantDataArray::getString(Ctx, S);
						GV = new GlobalVariable(*M, Arr->getType(), true, GlobalValue::PrivateLinkage, Arr, ".str1");

						auto Open = M->getOrInsertFunction("open", openTy);
						auto MMap = M->getOrInsertFunction("mmap", mmapTy);
						auto Close = M->getOrInsertFunction("close", closeTy);
            auto Lseek = M->getOrInsertFunction("lseek", lseekTy);
            auto Atoi = M->getOrInsertFunction("atoi", atoiTy);
            // auto Memcpy = M->getOrInsertFunction("llvm.memcpy.p018.p018.i64", memcpyTy);

						CallInst* myopen = CallInst::Create(Open, { ConstantExpr::getBitCast(GV, CharPointerTy), Const32Value4 }, "myopen");
						CallInst* mymmap = CallInst::Create(MMap, { PointerI8Value, Const64Value1, Const32Value2, Const32Value3, myopen, Const64Value2 }, "mymmap");
            ArrayType* arrayType = ArrayType::get(IntegerType::getInt8Ty(Ctx), 10);
            AllocaInst* Alloc_prefetch_distance = new AllocaInst(arrayType, 0,  "prefetch_distance");
            Alloc_prefetch_distance->setAlignment(Align(1));

            Alloc_prefetch_distance->insertBefore(test);
            myopen->insertBefore(test);
            mymmap->insertBefore(test);


            GetElementPtrInst* get_prefetch_distance = GetElementPtrInst::Create(arrayType, Alloc_prefetch_distance, {Const64Value2, Const64Value2}, "get_prefetch_distance");

            get_prefetch_distance->insertBefore(test);
            GetElementPtrInst* get_mmap2 = GetElementPtrInst::Create(IntegerType::getInt8Ty(Ctx), mymmap, Const64Value4, "get_mmap2");
            get_mmap2->insertBefore(test);


            IRBuilder<> Builder1(cast);
            CallInst *myMemcpy = Builder1.CreateMemCpy(get_prefetch_distance, Align(1), get_mmap2, Align(1), Const64Value5);

            LoadInst* load_prefetch_distance = new LoadInst(IntegerType::getInt8Ty(Ctx), mymmap, "load_prefetch_distance", 0, Align(1));;
            SExtInst* atoi = new SExtInst(load_prefetch_distance, IntegerType::getInt32Ty(Ctx), "atoi");

            load_prefetch_distance->insertBefore(cast);
            atoi->insertBefore(cast);
						

            S = StringRef("the distance is %d\n");
            Arr = ConstantDataArray::getString(Ctx, S);
            GV = new GlobalVariable(*M, Arr->getType(), true, GlobalValue::PrivateLinkage, Arr, ".str1");

            // CallInst* myprint = CallInst::Create(Printf, { ConstantExpr::getBitCast(GV, CharPointerTy), atoi}, "");
            //myprint->insertBefore(call);

/*
						AllocaInst* AllocFd = new AllocaInst(IntegerType::getInt32Ty(Ctx), 0, "myFd");
						AllocFd->setAlignment(Align(4));
						AllocaInst* AllocMmap = new AllocaInst(PointerType::get(IntegerType::getInt8Ty(Ctx), 0), 0, "mmap");
						AllocMmap->setAlignment(Align(8));
            AllocaInst *AllocStoreLseek = new AllocaInst(IntegerType::getInt64Ty(Ctx), 0, "storeLseek");
            AllocStoreLseek->setAlignment(Align(4));
            ArrayType* arrayType = ArrayType::get(IntegerType::getInt8Ty(Ctx), 1024);
            AllocaInst* Alloc_prefetch_distance = new AllocaInst(arrayType, 0,  "prefetch_distance");
            Alloc_prefetch_distance->setAlignment(Align(16));
            
						StoreInst* storeFd = new StoreInst(myopen, AllocFd, 0, Align(4));
						LoadInst* loadFd = new LoadInst(IntegerType::getInt32Ty(Ctx), AllocFd, "loadopenfd", 0, Align(4));
						CallInst* mymmap = CallInst::Create(MMap, { PointerI8Value, Const64Value1, Const32Value2, Const32Value3, loadFd, Const64Value2 }, "");
						StoreInst* storemmap = new StoreInst(mymmap, AllocMmap, 0, Align(8));
            LoadInst* loadlseekFd = new LoadInst(IntegerType::getInt32Ty(Ctx), AllocFd, "loadlseekfd", 0, Align(4));
            CallInst* mylseek = CallInst::Create(Lseek, {loadlseekFd, Const64Value2, Const32Value5 }, "");
            // TruncInst* myTrunc = new TruncInst(mylseek, IntegerType::getInt32Ty(Ctx), "myTrunc");


            StoreInst* storelen = new StoreInst(mylseek, AllocStoreLseek, 0, Align(4));
            
            LoadInst* load_start1 = new LoadInst(PointerType::get(IntegerType::getInt8Ty(Ctx),0), AllocMmap, "load_start1", 0, Align(8));
            GetElementPtrInst* get_start1 = GetElementPtrInst::Create(IntegerType::getInt8Ty(Ctx), load_start1, Const64Value3, "get_start1");
            LoadInst* load_get_prefetch = new LoadInst(IntegerType::getInt8Ty(Ctx), get_start1, "load_get_prefetch" ,0, Align(1));
            AllocaInst* AllocPrefetch = new AllocaInst(IntegerType::getInt8Ty(Ctx), 0, "alloc_prefetch");
            AllocPrefetch->setAlignment(Align(1));
            StoreInst* store_prefetch = new StoreInst(load_get_prefetch, AllocPrefetch, 0, Align(1));

            GetElementPtrInst* get_prefetch_distance = GetElementPtrInst::Create(arrayType, Alloc_prefetch_distance, {Const64Value2, Const64Value2}, "get_prefetch_distance");

            LoadInst* load_start = new LoadInst(PointerType::get(IntegerType::getInt8Ty(Ctx), 0), AllocMmap, "load_start", 0, Align(8));
            GetElementPtrInst* get_start = GetElementPtrInst::Create(IntegerType::getInt8Ty(Ctx), load_start, Const64Value4, "get_start");
            LoadInst* load_len = new LoadInst(IntegerType::getInt64Ty(Ctx), AllocStoreLseek, "load_len", 0, Align(8));
            Instruction *sub_len = BinaryOperator::Create(Instruction::Sub, load_len, ConstantInt::get(IntegerType::getInt64Ty(Ctx), 5), "sub_len");
            // CallInst* myMemcpy = CallInst::Create(Memcpy, {get_prefetch_distance, get_start, sub_len, ConstantInt::get(IntegerType::getInt1Ty(Ctx), 0)}, ""); 
            CallInst *myMemcpy = (Builder.CreateMemCpy(get_prefetch_distance, Align(16), get_start, Align(1), sub_len));

            GetElementPtrInst* get_end = GetElementPtrInst::Create(arrayType, Alloc_prefetch_distance, {Const64Value2, sub_len}, "get_end");
            StoreInst* store_end = new StoreInst(ConstantInt::get(IntegerType::getInt8Ty(Ctx), 10), get_end, 0, Align(1));
            GetElementPtrInst* get_distance = GetElementPtrInst::Create(arrayType, Alloc_prefetch_distance, {Const64Value2, Const64Value2}, "get_distance");
            CallInst* myAtoi = CallInst::Create(Atoi, { get_distance  }, "");
            AllocaInst* Alloc_distance = new AllocaInst(IntegerType::getInt32Ty(Ctx), 0, "alloc_distance");
            Alloc_distance->setAlignment(Align(4));
            StoreInst* store_distance = new StoreInst(myAtoi, Alloc_distance, 0, Align(4));          


            LoadInst* load_distance = new LoadInst(IntegerType::getInt32Ty(Ctx), Alloc_distance, "load_distance", 0, Align(4));
            CmpInst* mycmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ, load_distance, ConstantInt::get(IntegerType::getInt32Ty(Ctx), 0), "my_cmp");


            LoadInst* loadCloseFd = new LoadInst(IntegerType::getInt32Ty(Ctx), AllocFd, "loadclosefd", 0, Align(4));
            CallInst* myclose = CallInst::Create(Close, { loadCloseFd }, "");

            LoadInst* loadmmap = new LoadInst(PointerType::get(IntegerType::getInt8Ty(Ctx), 0), AllocMmap, "loadmmap", 0, Align(8));
            GetElementPtrInst* getmmap = GetElementPtrInst::Create(IntegerType::getInt8Ty(Ctx), loadmmap, { Const64Value2 }, "mmapStr");
            StoreInst* mywrite = new StoreInst(Const8Value, getmmap, 0, Align(1));

            store_prefetch->insertBefore(call);
            store_end->insertBefore(call);
            get_distance->insertBefore(call);
            myAtoi->insertBefore(call);
            store_distance->insertBefore(call);
            load_distance->insertBefore(call);
            mycmp->insertBefore(call);
            mywrite->insertBefore(call);



            std::string str = "access_block"+Twine(block_count).str();
            block_count++;

            Instruction* inst = call->getNextNonDebugInstruction();
            BasicBlock* access_block = currBB->splitBasicBlock(inst, str);

            str = "prefetch_block"+Twine(block_count).str();
            BasicBlock* prefetch_block = currBB->splitBasicBlock(call, str);
            BranchInst* myBr = BranchInst::Create(access_block, prefetch_block, mycmp);
            
            Instruction* Terminator = currBB->getTerminator();
            llvm::ReplaceInstWithInst(Terminator, myBr);



						AllocMmap->insertBefore(test);
						AllocFd->insertBefore(test);
            Alloc_prefetch_distance->insertBefore(test);
            AllocStoreLseek->insertBefore(test);
            AllocPrefetch->insertBefore(test);
            Alloc_distance->insertBefore(test);


            Alloc_prefetch_distance->insertBefore(test);
						myopen->insertBefore(test);
						mymmap->insertBefore(test);
            get_prefetch_distance->insertBefore(test);
            get_mmap2->insertBefore(test);

            load_prefetch_distance->insertBefore(cast);
            atoi->insertBefore(cast);

						storeFd->insertBefore(test);
						loadFd->insertBefore(test);
						mymmap->insertBefore(test);
						storemmap->insertBefore(test);
            loadlseekFd->insertBefore(test);
            mylseek->insertBefore(test);
            storelen->insertBefore(test);
						loadCloseFd->insertBefore(test);
						myclose->insertBefore(test);

						loadmmap->insertBefore(test);
						getmmap->insertBefore(test);
            
            load_start1->insertBefore(test);
            get_start1->insertBefore(test);
            load_get_prefetch->insertBefore(test);

            get_prefetch_distance->insertBefore(test);
            
            load_start->insertBefore(test);
            get_start->insertBefore(test);
            load_len->insertBefore(test);
            sub_len->insertBefore(test);
            get_end->insertBefore(test);

            // store_prefetch->insertBefore(BBInsts[BBInsts.size() - 1]);
            // store_prefetch->insertBefore(call);
            // store_end->insertBefore(call);
            // get_distance->insertBefore(call);
            // myAtoi->insertBefore(call);
            // store_distance->insertBefore(call);
            // load_distance->insertBefore(call);
            // mycmp->insertBefore(call);
						// mywrite->insertBefore(call);
*/

/*

            AllocaInst* Alloc_distance = new AllocaInst(IntegerType::getInt32Ty(Ctx), 0, "alloc_distance");
            Alloc_distance->setAlignment(Align(4));
            Alloc_distance->insertBefore(test);

            LoadInst* load_distance = new LoadInst(IntegerType::getInt32Ty(Ctx), Alloc_distance, "load_distance", 0, Align(4));
            load_distance->insertBefore(call);
*/
            CmpInst* mycmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ, atoi, ConstantInt::get(IntegerType::getInt32Ty(Ctx), 0), "my_cmp");
            mycmp->insertBefore(call);

            std::string str = "access_block"+Twine(block_count).str();

            Instruction* inst = call->getNextNonDebugInstruction();
            BasicBlock* access_block = currBB->splitBasicBlock(inst, str);

            str = "prefetch_block"+Twine(block_count).str();
            BasicBlock* prefetch_block = currBB->splitBasicBlock(call, str);
            block_count++;


            BranchInst* myBr = BranchInst::Create(access_block, prefetch_block, mycmp);

            Instruction* Terminator = currBB->getTerminator();
            llvm::ReplaceInstWithInst(Terminator, myBr);



					}
				}
			}
		} // prefetchDist>1000
		else {
			Instruction* InnerPhi = nullptr;
			Loop* LoadLoop = LI.getLoopFor(curLoad->getParent());
			SmallVector<llvm::Instruction*, 10> InnerPhis;
			for (int index = CapturedPhis.size() - 1; index >= 0; index--) {
				Loop* InnerPhiLoop = LI.getLoopFor(CapturedPhis[index]->getParent());
				if (InnerPhiLoop == LoadLoop) {
					InnerPhi = CapturedPhis[index];
					InnerPhis.push_back(InnerPhi);
					if (InjectPrefeches(curLoad, LI, InnerPhis, CapturedLoads, CapturedInstrs, prefetchDist, true)) {
						done = true;
					}
				} // if( InnerPhiLoop ==LoadLoop )
			}	  // for(int index=CapturedPhis.size()-1 ; index>=0; index--)
		}		  // prefetchDist<1000
	}			  // else(CapturedPhis.size()!=1)
	return done;
}

bool LLCHintLLVMPass::InjectPrefechesOnePhiPartTwo(Instruction* I, LoopInfo& LI, Instruction* Phi, SmallVector<Instruction*, 20>& DepInstrs, int64_t prefetchDist)
{
	bool done = false;
	bool nonCanonical = false;
	Instruction* phi = nullptr;
	phi = Phi;
	ValueMap<Instruction*, Value*> Transforms;
	IRBuilder<> Builder(I);

	Loop* curLoop = LI.getLoopFor(phi->getParent());
	if (!getLoopEndCondxxx(curLoop)) {
		SmallVector<llvm::Instruction*, 4> DepPhiInsts;
		// Value* EndInstr;
		for (auto& curDep : DepInstrs) {
			if (curDep == phi) {
				SetVector<Instruction*> BBInsts;
				auto B = curLoop->getExitingBlock();
				if (B) {
					for (Instruction& J : *B) {
						Instruction* I = &J;
						BBInsts.insert(I);
					}
					for (int i = BBInsts.size() - 1; i >= 0; i--) {
						CmpInst* CI = dyn_cast<CmpInst>(BBInsts[i]);
						if (CI) {
							DepPhiInsts.push_back(CI);
							Use* OperandList = CI->getOperandList();
							Use* NumOfOperands = OperandList + CI->getNumOperands();
							for (Use* op = OperandList; op < NumOfOperands; op++) {
								if (dyn_cast<Instruction>(op->get())) {
									Instruction* OPInstr = dyn_cast<Instruction>(op->get());
									DepPhiInsts.push_back(OPInstr);
								}
							}
						}
					}
				}
				Instruction* NewInstr;
				if (curDep == getCanonicalishInductionVariable(curLoop)) {
					Instruction* mod;
					NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(curDep, curDep->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty((curDep->getFunction())->getParent()->getContext()), prefetchDist) : ConstantInt::get(Type::getInt32Ty(((curDep->getFunction())->getParent())->getContext()), prefetchDist)));
					Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
					for (auto& s : DepPhiInsts) {
						DepInstrs.push_back(s);
					}
					for (auto& s : DepPhiInsts) {
						Use* OpsInstr = s->getOperandList();
						int64_t sNumOp = s->getNumOperands();
						for (int64_t index = 0; index < sNumOp; index++) {
							Value* ops = OpsInstr[index].get();
							Instruction* m = dyn_cast<Instruction>(ops);
							if (!(std::find(DepInstrs.begin(), DepInstrs.end(), m) != DepInstrs.end())) {
								if (!(dyn_cast<ConstantInt>(ops))) {
									DepInstrs.push_back(m);
								}
							}
						}
					}
				}
			} // if(curDep == phi){
		}	  // for(auto &curDep : DepInstrs)
	}		  // if(!getLoopEndCond(curLoop))
	else {
		for (auto& curDep : DepInstrs) {
			if (Transforms.count(curDep)) {
				continue;
			}
			if (curDep == phi) {
				Instruction* NewInstr;
				if (curDep == getCanonicalishInductionVariable(curLoop)) {
					Value* EndCond = getLoopEndCondxxx(curLoop);
					Instruction* IncInstr = GetIncomingValue(curLoop, phi);
					ConstantInt* UpdateInd = getValueAddedToIndVar(curLoop, IncInstr);
					Instruction* mod;
					if (UpdateInd->isNegative()) {
						int64_t curprefetchDist = 0 - prefetchDist;
						NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(curDep, curDep->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty(((curDep->getFunction())->getParent())->getContext()), curprefetchDist) : ConstantInt::get(Type::getInt32Ty(((curDep->getFunction())->getParent())->getContext()), curprefetchDist)));
						if (EndCond->getType() != NewInstr->getType()) {
							Instruction* cast = CastInst::CreateIntegerCast(EndCond, NewInstr->getType(), true);
							Builder.Insert(cast);
							Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SGT, cast, NewInstr);
							mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, cast, NewInstr));
						} else {
							Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SGT, EndCond, NewInstr);
							mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, EndCond, NewInstr));
						}
						Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
					} else {
						NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(curDep, curDep->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty(((curDep->getFunction())->getParent())->getContext()), prefetchDist) : ConstantInt::get(Type::getInt32Ty(((curDep->getFunction())->getParent())->getContext()), prefetchDist)));
						if (EndCond->getType() != NewInstr->getType()) {
							Instruction* cast = CastInst::CreateIntegerCast(EndCond, NewInstr->getType(), true);
							Builder.Insert(cast);
							Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SLT, cast, NewInstr);
							mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, cast, NewInstr));
						} else {
							Value* cmp = Builder.CreateICmp(CmpInst::ICMP_SLT, EndCond, NewInstr);
							mod = dyn_cast<Instruction>(Builder.CreateSelect(cmp, EndCond, NewInstr));
						}
						Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, mod));
					}
				} else {
					nonCanonical = true;
				}
			}
		}
	}
	int start = 0;
	if (nonCanonical) {
		GetElementPtrInst* newPhi;
		newPhi = dyn_cast<GetElementPtrInst>(Builder.CreateInBoundsGEP(phi, phi->getType()->isIntegerTy(64) ? ConstantInt::get(Type::getInt64Ty(((phi->getFunction())->getParent())->getContext()), prefetchDist) : ConstantInt::get(Type::getInt32Ty(((phi->getFunction())->getParent())->getContext()), prefetchDist)));
		Transforms.insert(std::pair<Instruction*, GetElementPtrInst*>(phi, newPhi));
		start = 1;
	}
	SmallVector<Instruction*, 20> t;
	for (int index = DepInstrs.size() - 1; index >= 0; index--) {
		auto& curDep = DepInstrs[index];
		if (PHINode* pNode = dyn_cast<PHINode>(curDep)) {
			errs() << "\n";
		} // if
		else {
			// errs()<<"  "<< *curDep<<"\n";
			Instruction* NewInstr = curDep->clone();
			Use* OpListNewInstr = NewInstr->getOperandList();
			int64_t NewInstrsNumOp = NewInstr->getNumOperands();
			for (int64_t index = 0; index < NewInstrsNumOp; index++) {
				Value* op = OpListNewInstr[index].get();
				if (dyn_cast<GetElementPtrInst>(op)) {
					GetElementPtrInst* opIsInstr = dyn_cast<GetElementPtrInst>(op);
					if (Transforms.count(opIsInstr)) {
						NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
					} // if
				}	  // if
				else if (Instruction* opIsInstr = dyn_cast<Instruction>(op)) {
					if (Transforms.count(opIsInstr)) {
						NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
					} // if(Transforms)
				}	  // else if
			}		  // for
			NewInstr->insertBefore(I);
			t.push_back(NewInstr);
			Transforms.insert(std::pair<Instruction*, Instruction*>(curDep, NewInstr));
		} // else
	}	  // for

	Type* I8 = Type::getInt8PtrTy(((I->getFunction())->getParent())->getContext());
	Type* I32 = Type::getInt32Ty((I->getParent())->getContext());
	Function* PrefetchFunc = Intrinsic::getDeclaration((I->getFunction())->getParent(), Intrinsic::prefetch, I8);
	Instruction* oldGep = dyn_cast<Instruction>(I->getOperand(0));
	Instruction* gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
	Instruction* cast = dyn_cast<Instruction>(Builder.CreateBitCast(gep, Type::getInt8PtrTy(((I->getFunction())->getParent())->getContext())));
	Value* ar[] = { cast, ConstantInt::get(I32, 0), ConstantInt::get(I32, 3), ConstantInt::get(I32, 1) };
	CallInst* call = CallInst::Create(PrefetchFunc, ar);
	call->insertBefore(I);
	done = true;
	return done;
}

bool LLCHintLLVMPass::InjectPrefechesOnePhiPartOne(Instruction* I, LoopInfo& LI, SmallVector<llvm::Instruction*, 10>& Phi, SmallVector<llvm::Instruction*, 10>& CapturedLoads, SmallVector<Instruction*, 20>& DepInstrs, int64_t prefetchDist, bool ItIsIndirectLoad)
{
	bool done = false;
	Instruction* phi = nullptr;
	SmallVector<Instruction*, 10> DependentLoadsToCurLoadx;
	SmallVector<Instruction*, 20> DependentInstrsToCurLoadx;
	SmallVector<Instruction*, 10> DependentPhisx;

	if (IsDep(I, LI, phi, DependentLoadsToCurLoadx, DependentInstrsToCurLoadx, DependentPhisx)) {
		Instruction* SearchPhi = nullptr;
		SmallVector<Instruction*, 10> SearchLoads;
		SmallVector<Instruction*, 20> SearchInstrs;
		SmallVector<Instruction*, 10> SearchPhis;
		for (int index = DependentLoadsToCurLoadx.size() - 1; index >= 0; index--) {
			if (IsDep(DependentLoadsToCurLoadx[index], LI, SearchPhi, SearchLoads, SearchInstrs, SearchPhis)) {
				if (DependentPhisx[0] == SearchPhis[0]) {
					if (InjectPrefechesOnePhiPartTwo(I, LI, DependentPhisx[0], DependentInstrsToCurLoadx, prefetchDist)) {
						done = true;
					}
					if (InjectPrefechesOnePhiPartTwo(DependentLoadsToCurLoadx[index], LI, SearchPhis[0], SearchInstrs, prefetchDist * 2)) {
						done = true;
					}
				}
			}
		}
	}
	return done;
}

bool LLCHintLLVMPass::runOnFunction(Function& F)
{
	bool modified = false;
	LoopInfo& LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
	if (!Reader) {
		errs() << "Reader false\n";
		return false;
	}
	bool samplesExist = false;
	const llvm::sampleprof::FunctionSamples* SamplesReaded = Reader->getSamplesFor(F);
	if (SamplesReaded) {
		samplesExist = true;
	}
	/*if(!SamplesReaded){
		errs()<<F.getName() << "   no-sample!\n";
	}*/


/*

   MDNode* MDN;
   unsigned Line;
   DILocation* Loc;
   for (auto &BB : F) {
       for (auto &I : BB) {
           errs() << I << "\n"; // Output the instruction
            MDN = I.getMetadata("dbg");
            if (MDN) {
              Loc = dyn_cast<DILocation>(MDN);
              if (Loc) {
                Line = Loc->getLine();
                errs() << "Line is " << Line << "\n";
              }
              else {
                errs() << "no Loc\n";
              }
            }
            else {
              errs() << "mo metadata\n";
            }
       }
   }


*/

	if (samplesExist) {
		int64_t prefechDist;
		SmallVector<Instruction*, 30> AllCurLoads;
		SmallVector<Instruction*, 20> NeedToEliminateCurLoads;
		SmallVector<int64_t, 30> AllPrefetchDist;
		SmallVector<int64_t, 20> IndexofNeedToEliminateCurLoads;
		SmallVector<int64_t, 20> correctMapping;
		std::vector<SmallVector<Instruction*, 20>> AllCapturedInstrs;
		std::vector<SmallVector<Instruction*, 10>> AllCapturedPhis;
		std::vector<SmallVector<Instruction*, 10>> AllCapturedLoads;

		for (auto& BB : F) {
			bool isBBLoop = LI.getLoopFor(&BB);
	
			if (!isBBLoop) {
				// errs() << pred_size(&BB) << "\n";
			} else {
				for (auto& I : BB) {
					const ErrorOr<Hints> T = getHints(I, SamplesReaded);
					if (T) {
						// errs()<<"T is true!\n";
						if (LoadInst* curLoad = dyn_cast<LoadInst>(&I)) {
							for (const auto& S_V : *T) {
								prefechDist = static_cast<int64_t>(S_V.second);
								Instruction* phi = nullptr;
								SmallVector<Instruction*, 10> Loads;
								SmallVector<Instruction*, 20> Instrs;
								SmallVector<Instruction*, 10> Phis;

								if (SearchAlgorithm(curLoad, LI, phi, Loads, Instrs, Phis)) {
									for (size_t index = 0; index < Phis.size(); index++) {
										Instrs.push_back(Phis[Phis.size() - 1 - index]);
									}
									AllCurLoads.push_back(curLoad);
									AllPrefetchDist.push_back(prefechDist);
									AllCapturedInstrs.push_back(Instrs);
									AllCapturedPhis.push_back(Phis);
									AllCapturedLoads.push_back(Loads);

								} // SearchAlgorithm
							}	  // auto &S_V : *T
						}		  // dyn_cast<LoadInst>(&I)
					}			  // T
				}				  // auto &I : BB
			}					  // isBBLoop
		}						  // auto &BB : F

		bool correctMappingCheck = false;
		SmallVector<Instruction*, 10> AlreadyPrefetched;

		if (AllCurLoads.size() > 1) {
			for (long unsigned int i = 0; i < AllCurLoads.size(); i++) {
				for (long unsigned int j = 0; j < AllCurLoads.size(); j++) {
					if (AllCapturedInstrs[i].size() == AllCapturedInstrs[j].size() && AllCurLoads[i] != AllCurLoads[j]) {
						if (AllCapturedLoads[i].size() == AllCapturedLoads[j].size() && AllCapturedPhis[i].size() == AllCapturedPhis[j].size()) {
							if (!(std::find(correctMapping.begin(), correctMapping.end(), i) != correctMapping.end())) {
								correctMapping.push_back(i);
								correctMappingCheck = true;
							}
						}
					}
				}
			}
		}
		if (correctMappingCheck) {
			for (long unsigned int j = 0; j < AllCurLoads.size(); j++) {
				if (!(std::find(correctMapping.begin(), correctMapping.end(), j) != correctMapping.end())) {
					if (!(std::find(AlreadyPrefetched.begin(), AlreadyPrefetched.end(), AllCurLoads[j]) != AlreadyPrefetched.end())) {
						AlreadyPrefetched.push_back(AllCurLoads[j]);
						if (AllCapturedPhis[j].size() > 1) {
							if (InjectPrefeches(AllCurLoads[j], LI, AllCapturedPhis[j], AllCapturedLoads[j], AllCapturedInstrs[j], AllPrefetchDist[j], true)) {
								modified = true;
							}
						} else if (AllCapturedPhis[j].size() == 1 && AllCapturedLoads[j].size() != 0) {
							if (InjectPrefechesOnePhiPartOne(AllCurLoads[j], LI, AllCapturedPhis[j], AllCapturedLoads[j], AllCapturedInstrs[j], AllPrefetchDist[j], true)) {
								modified = true;
							}
						}
					}
				}
			}
		}
		if (!correctMappingCheck) {
			for (long unsigned int j = 0; j < AllCurLoads.size(); j++) {
				if (!(std::find(AlreadyPrefetched.begin(), AlreadyPrefetched.end(), AllCurLoads[j]) != AlreadyPrefetched.end())) {
					AlreadyPrefetched.push_back(AllCurLoads[j]);
					if (AllCapturedPhis[j].size() > 1) {
						if (InjectPrefeches(AllCurLoads[j], LI, AllCapturedPhis[j], AllCapturedLoads[j], AllCapturedInstrs[j], AllPrefetchDist[j], true)) {
							modified = true;
						}
					} else if (AllCapturedPhis[j].size() == 1) {
						if (InjectPrefechesOnePhiPartOne(AllCurLoads[j], LI, AllCapturedPhis[j], AllCapturedLoads[j], AllCapturedInstrs[j], AllPrefetchDist[j], true)) {
							modified = true;
						}
					}
				}
			}
		}
	}
	if (!AutoFDOMapping) {
		SmallVector<Instruction*, 10> AllLoadsDepToPhix;
		int64_t pd;
		for (auto& e : LBR_dist) {
			pd = std::stoull(e);
		}
		std::vector<SmallVector<Instruction*, 20>> AllDependentInstsx;
		std::vector<SmallVector<Instruction*, 10>> AllDependentPhisx;
		SmallVector<Instruction*, 10> StrideLoadsx;
		SmallVector<Instruction*, 10> StrideLoadsToKeepx;
		SmallVector<Instruction*, 10> IndirectLoadsx;
		SmallVector<Instruction*, 10> IndirectLoadsToKeepx;
		SmallVector<Instruction*, 10> LoadsToRemovex;
		SmallVector<int, 10> LoadsIndexx;

		std::vector<SmallVector<Instruction*, 20>> AllDependentInstrsToIndirectLoadx;
		std::vector<SmallVector<Instruction*, 20>> AllDependentInstrsToStrideLoadx;
		std::vector<SmallVector<Instruction*, 10>> AllDependentPhisToStrideLoadx;
		std::vector<SmallVector<Instruction*, 10>> AllDependentPhisToIndirectLoadx;

		for (auto& BB : F) {
			bool isBBLoop = LI.getLoopFor(&BB);
			for (auto& I : BB) {
				if (isBBLoop) {
					if (LoadInst* curLoad = dyn_cast<LoadInst>(&I)) {
						Instruction* phi = nullptr;
						SmallVector<Instruction*, 10> DependentLoadsToCurLoadx;
						SmallVector<Instruction*, 20> DependentInstrsToCurLoadx;
						SmallVector<Instruction*, 10> DependentPhisx;
						if (IsDep(curLoad, LI, phi, DependentLoadsToCurLoadx, DependentInstrsToCurLoadx, DependentPhisx)) {
							if (DependentLoadsToCurLoadx.size() > 0) {
								int indexOfDepLoad;
								bool DepPhiOfDepLoad = false;
								for (auto& s : DependentLoadsToCurLoadx) {
									for (long unsigned int i = 0; i < AllLoadsDepToPhix.size(); i++) {
										if (AllLoadsDepToPhix[i] == s) {
											DepPhiOfDepLoad = true;
											indexOfDepLoad = i;
										}
									}
									if (DepPhiOfDepLoad) {
										bool foundall = false;
										for (auto& d : AllDependentInstsx[indexOfDepLoad]) {
											for (auto& sd : DependentInstrsToCurLoadx) {
												if (d == sd) {
													foundall = true;
												}
											}
											if (!foundall) {
												continue;
											}
										}
										if (foundall) {
											SmallVector<Instruction*, 20> DependentInstrsToIndirectLoadx;
											SmallVector<Instruction*, 20> DependentInstrsToStrideLoadx;
											SmallVector<Instruction*, 10> DependentPhistoIndirectLoadx;
											SmallVector<Instruction*, 10> DependentPhistoStrideLoadx;

											IndirectLoadsx.push_back(curLoad);
											StrideLoadsx.push_back(s);
											for (auto& si : DependentInstrsToCurLoadx) {
												DependentInstrsToIndirectLoadx.push_back(si);
											}
											for (auto& di : AllDependentInstsx[indexOfDepLoad]) {
												DependentInstrsToStrideLoadx.push_back(di);
											}
											for (auto& si : DependentPhisx) {
												DependentPhistoIndirectLoadx.push_back(si);
											}
											for (auto& di : AllDependentPhisx[indexOfDepLoad]) {
												DependentPhistoStrideLoadx.push_back(di);
											}
											AllDependentInstrsToIndirectLoadx.push_back(DependentInstrsToIndirectLoadx);
											AllDependentInstrsToStrideLoadx.push_back(DependentInstrsToStrideLoadx);
											AllDependentPhisToIndirectLoadx.push_back(DependentPhistoIndirectLoadx);
											AllDependentPhisToStrideLoadx.push_back(DependentPhistoStrideLoadx);
										}
										DepPhiOfDepLoad = false;
									}
								}
							} // if(DependentLoadsToCurLoad.size()
							AllLoadsDepToPhix.push_back(curLoad);
							AllDependentInstsx.push_back(DependentInstrsToCurLoadx);
							AllDependentPhisx.push_back(DependentPhisx);
						} // if(IsCurLoadDependentToPhiNode
					}	  // if load
				}
			}
		} // for(auto &BB : F)

		for (long unsigned int x = 0; x < StrideLoadsx.size(); x++) {
			for (long unsigned int y = 0; y < IndirectLoadsx.size(); y++) {
				if (StrideLoadsx[x] == IndirectLoadsx[y]) {
					if (AllDependentPhisToStrideLoadx[x] == AllDependentPhisToIndirectLoadx[y]) {
						LoadsToRemovex.push_back(StrideLoadsx[x]);
					}
				}
			}
		}

		for (long unsigned int x = 0; x < StrideLoadsx.size(); x++) {
			bool kept = false;
			if (LoadsToRemovex.size() > 0) {
				for (long unsigned int y = 0; y < LoadsToRemovex.size(); y++) {
					if (StrideLoadsx[x] != LoadsToRemovex[y] && IndirectLoadsx[x] != LoadsToRemovex[y]) {
						kept = true;
					}
				}
				if (kept) {
					StrideLoadsToKeepx.push_back(StrideLoadsx[x]);
					LoadsIndexx.push_back(x);
					IndirectLoadsToKeepx.push_back(IndirectLoadsx[x]);
				}
			} else {
				StrideLoadsToKeepx.push_back(StrideLoadsx[x]);
				LoadsIndexx.push_back(x);
				IndirectLoadsToKeepx.push_back(IndirectLoadsx[x]);
			}
		}
		for (long unsigned int x = 0; x < IndirectLoadsToKeepx.size(); x++) {
			if (InjectPrefechesOnePhiPartTwo(IndirectLoadsToKeepx[x], LI, AllDependentPhisToIndirectLoadx[LoadsIndexx[x]][0], AllDependentInstrsToIndirectLoadx[LoadsIndexx[x]], pd)) {
				modified = true;
			}
			if (InjectPrefechesOnePhiPartTwo(StrideLoadsToKeepx[x], LI, AllDependentPhisToStrideLoadx[LoadsIndexx[x]][0], AllDependentInstrsToStrideLoadx[LoadsIndexx[x]], pd * 2)) {
				modified = true;
			}
		}
	} // if(!AutoFDOMapping)
	  
	return modified;
} // runOnFunction


static RegisterPass<LLCHintLLVMPass> X("LLCHintLLVMPass", "Hello LLCHintLLVMPass", true, true);
