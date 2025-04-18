//===- CtxProfAnalysis.cpp - contextual profile analysis ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of the contextual profile analysis, which maintains contextual
// profiling info through IPO passes.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CtxProfAnalysis.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/ProfileData/PGOCtxProfReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#define DEBUG_TYPE "ctx_prof"

using namespace llvm;
cl::opt<std::string>
    UseCtxProfile("use-ctx-profile", cl::init(""), cl::Hidden,
                  cl::desc("Use the specified contextual profile file"));

static cl::opt<CtxProfAnalysisPrinterPass::PrintMode> PrintLevel(
    "ctx-profile-printer-level",
    cl::init(CtxProfAnalysisPrinterPass::PrintMode::YAML), cl::Hidden,
    cl::values(clEnumValN(CtxProfAnalysisPrinterPass::PrintMode::Everything,
                          "everything", "print everything - most verbose"),
               clEnumValN(CtxProfAnalysisPrinterPass::PrintMode::YAML, "yaml",
                          "just the yaml representation of the profile")),
    cl::desc("Verbosity level of the contextual profile printer pass."));

static cl::opt<bool> ForceIsInSpecializedModule(
    "ctx-profile-force-is-specialized", cl::init(false),
    cl::desc("Treat the given module as-if it were containing the "
             "post-thinlink module containing the root"));

const char *AssignGUIDPass::GUIDMetadataName = "guid";

PreservedAnalyses AssignGUIDPass::run(Module &M, ModuleAnalysisManager &MAM) {
  for (auto &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    if (F.getMetadata(GUIDMetadataName))
      continue;
    const GlobalValue::GUID GUID = F.getGUID();
    F.setMetadata(GUIDMetadataName,
                  MDNode::get(M.getContext(),
                              {ConstantAsMetadata::get(ConstantInt::get(
                                  Type::getInt64Ty(M.getContext()), GUID))}));
  }
  return PreservedAnalyses::none();
}

GlobalValue::GUID AssignGUIDPass::getGUID(const Function &F) {
  if (F.isDeclaration()) {
    assert(GlobalValue::isExternalLinkage(F.getLinkage()));
    return GlobalValue::getGUID(F.getGlobalIdentifier());
  }
  auto *MD = F.getMetadata(GUIDMetadataName);
  assert(MD && "guid not found for defined function");
  return cast<ConstantInt>(cast<ConstantAsMetadata>(MD->getOperand(0))
                               ->getValue()
                               ->stripPointerCasts())
      ->getZExtValue();
}
AnalysisKey CtxProfAnalysis::Key;

CtxProfAnalysis::CtxProfAnalysis(std::optional<StringRef> Profile)
    : Profile([&]() -> std::optional<StringRef> {
        if (Profile)
          return *Profile;
        if (UseCtxProfile.getNumOccurrences())
          return UseCtxProfile;
        return std::nullopt;
      }()) {}

PGOContextualProfile CtxProfAnalysis::run(Module &M,
                                          ModuleAnalysisManager &MAM) {
  if (!Profile)
    return {};
  ErrorOr<std::unique_ptr<MemoryBuffer>> MB = MemoryBuffer::getFile(*Profile);
  if (auto EC = MB.getError()) {
    M.getContext().emitError("could not open contextual profile file: " +
                             EC.message());
    return {};
  }
  PGOCtxProfileReader Reader(MB.get()->getBuffer());
  auto MaybeProfiles = Reader.loadProfiles();
  if (!MaybeProfiles) {
    M.getContext().emitError("contextual profile file is invalid: " +
                             toString(MaybeProfiles.takeError()));
    return {};
  }

  // FIXME: We should drive this from ThinLTO, but for the time being, use the
  // module name as indicator.
  // We want to *only* keep the contextual profiles in modules that capture
  // context trees. That allows us to compute specific PSIs, for example.
  auto DetermineRootsInModule = [&M]() -> const DenseSet<GlobalValue::GUID> {
    DenseSet<GlobalValue::GUID> ProfileRootsInModule;
    auto ModName = M.getName();
    auto Filename = sys::path::filename(ModName);
    // Drop the file extension.
    Filename = Filename.substr(0, Filename.find_last_of('.'));
    // See if it parses
    APInt Guid;
    // getAsInteger returns true if there are more chars to read other than the
    // integer. So the "false" test is what we want.
    if (!Filename.getAsInteger(0, Guid))
      ProfileRootsInModule.insert(Guid.getZExtValue());
    return ProfileRootsInModule;
  };
  const auto ProfileRootsInModule = DetermineRootsInModule();
  PGOContextualProfile Result;

  // the logic from here on allows for modules that contain - by design - more
  // than one root. We currently don't support that, because the determination
  // happens based on the module name matching the root guid, but the logic can
  // avoid assuming that.
  if (!ProfileRootsInModule.empty()) {
    Result.IsInSpecializedModule = true;
    // Trim first the roots that aren't in this module.
    for (auto &[RootGuid, _] :
         llvm::make_early_inc_range(MaybeProfiles->Contexts))
      if (!ProfileRootsInModule.contains(RootGuid))
        MaybeProfiles->Contexts.erase(RootGuid);
    // we can also drop the flat profiles
    MaybeProfiles->FlatProfiles.clear();
  }

  for (const auto &F : M) {
    if (F.isDeclaration())
      continue;
    auto GUID = AssignGUIDPass::getGUID(F);
    assert(GUID && "guid not found for defined function");
    const auto &Entry = F.begin();
    uint32_t MaxCounters = 0; // we expect at least a counter.
    for (const auto &I : *Entry)
      if (auto *C = dyn_cast<InstrProfIncrementInst>(&I)) {
        MaxCounters =
            static_cast<uint32_t>(C->getNumCounters()->getZExtValue());
        break;
      }
    if (!MaxCounters)
      continue;
    uint32_t MaxCallsites = 0;
    for (const auto &BB : F)
      for (const auto &I : BB)
        if (auto *C = dyn_cast<InstrProfCallsite>(&I)) {
          MaxCallsites =
              static_cast<uint32_t>(C->getNumCounters()->getZExtValue());
          break;
        }
    auto [It, Ins] = Result.FuncInfo.insert(
        {GUID, PGOContextualProfile::FunctionInfo(F.getName())});
    (void)Ins;
    assert(Ins);
    It->second.NextCallsiteIndex = MaxCallsites;
    It->second.NextCounterIndex = MaxCounters;
  }
  // If we made it this far, the Result is valid - which we mark by setting
  // .Profiles.
  Result.Profiles = std::move(*MaybeProfiles);
  Result.initIndex();
  return Result;
}

GlobalValue::GUID
PGOContextualProfile::getDefinedFunctionGUID(const Function &F) const {
  if (auto It = FuncInfo.find(AssignGUIDPass::getGUID(F)); It != FuncInfo.end())
    return It->first;
  return 0;
}

CtxProfAnalysisPrinterPass::CtxProfAnalysisPrinterPass(raw_ostream &OS)
    : OS(OS), Mode(PrintLevel) {}

PreservedAnalyses CtxProfAnalysisPrinterPass::run(Module &M,
                                                  ModuleAnalysisManager &MAM) {
  CtxProfAnalysis::Result &C = MAM.getResult<CtxProfAnalysis>(M);
  if (C.contexts().empty()) {
    OS << "No contextual profile was provided.\n";
    return PreservedAnalyses::all();
  }

  if (Mode == PrintMode::Everything) {
    OS << "Function Info:\n";
    for (const auto &[Guid, FuncInfo] : C.FuncInfo)
      OS << Guid << " : " << FuncInfo.Name
         << ". MaxCounterID: " << FuncInfo.NextCounterIndex
         << ". MaxCallsiteID: " << FuncInfo.NextCallsiteIndex << "\n";
  }

  if (Mode == PrintMode::Everything)
    OS << "\nCurrent Profile:\n";
  convertCtxProfToYaml(OS, C.profiles());
  OS << "\n";
  if (Mode == PrintMode::YAML)
    return PreservedAnalyses::all();

  OS << "\nFlat Profile:\n";
  auto Flat = C.flatten();
  for (const auto &[Guid, Counters] : Flat) {
    OS << Guid << " : ";
    for (auto V : Counters)
      OS << V << " ";
    OS << "\n";
  }
  return PreservedAnalyses::all();
}

InstrProfCallsite *CtxProfAnalysis::getCallsiteInstrumentation(CallBase &CB) {
  if (!InstrProfCallsite::canInstrumentCallsite(CB))
    return nullptr;
  for (auto *Prev = CB.getPrevNode(); Prev; Prev = Prev->getPrevNode()) {
    if (auto *IPC = dyn_cast<InstrProfCallsite>(Prev))
      return IPC;
    assert(!isa<CallBase>(Prev) &&
           "didn't expect to find another call, that's not the callsite "
           "instrumentation, before an instrumentable callsite");
  }
  return nullptr;
}

InstrProfIncrementInst *CtxProfAnalysis::getBBInstrumentation(BasicBlock &BB) {
  for (auto &I : BB)
    if (auto *Incr = dyn_cast<InstrProfIncrementInst>(&I))
      if (!isa<InstrProfIncrementInstStep>(&I))
        return Incr;
  return nullptr;
}

InstrProfIncrementInstStep *
CtxProfAnalysis::getSelectInstrumentation(SelectInst &SI) {
  Instruction *Prev = &SI;
  while ((Prev = Prev->getPrevNode()))
    if (auto *Step = dyn_cast<InstrProfIncrementInstStep>(Prev))
      return Step;
  return nullptr;
}

template <class ProfilesTy, class ProfTy>
static void preorderVisit(ProfilesTy &Profiles,
                          function_ref<void(ProfTy &)> Visitor) {
  std::function<void(ProfTy &)> Traverser = [&](auto &Ctx) {
    Visitor(Ctx);
    for (auto &[_, SubCtxSet] : Ctx.callsites())
      for (auto &[__, Subctx] : SubCtxSet)
        Traverser(Subctx);
  };
  for (auto &[_, P] : Profiles)
    Traverser(P);
}

void PGOContextualProfile::initIndex() {
  // Initialize the head of the index list for each function. We don't need it
  // after this point.
  DenseMap<GlobalValue::GUID, PGOCtxProfContext *> InsertionPoints;
  for (auto &[Guid, FI] : FuncInfo)
    InsertionPoints[Guid] = &FI.Index;
  preorderVisit<PGOCtxProfContext::CallTargetMapTy, PGOCtxProfContext>(
      Profiles.Contexts, [&](PGOCtxProfContext &Ctx) {
        auto InsertIt = InsertionPoints.find(Ctx.guid());
        if (InsertIt == InsertionPoints.end())
          return;
        // Insert at the end of the list. Since we traverse in preorder, it
        // means that when we iterate the list from the beginning, we'd
        // encounter the contexts in the order we would have, should we have
        // performed a full preorder traversal.
        InsertIt->second->Next = &Ctx;
        Ctx.Previous = InsertIt->second;
        InsertIt->second = &Ctx;
      });
}

bool PGOContextualProfile::isInSpecializedModule() const {
  return ForceIsInSpecializedModule.getNumOccurrences() > 0
             ? ForceIsInSpecializedModule
             : IsInSpecializedModule;
}

void PGOContextualProfile::update(Visitor V, const Function &F) {
  assert(isFunctionKnown(F));
  GlobalValue::GUID G = getDefinedFunctionGUID(F);
  for (auto *Node = FuncInfo.find(G)->second.Index.Next; Node;
       Node = Node->Next)
    V(*reinterpret_cast<PGOCtxProfContext *>(Node));
}

void PGOContextualProfile::visit(ConstVisitor V, const Function *F) const {
  if (!F)
    return preorderVisit<const PGOCtxProfContext::CallTargetMapTy,
                         const PGOCtxProfContext>(Profiles.Contexts, V);
  assert(isFunctionKnown(*F));
  GlobalValue::GUID G = getDefinedFunctionGUID(*F);
  for (const auto *Node = FuncInfo.find(G)->second.Index.Next; Node;
       Node = Node->Next)
    V(*reinterpret_cast<const PGOCtxProfContext *>(Node));
}

const CtxProfFlatProfile PGOContextualProfile::flatten() const {
  CtxProfFlatProfile Flat;
  auto Accummulate = [](SmallVectorImpl<uint64_t> &Into,
                        const SmallVectorImpl<uint64_t> &From) {
    if (Into.empty())
      Into.resize(From.size());
    assert(Into.size() == From.size() &&
           "All contexts corresponding to a function should have the exact "
           "same number of counters.");
    for (size_t I = 0, E = Into.size(); I < E; ++I)
      Into[I] += From[I];
  };

  preorderVisit<const PGOCtxProfContext::CallTargetMapTy,
                const PGOCtxProfContext>(
      Profiles.Contexts, [&](const PGOCtxProfContext &Ctx) {
        Accummulate(Flat[Ctx.guid()], Ctx.counters());
      });
  for (const auto &[_, RC] : Profiles.Contexts)
    for (const auto &[G, Unh] : RC.getUnhandled())
      Accummulate(Flat[G], Unh);
  for (const auto &[G, FC] : Profiles.FlatProfiles)
    Accummulate(Flat[G], FC);
  return Flat;
}

const CtxProfFlatIndirectCallProfile
PGOContextualProfile::flattenVirtCalls() const {
  CtxProfFlatIndirectCallProfile Ret;
  preorderVisit<const PGOCtxProfContext::CallTargetMapTy,
                const PGOCtxProfContext>(
      Profiles.Contexts, [&](const PGOCtxProfContext &Ctx) {
        auto &Targets = Ret[Ctx.guid()];
        for (const auto &[ID, SubctxSet] : Ctx.callsites())
          for (const auto &Subctx : SubctxSet)
            Targets[ID][Subctx.first] += Subctx.second.getEntrycount();
      });
  return Ret;
}

void CtxProfAnalysis::collectIndirectCallPromotionList(
    CallBase &IC, Result &Profile,
    SetVector<std::pair<CallBase *, Function *>> &Candidates) {
  const auto *Instr = CtxProfAnalysis::getCallsiteInstrumentation(IC);
  if (!Instr)
    return;
  Module &M = *IC.getParent()->getModule();
  const uint32_t CallID = Instr->getIndex()->getZExtValue();
  Profile.visit(
      [&](const PGOCtxProfContext &Ctx) {
        const auto &Targets = Ctx.callsites().find(CallID);
        if (Targets == Ctx.callsites().end())
          return;
        for (const auto &[Guid, _] : Targets->second)
          if (auto Name = Profile.getFunctionName(Guid); !Name.empty())
            if (auto *Target = M.getFunction(Name))
              if (Target->hasFnAttribute(Attribute::AlwaysInline))
                Candidates.insert({&IC, Target});
      },
      IC.getCaller());
}
