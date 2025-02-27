//===- LambdaABI.cpp - Generic interface to various runtime systems--------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Lambda ABI to convert Tapir instructions to calls
// into a generic runtime system to operates on spawned computations as lambdas.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Tapir/LambdaABI.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/TapirTaskInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/Tapir/Outline.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/EscapeEnumerator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/TapirUtils.h"

using namespace llvm;

#define DEBUG_TYPE "lambdaabi"

extern cl::opt<bool> DebugABICalls;

static cl::opt<std::string>
    ClRuntimeBCPath("tapir-runtime-bc-path", cl::init(""),
                    cl::desc("Path to the bitcode file for the runtime ABI"),
                    cl::Hidden);

static const StringRef StackFrameName = "__rts_sf";

namespace {

// Custom DiagnosticInfo for linking the Lambda ABI bitcode file.
class LambdaABILinkDiagnosticInfo : public DiagnosticInfo {
  const Module *SrcM;
  const Twine &Msg;

public:
  LambdaABILinkDiagnosticInfo(DiagnosticSeverity Severity, const Module *SrcM,
                              const Twine &Msg)
      : DiagnosticInfo(DK_Lowering, Severity), SrcM(SrcM), Msg(Msg) {}
  void print(DiagnosticPrinter &DP) const override {
    DP << "linking module '" << SrcM->getModuleIdentifier() << "': " << Msg;
  }
};

// Custom DiagnosticHandler to handle diagnostics arising when linking the
// Lambda ABI bitcode file.
class LambdaABIDiagnosticHandler final : public DiagnosticHandler {
  const Module *SrcM;
  DiagnosticHandler *OrigHandler;

public:
  LambdaABIDiagnosticHandler(const Module *SrcM, DiagnosticHandler *OrigHandler)
      : SrcM(SrcM), OrigHandler(OrigHandler) {}

  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    if (DI.getKind() != DK_Linker)
      return OrigHandler->handleDiagnostics(DI);

    std::string MsgStorage;
    {
      raw_string_ostream Stream(MsgStorage);
      DiagnosticPrinterRawOStream DP(Stream);
      DI.print(DP);
    }
    return OrigHandler->handleDiagnostics(
        LambdaABILinkDiagnosticInfo(DI.getSeverity(), SrcM, MsgStorage));
  }
};

// Structure recording information about runtime ABI functions.
struct RTSFnDesc {
  StringRef FnName;
  FunctionType *FnType;
  FunctionCallee &FnCallee;
};
} // namespace

// void LambdaABI::setOptions(const TapirTargetOptions &Options) {
//   if (!isa<LambdaABIOptions>(Options))
//     return;

//   const LambdaABIOptions &OptionsCast = cast<LambdaABIOptions>(Options);

//   // Get the path to the runtime bitcode file.
//   RuntimeBCPath = OptionsCast.getRuntimeBCPath();
// }

void LambdaABI::prepareModule() {
  LLVMContext &C = M.getContext();
  const DataLayout &DL = DestM.getDataLayout();
  Type *Int8Ty = Type::getInt8Ty(C);
  Type *Int16Ty = Type::getInt16Ty(C);
  Type *Int32Ty = Type::getInt32Ty(C);
  Type *Int64Ty = Type::getInt64Ty(C);

  // If a runtime bitcode path is given via the command line, use it.
  if ("" != ClRuntimeBCPath)
    RuntimeBCPath = ClRuntimeBCPath;

  if ("" == RuntimeBCPath) {
    C.emitError("LambdaABI: No bitcode ABI file given.");
    return;
  }

  LLVM_DEBUG(dbgs() << "Using external bitcode file for Lambda ABI: "
                    << RuntimeBCPath << "\n");
  SMDiagnostic SMD;

  // Parse the bitcode file.  This call imports structure definitions, but not
  // function definitions.
  if (std::unique_ptr<Module> ExternalModule =
          parseIRFile(RuntimeBCPath, SMD, C)) {
    // Get the original DiagnosticHandler for this context.
    std::unique_ptr<DiagnosticHandler> OrigDiagHandler =
        C.getDiagnosticHandler();

    // Setup an LambdaABIDiagnosticHandler for this context, to handle
    // diagnostics that arise from linking ExternalModule.
    C.setDiagnosticHandler(std::make_unique<LambdaABIDiagnosticHandler>(
        ExternalModule.get(), OrigDiagHandler.get()));

    // Link the external module into the current module, copying over global
    // values.
    //
    // TODO: Consider restructuring the import process to use
    // Linker::Flags::LinkOnlyNeeded to copy over only the necessary contents
    // from the external module.
    bool Fail = Linker::linkModules(
        M, std::move(ExternalModule), Linker::Flags::None,
        [](Module &M, const StringSet<> &GVS) {
          for (StringRef GVName : GVS.keys()) {
            LLVM_DEBUG(dbgs() << "Linking global value " << GVName << "\n");
            if (Function *Fn = M.getFunction(GVName)) {
              if (!Fn->isDeclaration() && !Fn->hasComdat())
                // We set the function's linkage as available_externally, so
                // that subsequent optimizations can remove these definitions
                // from the module.  We don't want this module redefining any of
                // these symbols, even if they aren't inlined, because the
                // Lambda runtime library will provide those definitions later.
                Fn->setLinkage(Function::AvailableExternallyLinkage);
            } else if (GlobalVariable *G = M.getGlobalVariable(GVName)) {
              if (!G->isDeclaration() && !G->hasComdat())
                G->setLinkage(GlobalValue::AvailableExternallyLinkage);
            }
          }
        });
    if (Fail)
      C.emitError("LambdaABI: Failed to link bitcode ABI file: " +
                  Twine(RuntimeBCPath));

    // Restore the original DiagnosticHandler for this context.
    C.setDiagnosticHandler(std::move(OrigDiagHandler));
  } else {
    C.emitError("LambdaABI: Failed to parse bitcode ABI file: " +
                Twine(RuntimeBCPath));
  }

  // Get or create local definitions of RTS structure types.
  const char *StackFrameName = "struct.__rts_stack_frame";
  StackFrameTy = StructType::lookupOrCreate(C, StackFrameName);

  PointerType *StackFramePtrTy = PointerType::getUnqual(StackFrameTy);
  Type *VoidTy = Type::getVoidTy(C);
  Type *VoidPtrTy = PointerType::getUnqual(C);

  // Define the types of the RTS functions.
  FunctionType *RTSFnTy = FunctionType::get(VoidTy, {StackFramePtrTy}, false);
  SpawnBodyFnArgTy = VoidPtrTy;
  Type *IntPtrTy = DL.getIntPtrType(C);
  SpawnBodyFnArgSizeTy = IntPtrTy;
  SpawnBodyFnTy = FunctionType::get(VoidTy, {SpawnBodyFnArgTy}, false);
  FunctionType *SpawnFnTy =
      FunctionType::get(VoidTy,
                        {StackFramePtrTy, PointerType::getUnqual(SpawnBodyFnTy),
                         SpawnBodyFnArgTy, SpawnBodyFnArgSizeTy, IntPtrTy},
                        false);
  FunctionType *Grainsize8FnTy = FunctionType::get(Int8Ty, {Int8Ty}, false);
  FunctionType *Grainsize16FnTy = FunctionType::get(Int16Ty, {Int16Ty}, false);
  FunctionType *Grainsize32FnTy = FunctionType::get(Int32Ty, {Int32Ty}, false);
  FunctionType *Grainsize64FnTy = FunctionType::get(Int64Ty, {Int64Ty}, false);
  FunctionType *WorkerInfoTy = FunctionType::get(Int32Ty, {}, false);

  // Create an array of RTS functions, with their associated types and
  // FunctionCallee member variables in the LambdaABI class.
  RTSFnDesc RTSFunctions[] = {
      {"__rts_enter_frame", RTSFnTy, RTSEnterFrame},
      {"__rts_spawn", SpawnFnTy, RTSSpawn},
      {"__rts_leave_frame", RTSFnTy, RTSLeaveFrame},
      {"__rts_sync", RTSFnTy, RTSSync},
      {"__rts_sync_nothrow", RTSFnTy, RTSSyncNoThrow},
      {"__rts_loop_grainsize_8", Grainsize8FnTy, RTSLoopGrainsize8},
      {"__rts_loop_grainsize_16", Grainsize16FnTy, RTSLoopGrainsize16},
      {"__rts_loop_grainsize_32", Grainsize32FnTy, RTSLoopGrainsize32},
      {"__rts_loop_grainsize_64", Grainsize64FnTy, RTSLoopGrainsize64},
      {"__rts_get_num_workers", WorkerInfoTy, RTSGetNumWorkers},
      {"__rts_get_worker_id", WorkerInfoTy, RTSGetWorkerID},
  };

  // Add attributes to internalized functions.
  for (RTSFnDesc FnDesc : RTSFunctions) {
    assert(!FnDesc.FnCallee && "Redefining RTS function");
    FnDesc.FnCallee = M.getOrInsertFunction(FnDesc.FnName, FnDesc.FnType);
    assert(isa<Function>(FnDesc.FnCallee.getCallee()) &&
           "Runtime function is not a function");
    Function *Fn = cast<Function>(FnDesc.FnCallee.getCallee());

    Fn->setDoesNotThrow();

    // Unless we're debugging, mark the function as always_inline.  This
    // attribute is required for some functions, but is helpful for all
    // functions.
    if (!DebugABICalls)
      Fn->addFnAttr(Attribute::AlwaysInline);
    else
      Fn->removeFnAttr(Attribute::AlwaysInline);

    if (Fn->getName() == "__rts_get_num_workers" ||
        Fn->getName() == "__rts_get_worker_id") {
      Fn->setLinkage(Function::InternalLinkage);
    }
  }

  // If no valid bitcode file was found fill in the missing pieces.
  // An error should have been emitted already unless the user
  // set DebugABICalls.

  if (StackFrameTy->isOpaque()) {
    // TODO: Figure out better handling of this potential error.
    LLVM_DEBUG(dbgs() << "LambdaABI: Failed to find __rts_stack_frame type.\n");
    // Create a dummy __rts_stack_frame structure
    StackFrameTy->setBody(Int64Ty);
  }
  // Create declarations of all RTS functions, and add basic attributes to those
  // declarations.
  for (RTSFnDesc FnDesc : RTSFunctions) {
    if (FnDesc.FnCallee)
      continue;
    FnDesc.FnCallee = M.getOrInsertFunction(FnDesc.FnName, FnDesc.FnType);
    assert(isa<Function>(FnDesc.FnCallee.getCallee()) &&
           "RTS function is not a function");
    Function *Fn = cast<Function>(FnDesc.FnCallee.getCallee());

    Fn->setDoesNotThrow();
  }
}

void LambdaABI::addHelperAttributes(Function &Helper) {
  // Inlining the helper function is not legal.
  Helper.removeFnAttr(Attribute::AlwaysInline);
  Helper.addFnAttr(Attribute::NoInline);
  // If the helper uses an argument structure, then it is not a write-only
  // function.
  if (getArgStructMode() != ArgStructMode::None) {
    Helper.removeFnAttr(Attribute::WriteOnly);
    Helper.setMemoryEffects(
        MemoryEffects(MemoryEffects::Location::Other, ModRefInfo::ModRef));
  }
  // Note that the address of the helper is unimportant.
  Helper.setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  // The helper is internal to this module.  We use internal linkage, rather
  // than private linkage, so that tools can still reference the helper
  // function.
  Helper.setLinkage(GlobalValue::InternalLinkage);
}

// Check whether the allocation of a __rts_stack_frame can be inserted after
// instruction \p I.
static bool skipInstruction(const Instruction &I) {
  if (isa<AllocaInst>(I))
    return true;

  if (isa<DbgInfoIntrinsic>(I))
    return true;

  if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I)) {
    // Skip simple intrinsics
    switch (II->getIntrinsicID()) {
    case Intrinsic::annotation:
    case Intrinsic::assume:
    case Intrinsic::sideeffect:
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
    case Intrinsic::launder_invariant_group:
    case Intrinsic::strip_invariant_group:
    case Intrinsic::is_constant:
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::objectsize:
    case Intrinsic::ptr_annotation:
    case Intrinsic::var_annotation:
    case Intrinsic::experimental_gc_result:
    case Intrinsic::experimental_gc_relocate:
    case Intrinsic::experimental_noalias_scope_decl:
    case Intrinsic::syncregion_start:
    case Intrinsic::taskframe_create:
      return true;
    default:
      return false;
    }
  }

  return false;
}

// Scan the basic block \p B to find a point to insert the allocation of a
// __rts_stack_frame.
static Instruction *getStackFrameInsertPt(BasicBlock &B) {
  BasicBlock::iterator BI(B.getFirstInsertionPt());
  BasicBlock::const_iterator BE(B.end());

  // Scan the basic block for the first instruction we should not skip.
  while (BI != BE) {
    if (!skipInstruction(*BI)) {
      return &*BI;
    }
    ++BI;
  }

  // We reached the end of the basic block; return the terminator.
  return B.getTerminator();
}

/// Create the __rts_stack_frame for the spawning function.
Value *LambdaABI::CreateStackFrame(Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  Type *SFTy = StackFrameTy;

  IRBuilder<> B(getStackFrameInsertPt(F.getEntryBlock()));
  AllocaInst *SF = B.CreateAlloca(SFTy, DL.getAllocaAddrSpace(),
                                  /*ArraySize*/ nullptr,
                                  /*Name*/ StackFrameName);

  SF->setAlignment(StackFrameAlign);

  return SF;
}

Value *LambdaABI::GetOrCreateStackFrame(Function &F) {
  if (DetachCtxToStackFrame.count(&F))
    return DetachCtxToStackFrame[&F];

  Value *SF = CreateStackFrame(F);
  DetachCtxToStackFrame[&F] = SF;

  return SF;
}

// Insert a call in Function F to __rts_enter_frame to initialize the
// __rts_stack_frame in F.  If TaskFrameCreate is nonnull, the call to
// __rts_enter_frame is inserted at TaskFrameCreate.
CallInst *LambdaABI::InsertStackFramePush(Function &F,
                                          Instruction *TaskFrameCreate,
                                          bool Helper) {
  Instruction *SF = cast<Instruction>(GetOrCreateStackFrame(F));

  BasicBlock::iterator InsertPt = ++SF->getIterator();
  IRBuilder<> B(&(F.getEntryBlock()), InsertPt);
  if (TaskFrameCreate)
    B.SetInsertPoint(TaskFrameCreate);
  if (!B.getCurrentDebugLocation()) {
    // Try to find debug information later in this block for the ABI call.
    BasicBlock::iterator BI = B.GetInsertPoint();
    BasicBlock::const_iterator BE(B.GetInsertBlock()->end());
    while (BI != BE) {
      if (DebugLoc Loc = BI->getDebugLoc()) {
        B.SetCurrentDebugLocation(Loc);
        break;
      }
      ++BI;
    }
  }

  Value *Args[1] = {SF};
  return B.CreateCall(RTSEnterFrame, Args);
}

// Insert a call in Function F to pop the stack frame.
//
// PromoteCallsToInvokes dictates whether call instructions that can throw are
// promoted to invoke instructions prior to inserting the epilogue-function
// calls.
void LambdaABI::InsertStackFramePop(Function &F, bool PromoteCallsToInvokes,
                                    bool InsertPauseFrame, bool Helper) {
  Value *SF = GetOrCreateStackFrame(F);
  SmallPtrSet<ReturnInst *, 8> Returns;
  SmallPtrSet<ResumeInst *, 8> Resumes;

  // Add eh cleanup that returns control to the runtime
  EscapeEnumerator EE(F, "rts_cleanup", PromoteCallsToInvokes);
  while (IRBuilder<> *Builder = EE.Next()) {
    if (ResumeInst *RI = dyn_cast<ResumeInst>(Builder->GetInsertPoint())) {
      if (!RI->getDebugLoc())
        // Attempt to set the debug location of this resume to match one of the
        // preceeding terminators.
        for (const BasicBlock *Pred : predecessors(RI->getParent()))
          if (const DebugLoc &Loc = Pred->getTerminator()->getDebugLoc()) {
            RI->setDebugLoc(Loc);
            break;
          }
      Resumes.insert(RI);
    } else if (ReturnInst *RI = dyn_cast<ReturnInst>(Builder->GetInsertPoint()))
      Returns.insert(RI);
  }

  for (ReturnInst *RI : Returns) {
    CallInst::Create(RTSLeaveFrame, {SF}, "", RI)
        ->setDebugLoc(RI->getDebugLoc());
  }
}

/// Lower a call to get the grainsize of a Tapir loop.
Value *LambdaABI::lowerGrainsizeCall(CallInst *GrainsizeCall) {
  Value *Limit = GrainsizeCall->getArgOperand(0);
  IRBuilder<> Builder(GrainsizeCall);

  // Select the appropriate __rts_grainsize function, based on the type.
  FunctionCallee RTSGrainsizeCall;
  if (GrainsizeCall->getType()->isIntegerTy(8))
    RTSGrainsizeCall = RTSLoopGrainsize8;
  else if (GrainsizeCall->getType()->isIntegerTy(16))
    RTSGrainsizeCall = RTSLoopGrainsize16;
  else if (GrainsizeCall->getType()->isIntegerTy(32))
    RTSGrainsizeCall = RTSLoopGrainsize32;
  else if (GrainsizeCall->getType()->isIntegerTy(64))
    RTSGrainsizeCall = RTSLoopGrainsize64;
  else
    llvm_unreachable("No RTSGrainsize call matches type for Tapir loop.");

  Value *Grainsize = Builder.CreateCall(RTSGrainsizeCall, Limit);

  // Replace uses of grainsize intrinsic call with this grainsize value.
  GrainsizeCall->replaceAllUsesWith(Grainsize);
  return Grainsize;
}

// Lower a sync instruction SI.
void LambdaABI::lowerSync(SyncInst &SI) {
  Function &Fn = *SI.getFunction();
  if (!DetachCtxToStackFrame[&Fn])
    // If we have not created a stackframe for this function, then we don't need
    // to handle the sync.
    return;

  Value *SF = GetOrCreateStackFrame(Fn);
  Value *Args[] = {SF};
  assert(Args[0] && "sync used in function without frame!");

  Instruction *SyncUnwind = nullptr;
  BasicBlock *SyncCont = SI.getSuccessor(0);
  BasicBlock *SyncUnwindDest = nullptr;
  // Determine whether a sync.unwind immediately follows SI.
  if (InvokeInst *II =
          dyn_cast<InvokeInst>(SyncCont->getFirstNonPHIOrDbgOrLifetime())) {
    if (isSyncUnwind(II)) {
      SyncUnwind = II;
      SyncCont = II->getNormalDest();
      SyncUnwindDest = II->getUnwindDest();
    }
  }

  CallBase *CB;
  if (!SyncUnwindDest) {
    if (Fn.doesNotThrow())
      CB = CallInst::Create(RTSSyncNoThrow, Args, "",
                            /*insert before*/ &SI);
    else
      CB = CallInst::Create(RTSSync, Args, "", /*insert before*/ &SI);

    BranchInst::Create(SyncCont, CB->getParent());
  } else {
    CB = InvokeInst::Create(RTSSync, SyncCont, SyncUnwindDest, Args, "",
                            /*insert before*/ &SI);
    for (PHINode &PN : SyncCont->phis())
      PN.addIncoming(PN.getIncomingValueForBlock(SyncUnwind->getParent()),
                     SI.getParent());
    for (PHINode &PN : SyncUnwindDest->phis())
      PN.addIncoming(PN.getIncomingValueForBlock(SyncUnwind->getParent()),
                     SI.getParent());
  }
  CB->setDebugLoc(SI.getDebugLoc());
  SI.eraseFromParent();
}

bool LambdaABI::preProcessFunction(Function &F, TaskInfo &TI,
                                   bool ProcessingTapirLoops) {
  return false;
}
void LambdaABI::postProcessFunction(Function &F, bool ProcessingTapirLoops) {}
void LambdaABI::postProcessHelper(Function &F) {}

void LambdaABI::preProcessOutlinedTask(Function &F, Instruction *DetachPt,
                                       Instruction *TaskFrameCreate,
                                       bool IsSpawner, BasicBlock *TFEntry) {
  if (IsSpawner)
    InsertStackFramePush(F, TaskFrameCreate, /*Helper*/ true);
}

void LambdaABI::postProcessOutlinedTask(Function &F, Instruction *DetachPt,
                                        Instruction *TaskFrameCreate,
                                        bool IsSpawner, BasicBlock *TFEntry) {
  if (IsSpawner)
    InsertStackFramePop(F, /*PromoteCallsToInvokes*/ true,
                        /*InsertPauseFrame*/ true, /*Helper*/ true);
}

void LambdaABI::preProcessRootSpawner(Function &F, BasicBlock *TFEntry) {
  InsertStackFramePush(F);
}

void LambdaABI::postProcessRootSpawner(Function &F, BasicBlock *TFEntry) {
  InsertStackFramePop(F, /*PromoteCallsToInvokes*/ false,
                      /*InsertPauseFrame*/ false, /*Helper*/ false);
}

void LambdaABI::processSubTaskCall(TaskOutlineInfo &TOI, DominatorTree &DT) {
  const DataLayout &DL = DestM.getDataLayout();
  CallBase *ReplCall = cast<CallBase>(TOI.ReplCall);

  Function &F = *ReplCall->getFunction();
  Value *SF = DetachCtxToStackFrame[&F];
  assert(SF && "No frame found for spawning task");

  // Get the alignment of the helper arguments.  The bitcode-ABI functions may
  // use the alignment to align the shared variables in the storage allocated by
  // the OpenMP runtime, especially to accommodate vector arguments.
  AllocaInst *ArgAlloca = cast<AllocaInst>(ReplCall->getArgOperand(0));
  uint64_t Alignment =
      DL.getPrefTypeAlign(ArgAlloca->getAllocatedType()).value();

  IRBuilder<> B(ReplCall);
  Value *FnCast = B.CreateBitCast(ReplCall->getCalledFunction(),
                                  PointerType::getUnqual(SpawnBodyFnTy));
  Value *ArgCast =
      B.CreateBitOrPointerCast(ReplCall->getArgOperand(0), SpawnBodyFnArgTy);
  auto ArgSize =
      cast<AllocaInst>(ReplCall->getArgOperand(0))->getAllocationSizeInBits(DL);
  assert(ArgSize &&
         "Could not determine size of compiler-generated ArgStruct.");
  Value *ArgSizeVal = ConstantInt::get(SpawnBodyFnArgSizeTy, *ArgSize / 8);

  if (InvokeInst *II = dyn_cast<InvokeInst>(ReplCall)) {
    B.CreateInvoke(RTSSpawn, II->getNormalDest(), II->getUnwindDest(),
                   {SF, FnCast, ArgCast, ArgSizeVal, B.getInt64(Alignment)});
  } else {
    B.CreateCall(RTSSpawn,
                 {SF, FnCast, ArgCast, ArgSizeVal, B.getInt64(Alignment)});
  }

  ReplCall->eraseFromParent();
}
