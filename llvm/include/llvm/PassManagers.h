//===- llvm/PassManagers.h - Pass Infrastructure classes  -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the LLVM Pass Manager infrastructure. 
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_PASSMANAGERS_H
#define LLVM_PASSMANAGERS_H

#include "llvm/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DenseMap.h"
#include <vector>
#include <map>

//===----------------------------------------------------------------------===//
// Overview:
// The Pass Manager Infrastructure manages passes. It's responsibilities are:
// 
//   o Manage optimization pass execution order
//   o Make required Analysis information available before pass P is run
//   o Release memory occupied by dead passes
//   o If Analysis information is dirtied by a pass then regenerate Analysis 
//     information before it is consumed by another pass.
//
// Pass Manager Infrastructure uses multiple pass managers.  They are
// PassManager, FunctionPassManager, MPPassManager, FPPassManager, BBPassManager.
// This class hierarchy uses multiple inheritance but pass managers do not
// derive from another pass manager.
//
// PassManager and FunctionPassManager are two top-level pass manager that
// represents the external interface of this entire pass manager infrastucture.
//
// Important classes :
//
// [o] class PMTopLevelManager;
//
// Two top level managers, PassManager and FunctionPassManager, derive from 
// PMTopLevelManager. PMTopLevelManager manages information used by top level 
// managers such as last user info.
//
// [o] class PMDataManager;
//
// PMDataManager manages information, e.g. list of available analysis info, 
// used by a pass manager to manage execution order of passes. It also provides
// a place to implement common pass manager APIs. All pass managers derive from
// PMDataManager.
//
// [o] class BBPassManager : public FunctionPass, public PMDataManager;
//
// BBPassManager manages BasicBlockPasses.
//
// [o] class FunctionPassManager;
//
// This is a external interface used by JIT to manage FunctionPasses. This
// interface relies on FunctionPassManagerImpl to do all the tasks.
//
// [o] class FunctionPassManagerImpl : public ModulePass, PMDataManager,
//                                     public PMTopLevelManager;
//
// FunctionPassManagerImpl is a top level manager. It manages FPPassManagers
//
// [o] class FPPassManager : public ModulePass, public PMDataManager;
//
// FPPassManager manages FunctionPasses and BBPassManagers
//
// [o] class MPPassManager : public Pass, public PMDataManager;
//
// MPPassManager manages ModulePasses and FPPassManagers
//
// [o] class PassManager;
//
// This is a external interface used by various tools to manages passes. It
// relies on PassManagerImpl to do all the tasks.
//
// [o] class PassManagerImpl : public Pass, public PMDataManager,
//                             public PMDTopLevelManager
//
// PassManagerImpl is a top level pass manager responsible for managing
// MPPassManagers.
//===----------------------------------------------------------------------===//

#include "llvm/Support/PrettyStackTrace.h"

namespace llvm {
  class Module;
  class Pass;
  class StringRef;
  class Value;
  class Timer;
  class PMDataManager;

// enums for debugging strings
enum PassDebuggingString {
  EXECUTION_MSG, // "Executing Pass '"
  MODIFICATION_MSG, // "' Made Modification '"
  FREEING_MSG, // " Freeing Pass '"
  ON_BASICBLOCK_MSG, // "'  on BasicBlock '" + PassName + "'...\n"
  ON_FUNCTION_MSG, // "' on Function '" + FunctionName + "'...\n"
  ON_MODULE_MSG, // "' on Module '" + ModuleName + "'...\n"
  ON_LOOP_MSG, // " 'on Loop ...\n'"
  ON_CG_MSG // "' on Call Graph ...\n'"
};  

/// PassManagerPrettyStackEntry - This is used to print informative information
/// about what pass is running when/if a stack trace is generated.
class PassManagerPrettyStackEntry : public PrettyStackTraceEntry {
  Pass *P;
  Value *V;
  Module *M;
public:
  explicit PassManagerPrettyStackEntry(Pass *p)
    : P(p), V(0), M(0) {}  // When P is releaseMemory'd.
  PassManagerPrettyStackEntry(Pass *p, Value &v)
    : P(p), V(&v), M(0) {} // When P is run on V
  PassManagerPrettyStackEntry(Pass *p, Module &m)
    : P(p), V(0), M(&m) {} // When P is run on M
  
  /// print - Emit information about this stack frame to OS.
  virtual void print(raw_ostream &OS) const;
};
  
  
//===----------------------------------------------------------------------===//
// PMStack
//
/// PMStack - This class implements a stack data structure of PMDataManager
/// pointers.
///
/// Top level pass managers (see PassManager.cpp) maintain active Pass Managers 
/// using PMStack. Each Pass implements assignPassManager() to connect itself
/// with appropriate manager. assignPassManager() walks PMStack to find
/// suitable manager.
class PMStack {
public:
  typedef std::vector<PMDataManager *>::const_reverse_iterator iterator;
  iterator begin() const { return S.rbegin(); }
  iterator end() const { return S.rend(); }

  void pop();
  PMDataManager *top() const { return S.back(); }
  void push(PMDataManager *PM);
  bool empty() const { return S.empty(); }

  void dump() const;

private:
  std::vector<PMDataManager *> S;
};


//===----------------------------------------------------------------------===//
// PMTopLevelManager
//
/// PMTopLevelManager manages LastUser info and collects common APIs used by
/// top level pass managers.
class PMTopLevelManager {
public:

  virtual unsigned getNumContainedManagers() const {
    return (unsigned)PassManagers.size();
  }

  /// Schedule pass P for execution. Make sure that passes required by
  /// P are run before P is run. Update analysis info maintained by
  /// the manager. Remove dead passes. This is a recursive function.
  void schedulePass(Pass *P);

  /// This is implemented by top level pass manager and used by 
  /// schedulePass() to add analysis info passes that are not available.
  virtual void addTopLevelPass(Pass  *P) = 0;

  /// Set pass P as the last user of the given analysis passes.
  void setLastUser(SmallVector<Pass *, 12> &AnalysisPasses, Pass *P);

  /// Collect passes whose last user is P
  void collectLastUses(SmallVector<Pass *, 12> &LastUses, Pass *P);

  /// Find the pass that implements Analysis AID. Search immutable
  /// passes and all pass managers. If desired pass is not found
  /// then return NULL.
  Pass *findAnalysisPass(AnalysisID AID);

  /// Find analysis usage information for the pass P.
  AnalysisUsage *findAnalysisUsage(Pass *P);

  explicit PMTopLevelManager(PMDataManager *PMDM);
  virtual ~PMTopLevelManager(); 

  /// Add immutable pass and initialize it.
  inline void addImmutablePass(ImmutablePass *P) {
    P->initializePass();
    ImmutablePasses.push_back(P);
  }

  inline SmallVector<ImmutablePass *, 8>& getImmutablePasses() {
    return ImmutablePasses;
  }

  void addPassManager(PMDataManager *Manager) {
    PassManagers.push_back(Manager);
  }

  // Add Manager into the list of managers that are not directly
  // maintained by this top level pass manager
  inline void addIndirectPassManager(PMDataManager *Manager) {
    IndirectPassManagers.push_back(Manager);
  }

  // Print passes managed by this top level manager.
  void dumpPasses() const;
  void dumpArguments() const;

  void initializeAllAnalysisInfo();

  // Active Pass Managers
  PMStack activeStack;

protected:
  
  /// Collection of pass managers
  SmallVector<PMDataManager *, 8> PassManagers;

private:

  /// Collection of pass managers that are not directly maintained
  /// by this pass manager
  SmallVector<PMDataManager *, 8> IndirectPassManagers;

  // Map to keep track of last user of the analysis pass.
  // LastUser->second is the last user of Lastuser->first.
  DenseMap<Pass *, Pass *> LastUser;

  // Map to keep track of passes that are last used by a pass.
  // This inverse map is initialized at PM->run() based on
  // LastUser map.
  DenseMap<Pass *, SmallPtrSet<Pass *, 8> > InversedLastUser;

  /// Immutable passes are managed by top level manager.
  SmallVector<ImmutablePass *, 8> ImmutablePasses;

  DenseMap<Pass *, AnalysisUsage *> AnUsageMap;
};


  
//===----------------------------------------------------------------------===//
// PMDataManager

/// PMDataManager provides the common place to manage the analysis data
/// used by pass managers.
class PMDataManager {
public:

  explicit PMDataManager(int Depth) : TPM(NULL), Depth(Depth) {
    initializeAnalysisInfo();
  }

  virtual ~PMDataManager();
  
  virtual Pass *getAsPass() = 0;

  /// Augment AvailableAnalysis by adding analysis made available by pass P.
  void recordAvailableAnalysis(Pass *P);

  /// verifyPreservedAnalysis -- Verify analysis presreved by pass P.
  void verifyPreservedAnalysis(Pass *P);

  /// Remove Analysis that is not preserved by the pass
  void removeNotPreservedAnalysis(Pass *P);
  
  /// Remove dead passes used by P.
  void removeDeadPasses(Pass *P, StringRef Msg, 
                        enum PassDebuggingString);

  /// Remove P.
  void freePass(Pass *P, StringRef Msg, 
                enum PassDebuggingString);

  /// Add pass P into the PassVector. Update 
  /// AvailableAnalysis appropriately if ProcessAnalysis is true.
  void add(Pass *P, bool ProcessAnalysis = true);

  /// Add RequiredPass into list of lower level passes required by pass P.
  /// RequiredPass is run on the fly by Pass Manager when P requests it
  /// through getAnalysis interface.
  virtual void addLowerLevelRequiredPass(Pass *P, Pass *RequiredPass);

  virtual Pass *getOnTheFlyPass(Pass *P, AnalysisID PI, Function &F);

  /// Initialize available analysis information.
  void initializeAnalysisInfo() { 
    AvailableAnalysis.clear();
    for (unsigned i = 0; i < PMT_Last; ++i)
      InheritedAnalysis[i] = NULL;
  }

  // Return true if P preserves high level analysis used by other
  // passes that are managed by this manager.
  bool preserveHigherLevelAnalysis(Pass *P);


  /// Populate RequiredPasses with analysis pass that are required by
  /// pass P and are available. Populate ReqPassNotAvailable with analysis
  /// pass that are required by pass P but are not available.
  void collectRequiredAnalysis(SmallVector<Pass *, 8> &RequiredPasses,
                               SmallVector<AnalysisID, 8> &ReqPassNotAvailable,
                               Pass *P);

  /// All Required analyses should be available to the pass as it runs!  Here
  /// we fill in the AnalysisImpls member of the pass so that it can
  /// successfully use the getAnalysis() method to retrieve the
  /// implementations it needs.
  void initializeAnalysisImpl(Pass *P);

  /// Find the pass that implements Analysis AID. If desired pass is not found
  /// then return NULL.
  Pass *findAnalysisPass(AnalysisID AID, bool Direction);

  // Access toplevel manager
  PMTopLevelManager *getTopLevelManager() { return TPM; }
  void setTopLevelManager(PMTopLevelManager *T) { TPM = T; }

  unsigned getDepth() const { return Depth; }

  // Print routines used by debug-pass
  void dumpLastUses(Pass *P, unsigned Offset) const;
  void dumpPassArguments() const;
  void dumpPassInfo(Pass *P, enum PassDebuggingString S1,
                    enum PassDebuggingString S2, StringRef Msg);
  void dumpRequiredSet(const Pass *P) const;
  void dumpPreservedSet(const Pass *P) const;

  virtual unsigned getNumContainedPasses() const {
    return (unsigned)PassVector.size();
  }

  virtual PassManagerType getPassManagerType() const { 
    assert ( 0 && "Invalid use of getPassManagerType");
    return PMT_Unknown; 
  }

  std::map<AnalysisID, Pass*> *getAvailableAnalysis() {
    return &AvailableAnalysis;
  }

  // Collect AvailableAnalysis from all the active Pass Managers.
  void populateInheritedAnalysis(PMStack &PMS) {
    unsigned Index = 0;
    for (PMStack::iterator I = PMS.begin(), E = PMS.end();
         I != E; ++I)
      InheritedAnalysis[Index++] = (*I)->getAvailableAnalysis();
  }

protected:

  // Top level manager.
  PMTopLevelManager *TPM;

  // Collection of pass that are managed by this manager
  SmallVector<Pass *, 16> PassVector;

  // Collection of Analysis provided by Parent pass manager and
  // used by current pass manager. At at time there can not be more
  // then PMT_Last active pass mangers.
  std::map<AnalysisID, Pass *> *InheritedAnalysis[PMT_Last];

  
  /// isPassDebuggingExecutionsOrMore - Return true if -debug-pass=Executions
  /// or higher is specified.
  bool isPassDebuggingExecutionsOrMore() const;
  
private:
  void dumpAnalysisUsage(StringRef Msg, const Pass *P,
                         const AnalysisUsage::VectorType &Set) const;

  // Set of available Analysis. This information is used while scheduling 
  // pass. If a pass requires an analysis which is not available then 
  // the required analysis pass is scheduled to run before the pass itself is
  // scheduled to run.
  std::map<AnalysisID, Pass*> AvailableAnalysis;

  // Collection of higher level analysis used by the pass managed by
  // this manager.
  SmallVector<Pass *, 8> HigherLevelAnalysis;

  unsigned Depth;
};

//===----------------------------------------------------------------------===//
// FPPassManager
//
/// FPPassManager manages BBPassManagers and FunctionPasses.
/// It batches all function passes and basic block pass managers together and 
/// sequence them to process one function at a time before processing next 
/// function.
class FPPassManager : public ModulePass, public PMDataManager {
public:
  static char ID;
  explicit FPPassManager(int Depth) 
  : ModulePass(ID), PMDataManager(Depth) { }
  
  /// run - Execute all of the passes scheduled for execution.  Keep track of
  /// whether any of the passes modifies the module, and if so, return true.
  bool runOnFunction(Function &F);
  bool runOnModule(Module &M);
  
  /// cleanup - After running all passes, clean up pass manager cache.
  void cleanup();

  /// doInitialization - Run all of the initializers for the function passes.
  ///
  bool doInitialization(Module &M);
  
  /// doFinalization - Run all of the finalizers for the function passes.
  ///
  bool doFinalization(Module &M);

  virtual PMDataManager *getAsPMDataManager() { return this; }
  virtual Pass *getAsPass() { return this; }

  /// Pass Manager itself does not invalidate any analysis info.
  void getAnalysisUsage(AnalysisUsage &Info) const {
    Info.setPreservesAll();
  }

  // Print passes managed by this manager
  void dumpPassStructure(unsigned Offset);

  virtual const char *getPassName() const {
    return "Function Pass Manager";
  }

  FunctionPass *getContainedPass(unsigned N) {
    assert ( N < PassVector.size() && "Pass number out of range!");
    FunctionPass *FP = static_cast<FunctionPass *>(PassVector[N]);
    return FP;
  }

  virtual PassManagerType getPassManagerType() const { 
    return PMT_FunctionPassManager; 
  }
};

Timer *getPassTimer(Pass *);

}

#endif
