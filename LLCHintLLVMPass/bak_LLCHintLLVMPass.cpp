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

using namespace llvm;
using namespace sampleprof;

bool AutoFDOMapping;

static cl::opt<std::string> PrefetchFile("input-file", cl::desc("Specify input filename for mypass"), cl::value_desc("filename"));

cl::list<std::string> LBR_dist("dist", cl::desc("Specify offset value from LBR"), cl::OneOrMore);

SmallVector<Instruction*, 10> IndirectLoads;
SmallVector<Instruction*, 20> IndirectInstrs;
SmallVector<Instruction*, 10> IndirectPhis;
Instruction* IndirectLoad;
int64_t IndirectPrefetchDist;

namespace {
struct LLCHintLLVMPass : public FunctionPass {
  bool doInitialization(Module& M) override;
  bool runOnFunction(Function& F) override;
  void getAnalysisUsage(AnalysisUsage& AU) const override
  {
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addPreserved<ScalarEvolutionWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
  }

  public:
  static char ID;
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

bool LLCHintLLVMPass::runOnFunction(Function& F)
{
  bool modified = false;
  LoopInfo& LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  if (!Reader) {
    return false;
  }
  bool samplesExist = false;
  const llvm::sampleprof::FunctionSamples* SamplesReaded = Reader->getSamplesFor(F);
  if (SamplesReaded) {
    samplesExist = true;
  }
  return modified;
} // runOnFunction

static RegisterPass<LLCHintLLVMPass> X("LLCHintLLVMPass", "Hello LLCHintLLVMPass", true, true);
