//===- TapirRaceDetect.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// TapirRaceDetect is an LLVM pass that analyses Tapir tasks and dependences
// between memory accesses to find accesses that might race.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/TapirRaceDetect.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TapirTaskInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SpecialCaseList.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace llvm;

#define DEBUG_TYPE "tapir-race-detect"

static cl::opt<bool>
    AssumeSafeMalloc(
        "assume-safe-malloc", cl::init(true), cl::Hidden,
        cl::desc("Assume that calls to allocation functions are safe."));

static cl::opt<bool>
    IgnoreTerminationCalls(
        "ignore-termination-calls", cl::init(true), cl::Hidden,
        cl::desc("Ignore calls in program-terminating exit blocks."));

static cl::opt<unsigned>
    MaxUsesToExploreCapture(
        "max-uses-to-explore-capture", cl::init(unsigned(-1)), cl::Hidden,
        cl::desc("Maximum number of uses to explore for a capture query."));

static cl::list<std::string> ClABIListFiles(
    "strat-ignorelist",
    cl::desc("File listing native ABI functions and how the pass treats them"),
    cl::Hidden);

// Boilerplate for legacy and new pass managers

TapirRaceDetect::Result
TapirRaceDetect::run(Function &F, FunctionAnalysisManager &FAM) {
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &TI = FAM.getResult<TaskAnalysis>(F);
  auto &DI = FAM.getResult<DependenceAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto *TLI = &FAM.getResult<TargetLibraryAnalysis>(F);
  return RaceInfo(&F, DT, LI, TI, DI, SE, TLI);
}

AnalysisKey TapirRaceDetect::Key;

INITIALIZE_PASS_BEGIN(TapirRaceDetectWrapperPass, "tapir-race-detect",
                      "Tapir Race Detection", true, true)
INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TaskInfoWrapperPass)
INITIALIZE_PASS_END(TapirRaceDetectWrapperPass, "tapir-race-detect",
                    "Tapir Race Detection", true, true)

char TapirRaceDetectWrapperPass::ID = 0;

TapirRaceDetectWrapperPass::TapirRaceDetectWrapperPass() : FunctionPass(ID) {
  initializeTapirRaceDetectWrapperPassPass(*PassRegistry::getPassRegistry());
}

bool TapirRaceDetectWrapperPass::runOnFunction(Function &F) {
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &TI = getAnalysis<TaskInfoWrapperPass>().getTaskInfo();
  auto &DI = getAnalysis<DependenceAnalysisWrapperPass>().getDI();
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto *TLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  Info.reset(new RaceInfo(&F, DT, LI, TI, DI, SE, TLI));
  return false;
}

RaceInfo &TapirRaceDetectWrapperPass::getRaceInfo() const { return *Info; }

void TapirRaceDetectWrapperPass::releaseMemory() { Info.reset(); }

void TapirRaceDetectWrapperPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<DependenceAnalysisWrapperPass>();
  AU.addRequiredTransitive<DominatorTreeWrapperPass>();
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<ScalarEvolutionWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequiredTransitive<TaskInfoWrapperPass>();
}

FunctionPass *llvm::createTapirRaceDetectWrapperPass() {
  return new TapirRaceDetectWrapperPass();
}

void TapirRaceDetectWrapperPass::print(raw_ostream &OS,
                                       const Module *) const {
  Info->print(OS);
}

PreservedAnalyses
TapirRaceDetectPrinterPass::run(Function &F, FunctionAnalysisManager &FAM) {
  OS << "'Tapir race detection' for function '" << F.getName() << "':\n";
  FAM.getResult<TapirRaceDetect>(F).print(OS);
  return PreservedAnalyses::all();
}

bool RaceInfo::invalidate(Function &F, const PreservedAnalyses &PA,
                          FunctionAnalysisManager::Invalidator &Inv) {
  // Check whether the analysis, all analyses on functions, or the function's
  // CFG have been preserved.
  auto PAC = PA.getChecker<TapirRaceDetect>();
  return !(PAC.preserved() || PAC.preservedSet<AllAnalysesOn<Function>>() ||
           Inv.invalidate<DominatorTreeAnalysis>(F, PA) ||
           Inv.invalidate<LoopAnalysis>(F, PA) ||
           Inv.invalidate<TaskAnalysis>(F, PA) ||
           Inv.invalidate<DependenceAnalysis>(F, PA) ||
           Inv.invalidate<ScalarEvolutionAnalysis>(F, PA) ||
           Inv.invalidate<TargetLibraryAnalysis>(F, PA));
}

// Copied from DataFlowSanitizer.cpp
static StringRef GetGlobalTypeString(const GlobalValue &G) {
  // Types of GlobalVariables are always pointer types.
  Type *GType = G.getValueType();
  // For now we support ignoring struct types only.
  if (StructType *SGType = dyn_cast<StructType>(GType)) {
    if (!SGType->isLiteral())
      return SGType->getName();
  }
  return "<unknown type>";
}

namespace {

// Copied and adapted from DataFlowSanitizer.cpp
class StratABIList {
  std::unique_ptr<SpecialCaseList> SCL;

 public:
  StratABIList() = default;

  void set(std::unique_ptr<SpecialCaseList> List) { SCL = std::move(List); }

  /// Returns whether either this function or its source file are listed in the
  /// given category.
  bool isIn(const Function &F, StringRef Category = StringRef()) const {
    return isIn(*F.getParent(), Category) ||
           SCL->inSection("cilk", "fun", F.getName(), Category);
  }

  /// Returns whether this type is listed in the given category.
  bool isIn(const Type &Ty, StringRef Category = StringRef()) const {
    const Type *ElTy = &Ty;
    // We only handle struct types right now.
    if (const StructType *STy = dyn_cast<StructType>(ElTy))
      if (STy->hasName())
        return SCL->inSection("cilk", "type", STy->getName(), Category);
    return false;
  }

  bool isIn(const GlobalVariable &GV, StringRef Category = StringRef()) const {
    return isIn(*GV.getParent(), Category) ||
        SCL->inSection("cilk", "global", GV.getName(), Category);
  }

  /// Returns whether this global alias is listed in the given category.
  ///
  /// If GA aliases a function, the alias's name is matched as a function name
  /// would be.  Similarly, aliases of globals are matched like globals.
  bool isIn(const GlobalAlias &GA, StringRef Category = StringRef()) const {
    if (isIn(*GA.getParent(), Category))
      return true;

    if (isa<FunctionType>(GA.getValueType()))
      return SCL->inSection("cilk", "fun", GA.getName(), Category);

    return SCL->inSection("cilk", "global", GA.getName(), Category) ||
           SCL->inSection("cilk", "type", GetGlobalTypeString(GA),
                          Category);
  }

  /// Returns whether this module is listed in the given category.
  bool isIn(const Module &M, StringRef Category = StringRef()) const {
    return SCL->inSection("cilk", "src", M.getModuleIdentifier(), Category);
  }
};

// Structure to record the set of child tasks that might be in parallel with
// this spindle, ignoring back edges of loops.
//
// TODO: Improve this analysis to track the loop back edges responsible for
// specific maybe-parallel tasks.  Use these back-edge tags to refine the
// dependence-analysis component of static race detection.  Possible test case:
// intel/BlackScholes.
struct MaybeParallelTasksInLoopBody : public MaybeParallelTasks {
  MPTaskListTy TaskList;
  LoopInfo &LI;

  MaybeParallelTasksInLoopBody(LoopInfo &LI) : LI(LI) {}

  // This method performs the data-flow update computation on a given spindle.
  bool evaluate(const Spindle *S, unsigned EvalNum) {
    LLVM_DEBUG(dbgs() << "MPTInLoop::evaluate @ " << S->getEntry()->getName()
                      << "\n");
    if (!TaskList.count(S))
      TaskList.try_emplace(S);

    bool Complete = true;
    for (const Spindle::SpindleEdge &PredEdge : S->in_edges()) {
      const Spindle *Pred = PredEdge.first;
      const BasicBlock *Inc = PredEdge.second;

      // If the incoming edge is a sync edge, get the associated sync region.
      const Value *SyncRegSynced = nullptr;
      if (const SyncInst *SI = dyn_cast<SyncInst>(Inc->getTerminator()))
        SyncRegSynced = SI->getSyncRegion();

      // Skip back edges for this task list.
      if (Loop *L = LI.getLoopFor(S->getEntry()))
        if ((L->getHeader() == S->getEntry()) && L->contains(Inc))
          continue;

      // Iterate through the tasks in the task list for Pred.
      for (const Task *MP : TaskList[Pred]) {
        // Filter out any tasks that are synced by the sync region.
        if (const DetachInst *DI = MP->getDetach())
          if (SyncRegSynced == DI->getSyncRegion())
            continue;
        // Insert the task into this spindle's task list.  If this task is a new
        // addition, then we haven't yet reached the fixed point of this
        // analysis.
        if (TaskList[S].insert(MP).second)
          Complete = false;
      }
    }
    LLVM_DEBUG({
      dbgs() << "  New MPT list for " << S->getEntry()->getName()
             << (Complete ? " (complete)\n" : " (not complete)\n");
      for (const Task *MP : TaskList[S])
        dbgs() << "    " << MP->getEntry()->getName() << "\n";
    });
    return Complete;
  }
};

class AccessPtrAnalysis {
public:
  /// Read or write access location.
  // using MemAccessInfo = PointerIntPair<const Value *, 1, bool>;
  using MemAccessInfo = RaceInfo::MemAccessInfo;
  // using MemAccessInfoList = SmallVector<MemAccessInfo, 8>;
  // using AccessToUnderlyingObjMap =
  //   DenseMap<MemAccessInfo, SmallPtrSet<Value *, 1>>;
  using AccessToUnderlyingObjMap = RaceInfo::AccessToUnderlyingObjMap;

  AccessPtrAnalysis(DominatorTree &DT, TaskInfo &TI, LoopInfo &LI,
                    DependenceInfo &DI, ScalarEvolution &SE,
                    const TargetLibraryInfo *TLI,
                    AccessToUnderlyingObjMap &AccessToObjs)
      : DT(DT), TI(TI), LI(LI), DI(DI), AA(DI.getAA()), SE(SE), TLI(TLI),
        AccessToObjs(AccessToObjs), MPTasksInLoop(LI) {
    TI.evaluateParallelState<MaybeParallelTasks>(MPTasks);

    std::vector<std::string> AllABIListFiles;
    AllABIListFiles.insert(AllABIListFiles.end(), ClABIListFiles.begin(),
                           ClABIListFiles.end());
    ABIList.set(SpecialCaseList::createOrDie(AllABIListFiles,
                                             *vfs::getRealFileSystem()));
  }

  void addFunctionArgument(Value *Arg);
  void addAccess(Instruction *I);

  void processAccessPtrs(RaceInfo::ResultTy &Result,
                         RaceInfo::ObjectMRTy &ObjectMRForRace,
                         RaceInfo::PtrChecksTy &AllPtrRtChecks);

private:
  using PtrAccessSet = SetVector<MemAccessInfo>;

  void checkForRacesHelper(const Task *T, RaceInfo::ResultTy &Result,
                           RaceInfo::ObjectMRTy &ObjectMRForRace);
  bool checkOpaqueAccesses(GeneralAccess &GA1, GeneralAccess &GA2);
  void evaluateMaybeParallelAccesses(GeneralAccess &GA1, GeneralAccess &GA2,
                                     RaceInfo::ResultTy &Result,
                                     RaceInfo::ObjectMRTy &ObjectMRForRace);
  bool checkDependence(std::unique_ptr<Dependence> D, GeneralAccess &GA1,
                       GeneralAccess &GA2);

  bool PointerCapturedBefore(const Value *Ptr, const Instruction *I,
                             unsigned MaxUsesToExplore) const;

  AliasResult underlyingObjectsAlias(const GeneralAccess &GAA,
                                     const GeneralAccess &GAB);

  void recordLocalRace(const GeneralAccess &GA, RaceInfo::ResultTy &Result,
                       RaceInfo::ObjectMRTy &ObjectMRForRace,
                       const GeneralAccess &Competitor);
  DominatorTree &DT;
  TaskInfo &TI;
  LoopInfo &LI;
  DependenceInfo &DI;
  AliasAnalysis *AA;
  ScalarEvolution &SE;

  const TargetLibraryInfo *TLI;
  SmallPtrSet<Value *, 4> ArgumentPtrs;
  AccessToUnderlyingObjMap &AccessToObjs;

  MaybeParallelTasks MPTasks;
  MaybeParallelTasksInLoopBody MPTasksInLoop;

  // A mapping of tasks to instructions in that task that might participate in a
  // determinacy race.
  using TaskAccessMapTy = DenseMap<const Task *, SmallVector<GeneralAccess, 4>>;
  TaskAccessMapTy TaskAccessMap;

  // A mapping of spindles to instructions in that spindle that might
  // participate in a determinacy race.
  using SpindleAccessMapTy =
    DenseMap<const Spindle *, SmallVector<GeneralAccess, 4>>;
  SpindleAccessMapTy SpindleAccessMap;

  // A mapping of loops to instructions in that loop that might
  // participate in a determinacy race.
  using LoopAccessMapTy = DenseMap<const Loop *, SmallVector<GeneralAccess, 4>>;
  LoopAccessMapTy LoopAccessMap;

  mutable DenseMap<std::pair<const Value *, const Instruction *>, bool>
  MayBeCapturedCache;

  // /// We need to check that all of the pointers in this list are disjoint
  // /// at runtime. Using std::unique_ptr to make using move ctor simpler.
  // DenseMap<const Loop *, RuntimePointerChecking *> AllPtrRtChecking;

  // ABI list to ignore.
  StratABIList ABIList;
};

} // end anonymous namespace

static bool isFreeFn(const Instruction *I, const TargetLibraryInfo *TLI) {
  if (!isa<CallBase>(I))
    return false;
  const CallBase *CB = dyn_cast<CallBase>(I);

  if (!TLI)
    return false;

  if (getFreedOperand(CB, TLI))
    return true;

  // Ideally we would just use getFreedOperand to determine whether I is a call
  // to a libfree funtion.  But if -fno-builtin is used, then getFreedOperand
  // won't recognize any libfree functions.  For instrumentation purposes,
  // it's sufficient to recognize the function name.
  const StringRef FreeFnNames[] = {
    "_ZdlPv",
    "_ZdaPv",
    "_ZdlPvj",
    "_ZdlPvm",
    "_ZdlPvRKSt9nothrow_t",
    "_ZdlPvSt11align_val_t",
    "_ZdaPvj",
    "_ZdaPvm",
    "_ZdaPvRKSt9nothrow_t",
    "_ZdaPvSt11align_val_t",
    "_ZdlPvSt11align_val_tRKSt9nothrow_t",
    "_ZdaPvSt11align_val_tRKSt9nothrow_t",
    "_ZdlPvjSt11align_val_t",
    "_ZdlPvmSt11align_val_t",
    "_ZdaPvjSt11align_val_t",
    "_ZdaPvmSt11align_val_t",
    "??3@YAXPAX@Z",
    "??3@YAXPAXABUnothrow_t@std@@@Z",
    "??3@YAXPAXI@Z",
    "??3@YAXPEAX@Z",
    "??3@YAXPEAXAEBUnothrow_t@std@@@Z",
    "??3@YAXPEAX_K@Z",
    "??_V@YAXPAX@Z",
    "??_V@YAXPAXABUnothrow_t@std@@@Z",
    "??_V@YAXPAXI@Z",
    "??_V@YAXPEAX@Z",
    "??_V@YAXPEAXAEBUnothrow_t@std@@@Z",
    "??_V@YAXPEAX_K@Z",
    "__kmpc_free_shared"
  };

  if (const Function *Called = CB->getCalledFunction()) {
    StringRef FnName = Called->getName();
    if (!llvm::any_of(FreeFnNames, [&](const StringRef FreeFnName) {
          return FnName == FreeFnName;
        }))
      return false;

    // Confirm that this function is a recognized library function
    LibFunc F;
    bool FoundLibFunc = TLI->getLibFunc(*Called, F);
    return FoundLibFunc;
  }

  return false;
}

static bool isAllocFn(const Instruction *I, const TargetLibraryInfo *TLI) {
  if (!isa<CallBase>(I))
    return false;

  if (!TLI)
    return false;

  if (isAllocationFn(I, TLI))
    return true;

  // Ideally we would just use isAllocationFn to determine whether I is a call
  // to an allocation funtion.  But if -fno-builtin is used, then isAllocationFn
  // won't recognize any allocation functions.  For instrumentation purposes,
  // it's sufficient to recognize the function name.
  const StringRef AllocFnNames[] = {
    "_Znwj",
    "_ZnwjRKSt9nothrow_t",
    "_ZnwjSt11align_val_t",
    "_ZnwjSt11align_val_tRKSt9nothrow_t",
    "_Znwm",
    "_ZnwmRKSt9nothrow_t",
    "_ZnwmSt11align_val_t",
    "_ZnwmSt11align_val_tRKSt9nothrow_t",
    "_Znaj",
    "_ZnajRKSt9nothrow_t",
    "_ZnajSt11align_val_t",
    "_ZnajSt11align_val_tRKSt9nothrow_t",
    "_Znam",
    "_ZnamRKSt9nothrow_t",
    "_ZnamSt11align_val_t",
    "_ZnamSt11align_val_tRKSt9nothrow_t",
    "??2@YAPAXI@Z",
    "??2@YAPAXIABUnothrow_t@std@@@Z",
    "??2@YAPEAX_K@Z",
    "??2@YAPEAX_KAEBUnothrow_t@std@@@Z",
    "??_U@YAPAXI@Z",
    "??_U@YAPAXIABUnothrow_t@std@@@Z",
    "??_U@YAPEAX_K@Z",
    "??_U@YAPEAX_KAEBUnothrow_t@std@@@Z",
    "strdup",
    "dunder_strdup",
    "strndup",
    "dunder_strndup",
    "__kmpc_alloc_shared",
    "posix_memalign"
  };

  if (const Function *Called = dyn_cast<CallBase>(I)->getCalledFunction()) {
    StringRef FnName = Called->getName();
    if (!llvm::any_of(AllocFnNames, [&](const StringRef AllocFnName) {
          return FnName == AllocFnName;
        }))
      return false;

    // Confirm that this function is a recognized library function
    LibFunc F;
    bool FoundLibFunc = TLI->getLibFunc(*Called, F);
    return FoundLibFunc;
  }

  return false;
}

static bool isAllocFn(const Value *V, const TargetLibraryInfo *TLI) {
  if (const CallBase *CB = dyn_cast<CallBase>(V))
    return isAllocFn(CB, TLI);
  return false;
}

static bool isReallocFn(const CallBase *Call) {
  return (static_cast<AllocFnKind>(
              Call->getFnAttr(Attribute::AllocKind).getValueAsInt()) &
          AllocFnKind::Realloc) != AllocFnKind::Unknown;
}

static bool checkInstructionForRace(const Instruction *I,
                                    const TargetLibraryInfo *TLI) {
  if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<VAArgInst>(I) ||
      isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I) ||
      isa<AnyMemSetInst>(I) || isa<AnyMemTransferInst>(I))
    return true;

  if (const CallBase *Call = dyn_cast<CallBase>(I)) {
    // Ignore debug info intrinsics
    if (isa<DbgInfoIntrinsic>(I))
      return false;

    if (const Function *Called = Call->getCalledFunction()) {
      // Check for detached.rethrow, taskframe.resume, or sync.unwind, which
      // might be invoked.
      if (Intrinsic::detached_rethrow == Called->getIntrinsicID() ||
          Intrinsic::taskframe_resume == Called->getIntrinsicID() ||
          Intrinsic::sync_unwind == Called->getIntrinsicID())
        return false;

      // Ignore CSI and Cilksan functions
      if (Called->hasName() && (Called->getName().starts_with("__csi") ||
                                Called->getName().starts_with("__csan") ||
                                Called->getName().starts_with("__cilksan")))
        return false;
    }

    // Ignore other intrinsics.
    if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(I)) {
      // Ignore intrinsics that do not access memory.
      if (II->doesNotAccessMemory())
        return false;
      // TODO: Exclude all intrinsics for which
      // TTI::getIntrinsicCost() == TCC_Free?
      switch (II->getIntrinsicID()) {
      default: return true;
      case Intrinsic::annotation:
      case Intrinsic::assume:
      case Intrinsic::invariant_start:
      case Intrinsic::invariant_end:
      case Intrinsic::launder_invariant_group:
      case Intrinsic::strip_invariant_group:
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
      case Intrinsic::ptr_annotation:
      case Intrinsic::var_annotation:
      case Intrinsic::experimental_noalias_scope_decl:
      case Intrinsic::syncregion_start:
      case Intrinsic::taskframe_create:
      case Intrinsic::taskframe_use:
      case Intrinsic::taskframe_end:
      case Intrinsic::taskframe_load_guard:
      case Intrinsic::sync_unwind:
        return false;
      }
    }

    // We can assume allocation functions are safe.
    if (AssumeSafeMalloc && isAllocFn(Call, TLI)) {
      return isReallocFn(Call);
    }

    // If this call occurs in a termination block of the program, ignore it.
    if (IgnoreTerminationCalls &&
        isa<UnreachableInst>(I->getParent()->getTerminator())) {
      const Function *CF = Call->getCalledFunction();
      // If this function call is indirect, we want to instrument it.
      if (!CF)
        return true;
      // If this is an ordinary function call in a terminating block, ignore it.
      if (!CF->hasFnAttribute(Attribute::NoReturn))
        return false;
      // If this is a call to a terminating function, such as "exit" or "abort",
      // ignore it.
      if (CF->hasName() &&
          ((CF->getName() == "exit") || (CF->getName() == "abort") ||
           (CF->getName() == "__clang_call_terminate") ||
           (CF->getName() == "__assert_fail")))
        return false;
    }

    // We want to instrument calls in general.
    return true;
  }
  return false;
}

// Get the general memory accesses for the instruction \p I, and stores those
// accesses into \p AccI.  Returns true if general memory accesses could be
// derived for I, false otherwise.
static void GetGeneralAccesses(
    Instruction *I, SmallVectorImpl<GeneralAccess> &AccI, AliasAnalysis *AA,
    const TargetLibraryInfo *TLI) {
  // Handle common memory instructions
  if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    MemoryLocation Loc = MemoryLocation::get(LI);
    if (!AA->pointsToConstantMemory(Loc))
      AccI.push_back(GeneralAccess(LI, Loc, ModRefInfo::Ref));
    return;
  }
  if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    AccI.push_back(GeneralAccess(SI, MemoryLocation::get(SI), ModRefInfo::Mod));
    return;
  }
  // Handle atomic instructions
  if (AtomicCmpXchgInst *CXI = dyn_cast<AtomicCmpXchgInst>(I)) {
    AccI.push_back(GeneralAccess(CXI, MemoryLocation::get(CXI),
                                 ModRefInfo::Mod));
    return;
  }
  if (AtomicRMWInst *RMWI = dyn_cast<AtomicRMWInst>(I)) {
    AccI.push_back(GeneralAccess(RMWI, MemoryLocation::get(RMWI),
                                 ModRefInfo::Mod));
    return;
  }

  // Handle VAArgs.
  if (VAArgInst *VAAI = dyn_cast<VAArgInst>(I)) {
    MemoryLocation Loc = MemoryLocation::get(VAAI);
    if (!AA->pointsToConstantMemory(Loc))
      AccI.push_back(GeneralAccess(VAAI, Loc, ModRefInfo::ModRef));
    return;
  }

  // Handle memory intrinsics.
  if (AnyMemSetInst *MSI = dyn_cast<AnyMemSetInst>(I)) {
    AccI.push_back(GeneralAccess(MSI, MemoryLocation::getForDest(MSI),
                                 ModRefInfo::Mod));
    return;
  }
  if (AnyMemTransferInst *MTI = dyn_cast<AnyMemTransferInst>(I)) {
    AccI.push_back(GeneralAccess(MTI, MemoryLocation::getForDest(MTI),
                                 0, ModRefInfo::Mod));
    MemoryLocation Loc = MemoryLocation::getForSource(MTI);
    if (!AA->pointsToConstantMemory(Loc))
      AccI.push_back(GeneralAccess(MTI, Loc, 1, ModRefInfo::Ref));
    return;
  }

  // Handle arbitrary call sites by examining pointee arguments.
  //
  // This logic is based on that in AliasSetTracker.cpp.
  if (const CallBase *Call = dyn_cast<CallBase>(I)) {
    ModRefInfo CallMask = AA->getMemoryEffects(Call).getModRef();

    // Some intrinsics are marked as modifying memory for control flow modelling
    // purposes, but don't actually modify any specific memory location.
    using namespace PatternMatch;
    if (Call->use_empty() &&
        match(Call, m_Intrinsic<Intrinsic::invariant_start>()))
      CallMask &= ModRefInfo::Ref;
    // TODO: See if we need to exclude additional intrinsics.

    if (isAllocFn(Call, TLI)) {
      // Handle realloc as a special case.
      if (isReallocFn(Call)) {
        // TODO: Try to get the size of the object being copied from.
        AccI.push_back(GeneralAccess(I, MemoryLocation::getForArgument(
                                         Call, 0, TLI), 0,
                                     AA->getArgModRefInfo(Call, 0)));
        // If we assume malloc is safe, don't worry about opaque accesses by
        // realloc.
        if (!AssumeSafeMalloc)
          AccI.push_back(GeneralAccess(I, std::nullopt, CallMask));
        return;
      }
    }

    for (auto IdxArgPair : enumerate(Call->args())) {
      int ArgIdx = IdxArgPair.index();
      const Value *Arg = IdxArgPair.value();
      if (!Arg->getType()->isPointerTy())
        continue;
      MemoryLocation ArgLoc =
        MemoryLocation::getForArgument(Call, ArgIdx, TLI);
      if (AA->pointsToConstantMemory(ArgLoc))
        continue;
      ModRefInfo ArgMask = AA->getArgModRefInfo(Call, ArgIdx);
      ArgMask &= CallMask;
      if (!isNoModRef(ArgMask)) {
        AccI.push_back(GeneralAccess(I, ArgLoc, ArgIdx, ArgMask));
      }
    }

    // If we find a free call and we assume malloc is safe, don't worry about
    // opaque accesses by that free call.
    if (AssumeSafeMalloc && getFreedOperand(Call, TLI))
      return;

    if (!Call->onlyAccessesArgMemory())
      // Add a generic GeneralAccess for this call to represent the fact that it
      // might access arbitrary global memory.
      AccI.push_back(GeneralAccess(I, std::nullopt, CallMask));
    return;
  }
}

void AccessPtrAnalysis::addFunctionArgument(Value *Arg) {
  ArgumentPtrs.insert(Arg);
}

void AccessPtrAnalysis::addAccess(Instruction *I) {
  if (checkInstructionForRace(I, TLI)) {

    // Exclude calls to functions in ABIList.
    if (const CallBase *Call = dyn_cast<CallBase>(I)) {
      if (const Function *CF = Call->getCalledFunction())
        if (ABIList.isIn(*CF))
          return;
    } else {
      MemoryLocation Loc = MemoryLocation::get(I);
      if (Loc.Ptr) {
        if (const Value *UnderlyingObj = getUnderlyingObject(Loc.Ptr, 0)) {
          if (const GlobalVariable *GV =
              dyn_cast<GlobalVariable>(UnderlyingObj))
            if (ABIList.isIn(*GV))
              return;
          if (ABIList.isIn(*UnderlyingObj->getType()))
            return;
        }
      }
    }

    SmallVector<GeneralAccess, 1> GA;
    GetGeneralAccesses(I, GA, DI.getAA(), TLI);
    TaskAccessMap[TI.getTaskFor(I->getParent())].append(GA.begin(), GA.end());
    SpindleAccessMap[TI.getSpindleFor(I->getParent())].append(GA.begin(),
                                                              GA.end());
    if (Loop *L = LI.getLoopFor(I->getParent()))
      LoopAccessMap[L].append(GA.begin(), GA.end());

    for (GeneralAccess Acc : GA) {
      // Skip this access if it does not have a valid pointer.
      if (!Acc.getPtr())
        continue;

      MemAccessInfo Access(Acc.getPtr(), Acc.isMod());
      // DepCands.insert(Access);

      SmallVector<const Value *, 1> Objects;
      LLVM_DEBUG(dbgs() << "Getting underlying objects for " << *Acc.getPtr()
                        << "\n");
      getUnderlyingObjects(const_cast<Value *>(Acc.getPtr()), Objects, &LI, 0);
      for (const Value *Obj : Objects) {
        LLVM_DEBUG(dbgs() << "  Considering object: " << *Obj << "\n");
        // nullptr never alias, don't join sets for pointer that have "null" in
        // their UnderlyingObjects list.
        if (isa<ConstantPointerNull>(Obj) &&
            !NullPointerIsDefined(I->getFunction(),
                                  Obj->getType()->getPointerAddressSpace()))
          continue;

        // Is this value a constant that cannot be derived from any pointer
        // value (we need to exclude constant expressions, for example, that
        // are formed from arithmetic on global symbols).
        if (const Constant *C = dyn_cast<Constant>(Obj)) {
          // This check is derived from Transforms/Utils/InlineFunction.cpp
          bool IsNonPtrConst = isa<BlockAddress>(C) || isa<ConstantInt>(C) ||
            isa<ConstantFP>(C) || isa<ConstantPointerNull>(C) ||
            isa<ConstantDataSequential>(C) || isa<UndefValue>(C) ||
            isa<ConstantTokenNone>(C);
          if (IsNonPtrConst)
            continue;
        }

        if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(Obj))
          // Constant variables cannot race.
          if (GV->isConstant())
            continue;

        if (isa<Function>(Obj))
          // Assume that functions are read-only
          continue;

        LLVM_DEBUG(dbgs() << "Adding object for access:\n  Obj: " << *Obj
                          << "\n  Access: " << *Acc.getPtr() << "\n");
        AccessToObjs[Access].insert(Obj);

        // UnderlyingObjToAccessMap::iterator Prev = ObjToLastAccess.find(Obj);
        // if (Prev != ObjToLastAccess.end())
        //   DepCands.unionSets(Access, Prev->second);

        // ObjToLastAccess[Obj] = Access;
      }
    }
  }
}

static const Loop *getCommonLoop(const BasicBlock *B1, const BasicBlock *B2,
                                 LoopInfo &LI) {
  unsigned B1Level = LI.getLoopDepth(B1);
  unsigned B2Level = LI.getLoopDepth(B2);
  const Loop *L1 = LI.getLoopFor(B1);
  const Loop *L2 = LI.getLoopFor(B2);
  while (B1Level > B2Level) {
    L1 = L1->getParentLoop();
    B1Level--;
  }
  while (B2Level > B1Level) {
    L2 = L2->getParentLoop();
    B2Level--;
  }
  while (L1 != L2) {
    L1 = L1->getParentLoop();
    L2 = L2->getParentLoop();
  }
  return L1;
}

static const Loop *getCommonLoop(const Loop *L, const BasicBlock *B,
                                 LoopInfo &LI) {
  unsigned L1Level = L->getLoopDepth();
  unsigned L2Level = LI.getLoopDepth(B);
  const Loop *L1 = L;
  const Loop *L2 = LI.getLoopFor(B);
  while (L1Level > L2Level) {
    L1 = L1->getParentLoop();
    L1Level--;
  }
  while (L2Level > L1Level) {
    L2 = L2->getParentLoop();
    L2Level--;
  }
  while (L1 != L2) {
    L1 = L1->getParentLoop();
    L2 = L2->getParentLoop();
  }
  return L1;
}

static const Spindle *GetRepSpindleInTask(const Spindle *S, const Task *T,
                                          const TaskInfo &TI) {
  const Task *Encl = T->getSubTaskEnclosing(S->getEntry());
  if (Encl->isRootTask())
    return S;
  return TI.getSpindleFor(Encl->getDetach()->getContinue());
}

bool AccessPtrAnalysis::checkDependence(std::unique_ptr<Dependence> D,
                                        GeneralAccess &GA1,
                                        GeneralAccess &GA2) {
  if (!D) {
    LLVM_DEBUG(dbgs() << "No dependence\n");
    return false;
  }

  LLVM_DEBUG({
    D->dump(dbgs());
    StringRef DepType = D->isFlow() ? "flow" : D->isAnti() ? "anti" : "output";
    dbgs() << "Found " << DepType << " dependency between Src and Dst\n";
    unsigned Levels = D->getLevels();
    for (unsigned II = 1; II <= Levels; ++II) {
      const SCEV *Distance = D->getDistance(II);
      if (Distance)
        dbgs() << "Level " << II << " distance " << *Distance << "\n";
    }
  });

  Instruction *I1 = GA1.I;
  Instruction *I2 = GA2.I;
  BasicBlock *B1 = I1->getParent();
  BasicBlock *B2 = I2->getParent();

  // Only dependencies that cross tasks can produce determinacy races.
  // Dependencies that cross loop iterations within the same task don't matter.

  // Find the deepest loop that contains both B1 and B2.
  const Loop *CommonLoop = getCommonLoop(B1, B2, LI);
  unsigned MaxLoopDepthToCheck = CommonLoop ? CommonLoop->getLoopDepth() : 0;

  // Check if dependence does not depend on looping.
  if (0 == MaxLoopDepthToCheck)
    // If there's no loop to worry about, then the existence of the dependence
    // implies the potential for a race.
    return true;

  // Use the base objects for the addresses to try to further refine the checks.

  // TODO: Use lifetime_begin intrinsics to further refine checks.
  const Loop *CommonObjLoop = CommonLoop;
  unsigned MinObjDepth = CommonLoop->getLoopDepth();
  SmallPtrSet<const Value *, 1> BaseObjs;
  MemAccessInfo MA1(GA1.getPtr(), GA1.isMod());
  MemAccessInfo MA2(GA2.getPtr(), GA2.isMod());
  for (const Value *Obj : AccessToObjs[MA1]) {
    if (AccessToObjs[MA2].count(Obj))
      BaseObjs.insert(Obj);
    else {
      MinObjDepth = 0;
      break;
    }
  }
  for (const Value *Obj : AccessToObjs[MA2]) {
    if (AccessToObjs[MA1].count(Obj))
      BaseObjs.insert(Obj);
    else {
      MinObjDepth = 0;
      break;
    }
  }

  // If we didn't find any base objects, we have no common-object loop.
  if (BaseObjs.empty())
    CommonObjLoop = nullptr;

  // Set MinObjDepth to 0 if there are not base objects to check.
  if (BaseObjs.empty() || !CommonObjLoop)
    MinObjDepth = 0;

  if (MinObjDepth != 0) {
    for (const Value *Obj : BaseObjs) {
      // If there are no more levels of common loop to check, return.
      if (!CommonObjLoop)
        break;

      LLVM_DEBUG(dbgs() << "Checking base object " << *Obj << "\n");
      assert(!(isa<ConstantPointerNull>(Obj) &&
               !NullPointerIsDefined(B1->getParent(),
                                     Obj->getType()->getPointerAddressSpace()))
             && "nullptr in list of base objects");

      // If the object is not an instruction, then there's no common loop to
      // find.
      if (!isa<Instruction>(Obj)) {
        CommonObjLoop = nullptr;
        break;
      }

      // This optimization of bounding the loop nest to check only applies if
      // the underlying objects perform an allocation.
      const Instruction *ObjI = dyn_cast<Instruction>(Obj);
      if (!isa<AllocaInst>(ObjI) && !isa<CallBase>(ObjI)) {
        CommonObjLoop = nullptr;
        break;
      }
      if (isa<AllocaInst>(ObjI))
        // Update the common loop for the underlying objects.
        CommonObjLoop = getCommonLoop(CommonObjLoop, ObjI->getParent(), LI);
      else if (const CallBase *CB = dyn_cast<CallBase>(ObjI)) {
        if (!CB->returnDoesNotAlias()) {
          CommonObjLoop = nullptr;
          break;
        }
        // Update the common loop for the underlying objects.
        CommonObjLoop = getCommonLoop(CommonObjLoop, ObjI->getParent(), LI);
      }
    }
  }
  // Save the depth of the common loop as the lower bound on the loop depth to
  // check.
  if (!CommonObjLoop) {
    LLVM_DEBUG(dbgs() << "No common loop found for underlying objects.\n");
    MinObjDepth = 0;
  } else
    MinObjDepth = CommonObjLoop->getLoopDepth();

  LLVM_DEBUG(dbgs() << "Min loop depth " << MinObjDepth <<
             " for underlying object.\n");

  LLVM_DEBUG({
      if (MinObjDepth > MaxLoopDepthToCheck) {
        dbgs() << "\tI1 " << *I1 << "\n\tI2 " << *I2;
        dbgs() << "\n\tPtr1 " << *GA1.getPtr()
               << " (null? " << (isa<ConstantPointerNull>(GA1.getPtr())) << ")";
        dbgs() << "\n\tPtr2 " << *GA2.getPtr()
               << " (null? " << (isa<ConstantPointerNull>(GA2.getPtr())) << ")";
        dbgs() << "\n\tAddrspace "
               << GA1.getPtr()->getType()->getPointerAddressSpace();
        dbgs() << "\n\tnullptr is defined? "
               << (NullPointerIsDefined(B1->getParent()));
        dbgs() << "\n\tMaxLoopDepthToCheck " << MaxLoopDepthToCheck;
        dbgs() << "\n\tMinObjDepthToCheck " << MinObjDepth << "\n";
      }
    });
  assert(MinObjDepth <= MaxLoopDepthToCheck &&
         "Minimum loop depth of underlying object cannot be greater "
         "than maximum loop depth of dependence.");

  // Get the task that encloses both B1 and B2.
  const Task *CommonTask = TI.getEnclosingTask(B1, B2);
  // Get the representative spindles for both B1 and B2 in this common task.
  const Spindle *I1Spindle = GetRepSpindleInTask(TI.getSpindleFor(B1),
                                                 CommonTask, TI);
  const Spindle *I2Spindle = GetRepSpindleInTask(TI.getSpindleFor(B2),
                                                 CommonTask, TI);
  // If this common loop does not contain the common task, then dependencies at
  // the level of this common loop do not constitute a potential race.  Find the
  // loop that contains the enclosing task.
  //
  // Skip this step if either representative spindle is a shared-eh spindle,
  // because those are more complicated.
  if (!I1Spindle->isSharedEH() && !I2Spindle->isSharedEH()) {
    if (!CommonLoop->contains(CommonTask->getEntry())) {
      const Loop *CommonTaskLoop = LI.getLoopFor(CommonTask->getEntry());
      // Typically, CommonTaskLoop is a subloop of CommonLoop.  But that doesn't
      // have to be true, e.g., if CommonLoop appears in an exit of
      // CommonTaskLoop.
      CommonLoop = CommonTaskLoop;
    }
    // Update MaxLoopDepthToCheck
    MaxLoopDepthToCheck = CommonLoop ? CommonLoop->getLoopDepth() : 0;

    // Check if dependence does not depend on looping.
    if (0 == MaxLoopDepthToCheck)
      MaxLoopDepthToCheck = MinObjDepth;
  }

  if (MaxLoopDepthToCheck == MinObjDepth) {
    LLVM_DEBUG(dbgs() << "Minimum object depth matches maximum loop depth.\n");
    if (TI.getTaskFor(B1) == TI.getTaskFor(B2))
      return false;

    // Check if dependence does not depend on looping.
    if (0 == MaxLoopDepthToCheck)
      // If there's no loop to worry about, then the existence of the dependence
      // implies the potential for a race.
      return true;

    if (!(D->getDirection(MaxLoopDepthToCheck) & Dependence::DVEntry::EQ))
      // Apparent dependence does not occur within the same iteration.
      return false;

    // Check if the instructions are parallel when the loop backedge is excluded
    // from dataflow.
    for (const Task *MPT : MPTasksInLoop.TaskList[I1Spindle])
      if (TI.encloses(MPT, B2))
        return true;
    for (const Task *MPT : MPTasksInLoop.TaskList[I2Spindle])
      if (TI.encloses(MPT, B1))
        return true;

    return false;
  }

  // Get the whole loop stack to check above the common loop.
  SmallVector<const Loop *, 4> LoopsToCheck;
  const Loop *CurrLoop = CommonLoop;
  while (CurrLoop) {
    LoopsToCheck.push_back(CurrLoop);
    CurrLoop = CurrLoop->getParentLoop();
  }

  // Check the loop stack from the top down until a loop is found where the
  // dependence might cross parallel tasks.
  unsigned MinLoopDepthToCheck = 1;
  while (!LoopsToCheck.empty()) {
    const Loop *CurrLoop = LoopsToCheck.pop_back_val();
    // If we're not yet at the minimum loop depth of the underlying object, go
    // deeper.
    if (MinLoopDepthToCheck < MinObjDepth) {
      ++MinLoopDepthToCheck;
      continue;
    }

    // Check the maybe-parallel tasks for the spindle containing the loop
    // header.
    const Spindle *CurrSpindle = TI.getSpindleFor(CurrLoop->getHeader());
    bool MPTEnclosesDst = false;
    for (const Task *MPT : MPTasks.TaskList[CurrSpindle]) {
      if (TI.encloses(MPT, B2)) {
        MPTEnclosesDst = true;
        break;
      }
    }

    // If Dst is found in a maybe-parallel task, then the minimum loop depth has
    // been found.
    if (MPTEnclosesDst)
      break;
    // Otherwise go deeper.
    ++MinLoopDepthToCheck;
  }

  // Scan the loop nests in common from inside out.
  for (unsigned II = MaxLoopDepthToCheck; II >= MinLoopDepthToCheck; --II) {
    LLVM_DEBUG(dbgs() << "Checking loop level " << II << "\n");
    if (D->isScalar(II))
      return true;
    if (D->getDirection(II) & unsigned(~Dependence::DVEntry::EQ))
      return true;
  }

  LLVM_DEBUG(dbgs() << "Dependence does not cross parallel tasks.\n");
  return false;
}

bool AccessPtrAnalysis::PointerCapturedBefore(const Value *Ptr,
                                              const Instruction *I,
                                              unsigned MaxUsesToExplore =
                                              MaxUsesToExploreCapture) const {
  const Value *StrippedPtr = Ptr->stripInBoundsOffsets();
  // Do not treat NULL pointers as captured.
  if (isa<ConstantPointerNull>(StrippedPtr))
    return false;
  auto CaptureQuery = std::make_pair(StrippedPtr, I);
  if (MayBeCapturedCache.count(CaptureQuery))
    return MayBeCapturedCache[CaptureQuery];

  bool Result = false;
  if (isa<GlobalValue>(StrippedPtr))
    // We assume that globals are captured.
    //
    // TODO: Possibly refine this check for private or internal globals.
    Result = true;
  else if (!isa<Instruction>(StrippedPtr)) {
    // If we could strip the pointer, we conservatively assume it may be
    // captured.
    LLVM_DEBUG(dbgs() << "PointerCapturedBefore: Could not fully strip pointer "
                      << *Ptr << "\n");
    Result = true;
  } else
    Result = PointerMayBeCapturedBefore(StrippedPtr, false, false, I, &DT, true,
                                        MaxUsesToExplore);
  MayBeCapturedCache[CaptureQuery] = Result;
  return Result;
}

bool AccessPtrAnalysis::checkOpaqueAccesses(GeneralAccess &GA1,
                                            GeneralAccess &GA2) {
  // If neither instruction may write to memory, then no race is possible.
  if (!GA1.I->mayWriteToMemory() && !GA2.I->mayWriteToMemory())
    return false;

  if (!GA1.Loc && !GA2.Loc) {
    LLVM_DEBUG({
        const CallBase *Call1 = cast<CallBase>(GA1.I);
        const CallBase *Call2 = cast<CallBase>(GA2.I);

        assert(!AA->doesNotAccessMemory(Call1) &&
               !AA->doesNotAccessMemory(Call2) &&
               "Opaque call does not access memory.");
        assert(!AA->getMemoryEffects(Call1).onlyAccessesArgPointees() &&
               !AA->getMemoryEffects(Call2).onlyAccessesArgPointees() &&
               "Opaque call only accesses arg pointees.");
      });
    // // If both calls only read memory, then there's no dependence.
    // if (AA->onlyReadsMemory(Call1) && AA->onlyReadsMemory(Call2))
    //   return false;

    // We have two logically-parallel calls that opaquely access memory, and at
    // least one call modifies memory.  Hence we have a dependnece and potential
    // race.
    return true;
  }

  BasicBlock *B1 = GA1.I->getParent();
  BasicBlock *B2 = GA2.I->getParent();

  // Get information about the non-opaque access.
  const Value *Ptr;
  Instruction *NonOpaque;
  if (GA1.Loc) {
    Ptr = GA1.getPtr();
    NonOpaque = GA1.I;
  } else { // GA2.Loc
    Ptr = GA2.getPtr();
    NonOpaque = GA2.I;
  }

  // One access is opaque, while the other has a pointer.  For the opaque access
  // to race, the pointer must escape before the non-opaque instruction.
  if (!PointerCapturedBefore(Ptr, NonOpaque))
    return false;

  // TODO: Use the instruction that performs the capture to further bound the
  // subsequent loop checks.

  // Otherwise we check the logical parallelism of the access.  Because one of
  // the pointers is null, we assume that the "minimum object depth" is 0.
  unsigned MinObjDepth = 0;
  LLVM_DEBUG(dbgs() << "Min loop depth " << MinObjDepth
                    << " used for opaque accesses.\n");

  // Find the deepest loop that contains both B1 and B2.
  const Loop *CommonLoop = getCommonLoop(B1, B2, LI);
  unsigned MaxLoopDepthToCheck = CommonLoop ? CommonLoop->getLoopDepth() : 0;

  // Check if dependence does not depend on looping.
  if (0 == MaxLoopDepthToCheck)
    // If there's no loop to worry about, then the existence of the dependence
    // implies the potential for a race.
    return true;

  LLVM_DEBUG(
      if (MinObjDepth > MaxLoopDepthToCheck) {
        dbgs() << "\tI1 " << *GA1.I << "\n\tI2 " << *GA2.I;
        dbgs() << "\n\tMaxLoopDepthToCheck " << MaxLoopDepthToCheck;
        dbgs() << "\n\tMinObjDepthToCheck " << MinObjDepth << "\n";
        dbgs() << *GA1.I->getFunction();
      });
  assert(MinObjDepth <= MaxLoopDepthToCheck &&
         "Minimum loop depth of underlying object cannot be greater "
         "than maximum loop depth of dependence.");

  // Get the task that encloses both B1 and B2.
  const Task *CommonTask = TI.getEnclosingTask(B1, B2);
  // Get the representative spindles for both B1 and B2 in this common task.
  const Spindle *I1Spindle = GetRepSpindleInTask(TI.getSpindleFor(B1),
                                                 CommonTask, TI);
  const Spindle *I2Spindle = GetRepSpindleInTask(TI.getSpindleFor(B2),
                                                 CommonTask, TI);
  // If this common loop does not contain the common task, then dependencies at
  // the level of this common loop do not constitute a potential race.  Find the
  // loop that contains the enclosing task.
  //
  // Skip this step if either representative spindle is a shared-eh spindle,
  // because those are more complicated.
  if (!I1Spindle->isSharedEH() && !I2Spindle->isSharedEH()) {
    if (!CommonLoop->contains(CommonTask->getEntry())) {
      const Loop *CommonTaskLoop = LI.getLoopFor(CommonTask->getEntry());
      // Typically, CommonTaskLoop is a subloop of CommonLoop.  But that doesn't
      // have to be true, e.g., if CommonLoop appears in an exit of
      // CommonTaskLoop.
      // assert((!CommonTaskLoop || CommonTaskLoop->contains(CommonLoop)) &&
      //        "Loop for common task does not contain common loop.");
      CommonLoop = CommonTaskLoop;
    }
    // Update MaxLoopDepthToCheck
    MaxLoopDepthToCheck = CommonLoop ? CommonLoop->getLoopDepth() : 0;

    // Check if dependence does not depend on looping.
    if (0 == MaxLoopDepthToCheck)
      MaxLoopDepthToCheck = MinObjDepth;
  }

  if (MaxLoopDepthToCheck == MinObjDepth) {
    LLVM_DEBUG(dbgs() << "Minimum object depth matches maximum loop depth.\n");
    if (TI.getTaskFor(B1) == TI.getTaskFor(B2))
      return false;

    // Check if dependence does not depend on looping.
    if (0 == MaxLoopDepthToCheck)
      // If there's no loop to worry about, then the existence of the dependence
      // implies the potential for a race.
      return true;

    // Check if the instructions are parallel when the loop backedge is excluded
    // from dataflow.
    for (const Task *MPT : MPTasksInLoop.TaskList[I1Spindle])
      if (TI.encloses(MPT, B2))
        return true;
    for (const Task *MPT : MPTasksInLoop.TaskList[I2Spindle])
      if (TI.encloses(MPT, B1))
        return true;

    return false;
  }

  // The opaque access acts like a dependence across all iterations of any loops
  // containing the accesses.
  return true;
}

static void setObjectMRForRace(RaceInfo::ObjectMRTy &ObjectMRForRace,
                               const Value *Ptr, ModRefInfo MRI) {
  if (!ObjectMRForRace.count(Ptr))
    ObjectMRForRace[Ptr] = ModRefInfo::NoModRef;
  ObjectMRForRace[Ptr] |= MRI;
}

void AccessPtrAnalysis::recordLocalRace(const GeneralAccess &GA,
                                        RaceInfo::ResultTy &Result,
                                        RaceInfo::ObjectMRTy &ObjectMRForRace,
                                        const GeneralAccess &Racer) {
  Result.recordLocalRace(GA, Racer);

  if (!GA.getPtr())
    return;

  for (const Value *Obj : AccessToObjs[MemAccessInfo(GA.getPtr(), GA.isMod())]) {
    if (GA.isMod())
      setObjectMRForRace(ObjectMRForRace, Obj, ModRefInfo::Ref);
    setObjectMRForRace(ObjectMRForRace, Obj, ModRefInfo::Mod);
  }
}

static void recordAncestorRace(const GeneralAccess &GA, const Value *Ptr,
                               RaceInfo::ResultTy &Result,
                               RaceInfo::ObjectMRTy &ObjectMRForRace,
                               const GeneralAccess &Racer = GeneralAccess()) {
  if (GA.isMod()) {
    Result.recordRaceViaAncestorRef(GA, Racer);
    setObjectMRForRace(ObjectMRForRace, Ptr, ModRefInfo::Ref);
  }
  Result.recordRaceViaAncestorMod(GA, Racer);
  setObjectMRForRace(ObjectMRForRace, Ptr, ModRefInfo::Mod);
}

static void recordOpaqueRace(const GeneralAccess &GA, const Value *Ptr,
                             RaceInfo::ResultTy &Result,
                             RaceInfo::ObjectMRTy &ObjectMRForRace,
                             const GeneralAccess &Racer = GeneralAccess()) {
  if (GA.isMod()) {
    Result.recordOpaqueRace(GA, Racer);
    setObjectMRForRace(ObjectMRForRace, Ptr, ModRefInfo::Ref);
  }
  Result.recordOpaqueRace(GA, Racer);
  setObjectMRForRace(ObjectMRForRace, Ptr, ModRefInfo::Mod);
}

// Returns NoAlias/MayAliass/MustAlias for two memory locations based upon their
// underlaying objects. If LocA and LocB are known to not alias (for any reason:
// tbaa, non-overlapping regions etc), then it is known there is no dependecy.
// Otherwise the underlying objects are checked to see if they point to
// different identifiable objects.
AliasResult
AccessPtrAnalysis::underlyingObjectsAlias(const GeneralAccess &GAA,
                                          const GeneralAccess &GAB) {
  MemoryLocation LocA = *GAA.Loc;
  MemoryLocation LocB = *GAB.Loc;
  // Check the original locations (minus size) for noalias, which can happen for
  // tbaa, incompatible underlying object locations, etc.
  MemoryLocation LocAS =
      MemoryLocation::getBeforeOrAfter(LocA.Ptr, LocA.AATags);
  MemoryLocation LocBS =
      MemoryLocation::getBeforeOrAfter(LocB.Ptr, LocB.AATags);
  if (AA->alias(LocAS, LocBS) == AliasResult::NoAlias)
    return AliasResult::NoAlias;

  // Check the underlying objects are the same
  const Value *AObj = getUnderlyingObject(LocA.Ptr);
  const Value *BObj = getUnderlyingObject(LocB.Ptr);

  // If the underlying objects are the same, they must alias
  if (AObj == BObj)
    return AliasResult::MustAlias;

  // We may have hit the recursion limit for underlying objects, or have
  // underlying objects where we don't know they will alias.
  if (!isIdentifiedObject(AObj) || !isIdentifiedObject(BObj)) {
    if ((isIdentifiedObject(AObj) && !PointerCapturedBefore(AObj, GAB.I)) ||
        (isIdentifiedObject(BObj) && !PointerCapturedBefore(BObj, GAA.I)))
      return AliasResult::NoAlias;
    return AliasResult::MayAlias;
  }

  // Otherwise we know the objects are different and both identified objects so
  // must not alias.
  return AliasResult::NoAlias;
}

static bool isThreadLocalObject(const Value *V) {
  if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(V))
    return Intrinsic::threadlocal_address == II->getIntrinsicID();
  if (const GlobalValue *GV = dyn_cast<GlobalValue>(V))
    return GV->isThreadLocal();
  return false;
}

void AccessPtrAnalysis::evaluateMaybeParallelAccesses(
    GeneralAccess &GA1, GeneralAccess &GA2, RaceInfo::ResultTy &Result,
    RaceInfo::ObjectMRTy &ObjectMRForRace) {
  // No race is possible if no access modifies.
  if (!GA1.isMod() && !GA2.isMod())
    return;

  bool LocalRace = false;
  if (!GA1.getPtr() || !GA2.getPtr()) {
    LLVM_DEBUG({
        dbgs() << "Checking for race involving opaque access:\n"
               << "  GA1 =\n";
        if (GA1.getPtr())
          dbgs() << "    Ptr:" << *GA1.getPtr() << "\n";
        else
          dbgs() << "    Ptr: null\n";
        dbgs() << "    I:" << *GA1.I << "\n"
               << "  GA2 =\n";
        if (GA2.getPtr())
          dbgs() << "    Ptr:" << *GA2.getPtr() << "\n";
        else
          dbgs() << "    Ptr: null\n";
        dbgs() << "    I:" << *GA2.I << "\n";});
    if (checkOpaqueAccesses(GA1, GA2))
      LocalRace = true;
  } else {
    // If either GA has a nullptr, then skip the check, since nullptr's cannot
    // alias.
    Function *F = GA1.I->getFunction();
    if (isa<ConstantPointerNull>(GA1.getPtr()) &&
        !NullPointerIsDefined(
            F, GA1.getPtr()->getType()->getPointerAddressSpace()))
      return;
    if (isa<ConstantPointerNull>(GA2.getPtr()) &&
        !NullPointerIsDefined(
            F, GA2.getPtr()->getType()->getPointerAddressSpace()))
      return;

    // If the underlying objects cannot alias, then skip the check.
    if (AliasResult::NoAlias == underlyingObjectsAlias(GA1, GA2))
      return;

    // If both objects are thread-local, then skip the check.
    if (isThreadLocalObject(GA1.getPtr()) && isThreadLocalObject(GA2.getPtr()))
      return;

    LLVM_DEBUG(
        dbgs() << "Checking for race from dependence:\n"
               << "  GA1 =\n"
               << "    Ptr:" << *GA1.getPtr() << "\n    I:" << *GA1.I << "\n"
               << "  GA2 =\n"
               << "    Ptr:" << *GA2.getPtr() << "\n    I:" << *GA2.I << "\n");
    if (checkDependence(DI.depends(&GA1, &GA2, true), GA1, GA2))
      LocalRace = true;
  }

  if (LocalRace) {
    LLVM_DEBUG(dbgs() << "Local race found:\n"
                      << "  I1 =" << *GA1.I << "\n  I2 =" << *GA2.I << "\n");
    recordLocalRace(GA1, Result, ObjectMRForRace, GA2);
    recordLocalRace(GA2, Result, ObjectMRForRace, GA1);
  }
}

void AccessPtrAnalysis::checkForRacesHelper(
    const Task *T, RaceInfo::ResultTy &Result,
    RaceInfo::ObjectMRTy &ObjectMRForRace) {
  SmallPtrSet<const Spindle *, 4> Visited;

  // Now handle each spindle in this task.
  for (const Spindle *S :
         depth_first<InTask<Spindle *>>(T->getEntrySpindle())) {
    LLVM_DEBUG(dbgs() << "Testing Spindle@" << S->getEntry()->getName()
                      << "\n");
    for (GeneralAccess GA : SpindleAccessMap[S]) {
      if (GA.getPtr()) {
        LLVM_DEBUG({
          dbgs() << "GA Underlying objects:\n";
          for (const Value *Obj :
               AccessToObjs[MemAccessInfo(GA.getPtr(), GA.isMod())])
            dbgs() << "    " << *Obj << "\n";
        });
        for (const Value *Obj :
             AccessToObjs[MemAccessInfo(GA.getPtr(), GA.isMod())]) {
          if (isa<AllocaInst>(Obj))
            // Races on alloca'd objects are checked locally.
            continue;

          if (AssumeSafeMalloc && isAllocFn(Obj, TLI))
            // Races on malloc'd objects are checked locally.
            continue;

          if (const Argument *A = dyn_cast<Argument>(Obj)) {
            // Check if the attributes on the argument preclude a race with the
            // caller.
            if (A->hasByValAttr() || // A->hasNoAliasAttr() ||
                A->hasStructRetAttr() || A->hasInAllocaAttr())
              continue;

            // Otherwise record the possible race with an ancestor.
            LLVM_DEBUG(dbgs() << "Setting race via ancestor:\n"
                              << "  GA.I: " << *GA.I << "\n"
                              << "  Arg: " << *A << "\n");
            recordAncestorRace(GA, A, Result, ObjectMRForRace);
            continue;
          }

          if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(Obj)) {
            // Constant variables cannot race.
            assert(!GV->isConstant() && "Constant GV should be excluded.");
            if (GV->hasPrivateLinkage() || GV->hasInternalLinkage()) {
              // Races are only possible with ancestor functions in this module.
              LLVM_DEBUG(dbgs() << "Setting race via private/internal global:\n"
                                << "  GA.I: " << *GA.I << "\n"
                                << "  GV: " << *GV << "\n");
              // TODO: Add MAAPs for private and internal global variables.
              recordAncestorRace(GA, GV, Result, ObjectMRForRace);
              // recordOpaqueRace(GA, GV, Result, ObjectMRForRace);
            } else {
              // Record the possible opaque race.
              LLVM_DEBUG(dbgs() << "Setting opaque race:\n"
                                << "  GA.I: " << *GA.I << "\n"
                                << "  GV: " << *GV << "\n");
              recordOpaqueRace(GA, GV, Result, ObjectMRForRace);
            }
            continue;
          }

          if (isa<ConstantExpr>(Obj)) {
            // Record the possible opaque race.
            LLVM_DEBUG(dbgs() << "Setting opaque race:\n"
                              << "  GA.I: " << *GA.I << "\n"
                              << "  Obj: " << *Obj << "\n");
            recordOpaqueRace(GA, Obj, Result, ObjectMRForRace);
            continue;
          }

          if (!isa<Instruction>(Obj)) {
            dbgs() << "ALERT: Unexpected underlying object: " << *Obj << "\n";
          }

          // Record the possible opaque race.
          LLVM_DEBUG(dbgs() << "Setting opaque race:\n"
                            << "  GA.I: " << *GA.I << "\n"
                            << "  Obj: " << *Obj << "\n");
          recordOpaqueRace(GA, Obj, Result, ObjectMRForRace);
        }
      }
    }
    for (const Task *MPT : MPTasks.TaskList[S]) {
      LLVM_DEBUG(dbgs() << "Testing against Task@" << MPT->getEntry()->getName()
                        << "\n");
      for (const Task *SubMPT : depth_first(MPT))
        for (GeneralAccess GA1 : SpindleAccessMap[S])
          for (GeneralAccess GA2 : TaskAccessMap[SubMPT])
            evaluateMaybeParallelAccesses(GA1, GA2, Result, ObjectMRForRace);
    }
    // If a successor of this spindle belongs to a subtask, recursively process
    // that subtask.
    for (const Spindle *Succ : successors(S)) {
      if (S->succInSubTask(Succ)) {
        // Skip successor spindles we've seen before.
        if (!Visited.insert(Succ).second)
          continue;
        checkForRacesHelper(Succ->getParentTask(), Result, ObjectMRForRace);
      }
    }
  }
}

void AccessPtrAnalysis::processAccessPtrs(
    RaceInfo::ResultTy &Result, RaceInfo::ObjectMRTy &ObjectMRForRace,
    RaceInfo::PtrChecksTy &AllPtrRtChecks) {
  TI.evaluateParallelState<MaybeParallelTasks>(MPTasks);
  TI.evaluateParallelState<MaybeParallelTasksInLoopBody>(MPTasksInLoop);

  // using InstPtrPair = std::pair<const Instruction *, const Value *>;
  // SmallPtrSet<InstPtrPair, 32> Visited;
  for (const Spindle *S :
         depth_first<Spindle *>(TI.getRootTask()->getEntrySpindle())) {
    for (GeneralAccess GA : SpindleAccessMap[S]) {
      // InstPtrPair Visit =
      //   std::make_pair<const Instruction *, const Value *>(GA.I, GA.getPtr());
      // // Skip instructions we've already visited.
      // if (!Visited.insert(Visit).second)
      //   continue;

      if (!GA.getPtr()) {
        if (const CallBase *Call = dyn_cast<CallBase>(GA.I)) {
          if (!Call->onlyAccessesArgMemory() &&
              !(AssumeSafeMalloc &&
                (isAllocFn(Call, TLI) || isFreeFn(Call, TLI)))) {
            LLVM_DEBUG(dbgs() << "Setting opaque race:\n"
                              << "  GA.I: " << *GA.I << "\n"
                              << "  no explicit racer\n");
            Result.recordOpaqueRace(GA, GeneralAccess());
          }
        }
      }

      // Check for aliasing against the function arguments.
      for (Value *ArgPtr : ArgumentPtrs) {
        LLVM_DEBUG({
            dbgs() << "Checking instruction against arg pointer:\n"
                   << "  GA.I: " << *GA.I << "\n"
                   << "  Arg: " << *ArgPtr << "\n";
          });
        if (!GA.getPtr()) {
          ModRefInfo MRI =
              AA->getModRefInfo(GA.I, MemoryLocation::getBeforeOrAfter(ArgPtr));
          Argument *Arg = cast<Argument>(ArgPtr);
          if (isModSet(MRI) && !Arg->onlyReadsMemory()) {
            LLVM_DEBUG(dbgs() << "  Mod is set.\n");
            Result.recordRaceViaAncestorRef(GA, GeneralAccess());
            Result.recordRaceViaAncestorMod(GA, GeneralAccess());
            setObjectMRForRace(ObjectMRForRace, ArgPtr, ModRefInfo::ModRef);
          }
          if (isRefSet(MRI)) {
            LLVM_DEBUG(dbgs() << "  Ref is set.\n");
            Result.recordRaceViaAncestorMod(GA, GeneralAccess());
            setObjectMRForRace(ObjectMRForRace, ArgPtr, ModRefInfo::Mod);
          }
        } else {
          MemoryLocation GALoc = *GA.Loc;
          if (AA->alias(GALoc, MemoryLocation::getBeforeOrAfter(ArgPtr))) {
            Argument *Arg = cast<Argument>(ArgPtr);
            if (GA.isMod() && !Arg->onlyReadsMemory()) {
              LLVM_DEBUG(dbgs() << "  Mod is set.\n");
              Result.recordRaceViaAncestorRef(GA, GeneralAccess());
              Result.recordRaceViaAncestorMod(GA, GeneralAccess());
              setObjectMRForRace(ObjectMRForRace, ArgPtr, ModRefInfo::ModRef);
            }
            if (GA.isRef()) {
              LLVM_DEBUG(dbgs() << "  Ref is set.\n");
              Result.recordRaceViaAncestorMod(GA, GeneralAccess());
              setObjectMRForRace(ObjectMRForRace, ArgPtr, ModRefInfo::Mod);
            }
          }
        }
      }
    }
  }
  checkForRacesHelper(TI.getRootTask(), Result, ObjectMRForRace);
}

RaceInfo::RaceInfo(Function *F, DominatorTree &DT, LoopInfo &LI, TaskInfo &TI,
                   DependenceInfo &DI, ScalarEvolution &SE,
                   const TargetLibraryInfo *TLI)
    : F(F), DT(DT), LI(LI), TI(TI), DI(DI), SE(SE), TLI(TLI) {
  analyzeFunction();
}

void RaceInfo::getObjectsFor(Instruction *I,
                             SmallPtrSetImpl<const Value *> &Objects) {
  SmallVector<GeneralAccess, 1> GA;
  GetGeneralAccesses(I, GA, DI.getAA(), TLI);
  for (GeneralAccess Acc : GA) {
    // Skip this access if it does not have a valid pointer.
    if (!Acc.getPtr())
      continue;

    getObjectsFor(MemAccessInfo(Acc.getPtr(), Acc.isMod()), Objects);
  }
}

void RaceInfo::getObjectsFor(MemAccessInfo Access,
                             SmallPtrSetImpl<const Value *> &Objects) {
  for (const Value *Obj : AccessToObjs[Access])
    Objects.insert(Obj);
}

void RaceInfo::print(raw_ostream &OS) const {
  if (Result.empty()) {
    OS << "No possible races\n";
    return;
  }
  RaceType OverallRT = getOverallRaceType();
  OS << "Overall race type: ";
  printRaceType(OverallRT, OS);
  OS << "\n";
  for (auto Res : Result) {
    OS << "  Result: " << *Res.first << "\n";
    for (auto &RD : Res.second) {
      if (RD.getPtr())
        OS << "    ptr: " << *RD.getPtr();
      else
        OS << "    nullptr";
      OS << "\n";
      printRaceType(RD.Type, OS.indent(6));
      if (RD.Racer.isValid()) {
        OS << "\n      Racer:";
        OS << "\n        I = " << *RD.Racer.I;
        OS << "\n        Loc = ";
        if (!RD.Racer.Loc)
          OS << "nullptr";
        else if (RD.Racer.Loc->Ptr == RD.getPtr())
          OS << "same pointer";
        else
          OS << *RD.Racer.Loc->Ptr;
        OS << "\n        OperandNum = ";
        if (RD.Racer.OperandNum == static_cast<unsigned>(-1))
          OS << "none";
        else
          OS << RD.Racer.OperandNum;
        OS << "\n        ModRef = " << (RD.Racer.isMod() ? "Mod " : "")
           << (RD.Racer.isRef() ? "Ref" : "");
      }
      else
        OS << "\n      Opaque racer";
      OS << "\n";
    }
  }
  OS << "Underlying objects of races:\n";
  for (auto Res : ObjectMRForRace) {
    OS << *Res.first << "\n   ";
    if (isModSet(Res.second))
      OS << " Mod";
    if (isRefSet(Res.second))
      OS << " Ref";
    OS << "\n";
  }
}

// The main analysis routine.
void RaceInfo::analyzeFunction() {
  LLVM_DEBUG(dbgs() << "Analyzing function '" << F->getName() << "'\n");

  // At a high level, we need to identify pairs of instructions that might
  // execute in parallel and alias.

  AccessPtrAnalysis APA(DT, TI, LI, DI, SE, TLI, AccessToObjs);
  // Record pointer arguments to this function
  for (Argument &Arg : F->args())
    if (Arg.getType()->isPtrOrPtrVectorTy())
      APA.addFunctionArgument(&Arg);
  // TODO: Add global variables to APA.

  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB.instructionsWithoutDebug()) {
      if (I.mayReadFromMemory() || I.mayWriteToMemory()) {
        if (checkInstructionForRace(&I, TLI))
          APA.addAccess(&I);
      }
    }
  }

  APA.processAccessPtrs(Result, ObjectMRForRace, AllPtrRtChecks);
}
