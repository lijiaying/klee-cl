/*******************************************************************************
 * Copyright (C) 2010 Dependable Systems Laboratory, EPFL
 *
 * This file is part of the Cloud9-specific extensions to the KLEE symbolic
 * execution engine.
 *
 * Do NOT distribute this file to any third party; it is part of
 * unpublished work.
 *
 ******************************************************************************/

//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Common.h"

#include "Executor.h"
 
#include "Context.h"
#include "CoreStats.h"
#include "ExternalDispatcher.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"
#include "../Solver/SolverStats.h"

#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprSMTLIBLetPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/GetElementPtrTypeIterator.h"
#include "klee/Config/Version.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/System/Time.h"

#include "llvm/Attributes.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(2, 7)
#include "llvm/LLVMContext.h"
#endif
#include "llvm/Module.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 9)
#include "llvm/System/Process.h"
#else
#include "llvm/Support/Process.h"
#endif
#include "llvm/Target/TargetData.h"

#include <cassert>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include <sys/mman.h>

#include <errno.h>
#include <cxxabi.h>

//using namespace llvm;
using namespace klee;

namespace {
  cl::opt<bool>
  DumpStatesOnHalt("dump-states-on-halt",
                   cl::init(true));
 
  cl::opt<bool>
  NoPreferCex("no-prefer-cex",
              cl::init(false));
 
  cl::opt<bool>
  UseAsmAddresses("use-asm-addresses",
                  cl::init(false));
 
  cl::opt<bool>
  RandomizeFork("randomize-fork",
                cl::init(false));
 
  cl::opt<bool>
  AllowExternalSymCalls("allow-external-sym-calls",
                        cl::init(false));

  cl::opt<bool>
  DebugPrintInstructions("debug-print-instructions", 
                         cl::desc("Print instructions during execution."));

  cl::opt<bool>
  DebugCheckForImpliedValues("debug-check-for-implied-values");


  cl::opt<bool>
  SimplifySymIndices("simplify-sym-indices",
                     cl::init(false));

  cl::opt<unsigned>
  MaxSymArraySize("max-sym-array-size",
                  cl::init(0));

  cl::opt<bool>
  DebugValidateSolver("debug-validate-solver",
		      cl::init(false));

  cl::opt<bool>
  SuppressExternalWarnings("suppress-external-warnings");

  cl::opt<bool>
  AllExternalWarnings("all-external-warnings");

  cl::opt<bool>
  OnlyOutputStatesCoveringNew("only-output-states-covering-new",
                              cl::init(false));

  cl::opt<bool>
  AlwaysOutputSeeds("always-output-seeds",
                              cl::init(true));

  cl::opt<bool>
  UseFastCexSolver("use-fast-cex-solver",
		   cl::init(false));

  cl::opt<bool>
  UseIndependentSolver("use-independent-solver",
                       cl::init(true),
		       cl::desc("Use constraint independence"));

  cl::opt<bool>
  EmitAllErrors("emit-all-errors",
                cl::init(false),
                cl::desc("Generate tests cases for all errors "
                         "(default=one per (error,instruction) pair)"));

  cl::opt<bool>
  UseCexCache("use-cex-cache",
              cl::init(true),
	      cl::desc("Use counterexample caching"));

  cl::opt<bool>
  UseFPRewriter("use-fp-rewriter",
                cl::init(false));

  // FIXME: Command line argument duplicated in main.cpp of Kleaver
  cl::opt<int>
  MinQueryTimeToLog("min-query-time-to-log",
                    cl::init(0),
                    cl::value_desc("milliseconds"),
                    cl::desc("Set time threshold (in ms) for queries logged in files. "
                             "Only queries longer than threshold will be logged. (default=0). "
                             "Set this param to a negative value to log timeouts only."));
   
  cl::opt<bool>
  NoExternals("no-externals", 
           cl::desc("Do not allow external functin calls"));

  cl::opt<bool>
  UseCache("use-cache",
	   cl::init(true),
	   cl::desc("Use validity caching"));

  cl::opt<bool>
  OnlyReplaySeeds("only-replay-seeds", 
                  cl::desc("Discard states that do not have a seed."));
 
  cl::opt<bool>
  OnlySeed("only-seed", 
           cl::desc("Stop execution after seeding is done without doing regular search."));
 
  cl::opt<bool>
  AllowSeedExtension("allow-seed-extension", 
                     cl::desc("Allow extra (unbound) values to become symbolic during seeding."));
 
  cl::opt<bool>
  ZeroSeedExtension("zero-seed-extension");
 
  cl::opt<bool>
  AllowSeedTruncation("allow-seed-truncation", 
                      cl::desc("Allow smaller buffers than in seeds."));
 
  cl::opt<bool>
  NamedSeedMatching("named-seed-matching",
                    cl::desc("Use names to match symbolic objects to inputs."));

  cl::opt<double>
  MaxStaticForkPct("max-static-fork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticSolvePct("max-static-solve-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPForkPct("max-static-cpfork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPSolvePct("max-static-cpsolve-pct", cl::init(1.));

  cl::opt<double>
  MaxInstructionTime("max-instruction-time",
                     cl::desc("Only allow a single instruction to take this much time (default=0 (off))"),
                     cl::init(0));
  
  cl::opt<double>
  SeedTime("seed-time",
           cl::desc("Amount of time to dedicate to seeds, before normal search (default=0 (off))"),
           cl::init(0));
  
  cl::opt<double>
  MaxSTPTime("max-stp-time",
             cl::desc("Maximum amount of time for a single query (default=120s)"),
             cl::init(120.0));
  
  cl::opt<unsigned int>
  StopAfterNInstructions("stop-after-n-instructions",
                         cl::desc("Stop execution after specified number of instructions (0=off)"),
                         cl::init(0));
  
  cl::opt<unsigned>
  MaxForks("max-forks",
           cl::desc("Only fork this many times (-1=off)"),
           cl::init(~0u));
  
  cl::opt<unsigned>
  MaxDepth("max-depth",
           cl::desc("Only allow this many symbolic branches (0=off)"),
           cl::init(0));
  
  cl::opt<unsigned>
  MaxMemory("max-memory",
            cl::desc("Refuse to fork when more above this about of memory (in MB, 0=off)"),
            cl::init(0));

  cl::opt<bool>
  MaxMemoryInhibit("max-memory-inhibit",
            cl::desc("Inhibit forking at memory cap (vs. random terminate)"),
            cl::init(true));

  cl::opt<bool>
  UseForkedSTP("use-forked-stp", 
                 cl::desc("Run STP in forked process"));

  cl::opt<bool>
  STPOptimizeDivides("stp-optimize-divides", 
                 cl::desc("Optimize constant divides into add/shift/multiplies before passing to STP"),
                 cl::init(true));

  cl::opt<unsigned int>
  MaxPreemptions("scheduler-preemption-bound",
		 cl::desc("scheduler preemption bound (default=0)"),
		 cl::init(0));
  
  cl::opt<bool>
  ForkOnSchedule("fork-on-schedule", 
		 cl::desc("fork when various schedules are possible (defaul=disabled)"),
		 cl::init(false));

  /* Using cl::list<> instead of cl::bits<> results in quite a bit of ugliness when it comes to checking
   * if an option is set. Unfortunately with gcc4.7 cl::bits<> is broken with LLVM2.9 and I doubt everyone
   * wants to patch their copy of LLVM just for these options.
   */		
  cl::list<klee::QueryLoggingSolver> queryLoggingOptions("use-query-log",
		  cl::desc("Log queries to a file. Multiple options can be specified seperate by a comma. By default nothing is logged."),
		  cl::values(
					  clEnumValN(klee::ALL_PC,"all:pc","All queries in .pc (KQuery) format"),
					  clEnumValN(klee::ALL_SMTLIB,"all:smt2","All queries in .smt2 (SMT-LIBv2) format"),
					  clEnumValN(klee::SOLVER_PC,"solver:pc","All queries reaching the solver in .pc (KQuery) format"),
					  clEnumValN(klee::SOLVER_SMTLIB,"solver:smt2","All queries reaching the solver in .pc (SMT-LIBv2) format"),
					  clEnumValEnd
		             ), cl::CommaSeparated
  );


}


namespace klee {
  RNG theRNG;

  //A bit of ugliness so we can use cl::list<> like cl::bits<>, see queryLoggingOptions
  template <typename T>
  static bool optionIsSet(cl::list<T> list, T option)
  {
	  return std::find(list.begin(), list.end(), option) != list.end();
  }


}

Solver *constructSolverChain(STPSolver *stpSolver,
                             std::string querySMT2LogPath,
                             std::string baseSolverQuerySMT2LogPath,
                             std::string queryPCLogPath,
                             std::string baseSolverQueryPCLogPath) {
  Solver *solver = stpSolver;

  

  if (optionIsSet(queryLoggingOptions,SOLVER_PC))
  {
    solver = createPCLoggingSolver(solver, 
                                   baseSolverQueryPCLogPath,
		                   MinQueryTimeToLog);
    klee_message("Logging queries that reach solver in .pc format to %s",baseSolverQueryPCLogPath.c_str());
  }

  if (optionIsSet(queryLoggingOptions,SOLVER_SMTLIB))
  {
    solver = createSMTLIBLoggingSolver(solver,baseSolverQuerySMT2LogPath,
                                       MinQueryTimeToLog);
    klee_message("Logging queries that reach solver in .smt2 format to %s",baseSolverQuerySMT2LogPath.c_str());
  }

  if (UseFPRewriter)
    solver = createFPRewritingSolver(solver);
  
  if (UseFastCexSolver)
    solver = createFastCexSolver(solver);

  if (UseCexCache)
    solver = createCexCachingSolver(solver);

  if (UseCache)
    solver = createCachingSolver(solver);

  if (UseIndependentSolver)
    solver = createIndependentSolver(solver);

  if (DebugValidateSolver)
    solver = createValidatingSolver(solver, stpSolver);

  if (optionIsSet(queryLoggingOptions,ALL_PC))
  {
    solver = createPCLoggingSolver(solver, 
                                   queryPCLogPath,
                                   MinQueryTimeToLog);
    klee_message("Logging all queries in .pc format to %s",queryPCLogPath.c_str());
  }
  
  if (optionIsSet(queryLoggingOptions,ALL_SMTLIB))
  {
    solver = createSMTLIBLoggingSolver(solver,querySMT2LogPath,
                                       MinQueryTimeToLog);
    klee_message("Logging all queries in .smt2 format to %s",querySMT2LogPath.c_str());
  }

  return solver;
}

static const fltSemantics *TypeToFloatSemantics(const Type *Ty) {
  if (Ty == Type::getFloatTy(Ty->getContext()))
    return &APFloat::IEEEsingle;
  if (Ty == Type::getDoubleTy(Ty->getContext()))
    return &APFloat::IEEEdouble;
  if (Ty == Type::getX86_FP80Ty(Ty->getContext()))
    return &APFloat::x87DoubleExtended;
  else if (Ty == Type::getFP128Ty(Ty->getContext()))
    return &APFloat::IEEEquad;
  
  assert(Ty == Type::getPPC_FP128Ty(Ty->getContext()) && "Unknown FP format");
  return &APFloat::PPCDoubleDouble;
}

namespace {

class SIMDOperation {
public:
  const Executor *Exec;
  KModule *kmodule;
  SIMDOperation(const Executor *Exec, KModule *kmodule) : Exec(Exec), kmodule(kmodule) {}

  virtual ref<Expr> evalOne(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *ft, ref<Expr> l, ref<Expr> r) = 0;

  ref<Expr> eval(LLVM_TYPE_Q Type *t, ref<Expr> src) {
    return eval(t, t, src);
  }

  ref<Expr> eval(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *ft, ref<Expr> src) {
    unsigned Bits = Exec->getWidthForLLVMType(kmodule, ft);
    return eval(tt, ft, src, klee::ConstantExpr::create(0, Bits));
  }

  ref<Expr> eval(LLVM_TYPE_Q Type *t, ref<Expr> l, ref<Expr> r) {
    return eval(t, t, l, r);
  }

  ref<Expr> eval(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *ft, ref<Expr> l, ref<Expr> r) {
    if (LLVM_TYPE_Q VectorType *vft = dyn_cast<VectorType>(ft)) {
      assert(isa<VectorType>(tt));
      LLVM_TYPE_Q VectorType *vtt = cast<VectorType>(tt);

      LLVM_TYPE_Q Type *fElTy = vft->getElementType();
      LLVM_TYPE_Q Type *tElTy = vtt->getElementType();
      unsigned EltBits = Exec->getWidthForLLVMType(kmodule, fElTy);
   
      unsigned ElemCount = vft->getNumElements();
      assert(vtt->getNumElements() == ElemCount);
      ref<Expr> *elems = new ref<Expr>[vft->getNumElements()];
      for (unsigned i = 0; i < ElemCount; ++i)
        elems[i] = evalOne(tElTy, fElTy,
                           ExtractExpr::create(l, EltBits*(ElemCount-i-1), EltBits),
                           ExtractExpr::create(r, EltBits*(ElemCount-i-1), EltBits));
   
      ref<Expr> Result = ConcatExpr::createN(ElemCount, elems);
      delete[] elems;
      return Result;
    } else
      return evalOne(tt, ft, l, r);
  }
};

class ISIMDOperation : public SIMDOperation {
public:
  typedef ref<Expr> (*ExprCtor)(const ref<Expr> &l, const ref<Expr> &r);
  ExprCtor Ctor;

  ISIMDOperation(const Executor *Exec, KModule *kmodule, ExprCtor Ctor) : SIMDOperation(Exec, kmodule), Ctor(Ctor) {}

  ref<Expr> evalOne(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *t, ref<Expr> l, ref<Expr> r) {
    return Ctor(l, r);
  }
};

class FSIMDOperation : public SIMDOperation {
public:
  typedef ref<Expr> (*ExprCtor)(const ref<Expr> &l, const ref<Expr> &r, bool isIEEE);
  ExprCtor Ctor;

  FSIMDOperation(const Executor *Exec, KModule *kmodule, ExprCtor Ctor) : SIMDOperation(Exec, kmodule), Ctor(Ctor) {}

  ref<Expr> evalOne(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *t, ref<Expr> l, ref<Expr> r) {
    return Ctor(l, r, t->isFP128Ty());
  }
};

class FCmpSIMDOperation : public SIMDOperation {
public:
  FCmpSIMDOperation(const Executor *Exec, KModule *kmodule, FCmpInst::Predicate pred) : SIMDOperation(Exec, kmodule), pred(klee::ConstantExpr::create(pred, 4)) {}
  ref<klee::ConstantExpr> pred;

  ref<Expr> evalOne(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *t, ref<Expr> l, ref<Expr> r) {
    return FCmpExpr::create(l, r, pred, t->isFP128Ty());
  }
};

class FUnSIMDOperation : public SIMDOperation {
public:
  typedef ref<Expr> (*ExprCtor)(const ref<Expr> &src, bool isIEEE);
  ExprCtor Ctor;

  FUnSIMDOperation(const Executor *Exec, KModule *kmodule, ExprCtor Ctor) : SIMDOperation(Exec, kmodule), Ctor(Ctor) {}

  ref<Expr> evalOne(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *t, ref<Expr> l, ref<Expr> r) {
    return Ctor(l, t->isFP128Ty());
  }
};

class I2FSIMDOperation : public SIMDOperation {
public:
  typedef ref<Expr> (*ExprCtor)(const ref<Expr> &src, const fltSemantics *sem);
  ExprCtor Ctor;

  I2FSIMDOperation(const Executor *Exec, KModule *kmodule, ExprCtor Ctor) : SIMDOperation(Exec, kmodule), Ctor(Ctor) {}

  ref<Expr> evalOne(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *t, ref<Expr> l, ref<Expr> r) {
    return Ctor(l, TypeToFloatSemantics(t));
  }
};

class F2ISIMDOperation : public SIMDOperation {
public:
  typedef ref<Expr> (*ExprCtor)(const ref<Expr> &e, Expr::Width W, bool isIEEE, bool roundNearest);
  ExprCtor Ctor;
  bool RoundNearest;

  F2ISIMDOperation(const Executor *Exec, KModule *kmodule, ExprCtor Ctor, bool RoundNearest) : SIMDOperation(Exec, kmodule), Ctor(Ctor), RoundNearest(RoundNearest) {}

  ref<Expr> evalOne(LLVM_TYPE_Q Type *tt, LLVM_TYPE_Q Type *ft, ref<Expr> l, ref<Expr> r) {
    return Ctor(l, Exec->getWidthForLLVMType(kmodule, tt), ft->isFP128Ty(), RoundNearest);
  }
};

}
//namespace klee {

Executor::Executor(const InterpreterOptions &opts,
                   InterpreterHandler *ih) 
  : Interpreter(opts),
    interpreterHandler(ih),
    searcher(0),
    externalDispatcher(new ExternalDispatcher()),
    statsTracker(0),
    pathWriter(0),
    symPathWriter(0),
    specialFunctionHandler(0),
    processTree(0),
    replayOut(0),
    replayPath(0),    
    usingSeeds(0),
    atMemoryLimit(false),
    inhibitForking(false),
    haltExecution(false),
    ivcEnabled(false),
    stpTimeout(MaxSTPTime != 0 && MaxInstructionTime != 0
	       ? std::min(MaxSTPTime,MaxInstructionTime)
	       : std::max(MaxSTPTime,MaxInstructionTime)) {

  STPSolver *stpSolver = new STPSolver(UseForkedSTP, STPOptimizeDivides);
  Solver *solver = 
    constructSolverChain(stpSolver,
                         interpreterHandler->getOutputFilename("all-queries.smt2"),
                         interpreterHandler->getOutputFilename("solver-queries.smt2"),
                         interpreterHandler->getOutputFilename("all-queries.pc"),
                         interpreterHandler->getOutputFilename("solver-queries.pc"));
  
  this->solver = new TimingSolver(solver, stpSolver);

  memory = new MemoryManager();

}


unsigned Executor::addModule(llvm::Module *module, 
                             const ModuleOptions &opts) {
  KModule *kmodule = new KModule(module);
  kmodules.push_back(kmodule);

  // Initialize the context.
  TargetData *TD = kmodule->targetData;
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width) TD->getPointerSizeInBits());

  if (!specialFunctionHandler)
    specialFunctionHandler = new SpecialFunctionHandler(*this);

  specialFunctionHandler->prepare(kmodule);
  kmodule->prepare(opts, interpreterHandler, infos);
  specialFunctionHandler->bind(kmodule);

  if (StatsTracker::useStatistics()) {
    if (!statsTracker)
      statsTracker = 
        new StatsTracker(*this,
                         interpreterHandler->getOutputFilename("assembly.ll"),
                         userSearcherRequiresMD2U());

    statsTracker->addModule(kmodule);
  }
  
  return kmodules.size()-1;
}

KModule *Executor::kmodule(const ExecutionState &state) const {
  return kmodules[state.stack().back().moduleId];
}

Executor::~Executor() {
  delete memory;
  delete externalDispatcher;
  if (processTree)
    delete processTree;
  if (specialFunctionHandler)
    delete specialFunctionHandler;
  if (statsTracker)
    delete statsTracker;
  delete solver;
  for (std::vector<KModule*>::iterator i=kmodules.begin(), e=kmodules.end();
       i != e; ++i)
    delete *i;
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      const Constant *c, 
                                      unsigned offset) {
  TargetData *targetData = kmodule(state)->targetData;
  if (const ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i=0, e=cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i), 
			     offset + i*elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i=0; i<size; i++)
      os->write8(offset+i, (uint8_t) 0);
  } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i=0, e=ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i), 
			     offset + i*elementSize);
  } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
      targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i=0, e=cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i), 
			     offset + sl->getElementOffset(i));
  } else {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    ref<ConstantExpr> C = evalConstant(kmodule(state), c);

    // Extend the constant if necessary;
    assert(StoreBits >= C->getWidth() && "Invalid store size!");
    if (StoreBits > C->getWidth())
      C = C->ZExt(StoreBits);

    os->write(offset, C);
  }
}

MemoryObject * Executor::addExternalObject(ExecutionState &state, 
                                           void *addr, unsigned size, 
                                           bool isReadOnly) {
  MemoryObject *mo = memory->allocateFixed((uint64_t) (unsigned long) addr, 
                                           size, 0);
  ObjectState *os = bindObjectInState(state, 0, mo, false);
  for(unsigned i = 0; i < size; i++)
    os->write8(i, ((uint8_t*)addr)[i]);
  if(isReadOnly)
    os->setReadOnly(true);  
  return mo;
}


extern void *__dso_handle __attribute__ ((__weak__));

void Executor::initializeGlobals(ExecutionState &state) {
  initializeGlobals(state, kmodule(state)->module);
}

void Executor::initializeGlobals(ExecutionState &state, unsigned moduleId) {
  initializeGlobals(state, kmodules[moduleId]->module);
}

void Executor::initializeGlobals(ExecutionState &state, Module *m) {
  if (m->getModuleInlineAsm() != "")
    klee_warning("executable has module level assembly (ignoring)");

  //assert(m->lib_begin() == m->lib_end() &&
  //       "XXX do not support dependent libraries");

  // represent function globals using the address of the actual llvm function
  // object. given that we use malloc to allocate memory in states this also
  // ensures that we won't conflict. we don't need to allocate a memory object
  // since reading/writing via a function pointer is unsupported anyway.
  for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
    Function *f = i;
    ref<ConstantExpr> addr(0);

    // If the symbol has external weak linkage then it is implicitly
    // not defined in this module; if it isn't resolvable then it
    // should be null.
    if (f->hasExternalWeakLinkage() && 
        !externalDispatcher->resolveSymbol(f->getName())) {
      addr = Expr::createPointer(0);
    } else {
      addr = Expr::createPointer((unsigned long) (void*) f);
      legalFunctions.insert((uint64_t) (unsigned long) (void*) f);
    }
    
    globalAddresses.insert(std::make_pair(f, addr));
  }

  // Disabled, we don't want to promote use of live externals.

  // allocate and initialize globals, done in two passes since we may
  // need address of a global in order to initialize some other one.

  // allocate memory objects for all globals
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->isDeclaration()) {
      // FIXME: We have no general way of handling unknown external
      // symbols. If we really cared about making external stuff work
      // better we could support user definition, or use the EXE style
      // hack where we check the object file information.

      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      unsigned addrspace = i->getType()->getAddressSpace();
      uint64_t size = kmodule(state)->targetData->getTypeStoreSize(ty);

      // XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
      if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
        size = 0x2C;
      }
#endif

      if (size == 0) {
        llvm::errs() << "Unable to find size for global variable: " 
                     << i->getName() 
                     << " (use will result in out of bounds access)\n";
      }

      MemoryObject *mo = memory->allocate(&state, size, false, true, i);
      ObjectState *os = bindObjectInState(state, addrspace, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      // Program already running = object already initialized.  Read
      // concrete value and write it to our copy.
      if (size) {
        void *addr;
        if (i->getName() == "__dso_handle") {
          addr = &__dso_handle; // wtf ?
        } else {
          addr = externalDispatcher->resolveSymbol(i->getName());
        }
        if (!addr)
          klee_error("unable to load symbol(%s) while initializing globals.", 
                     i->getName().data());

        for (unsigned offset=0; offset<mo->size; offset++)
          os->write8(offset, ((unsigned char*)addr)[offset]);
      }
    } else {
      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      unsigned addrspace = i->getType()->getAddressSpace();
      uint64_t size = kmodule(state)->targetData->getTypeStoreSize(ty);
      MemoryObject *mo = 0;

      if (UseAsmAddresses && i->getName()[0]=='\01') {
        char *end;
        uint64_t address = ::strtoll(i->getName().str().c_str()+1, &end, 0);

        if (end && *end == '\0') {
          klee_message("NOTE: allocated global at asm specified address: %#08llx"
                       " (%llu bytes)",
                       (long long) address, (unsigned long long) size);
          mo = memory->allocateFixed(address, size, &*i);
          mo->isUserSpecified = true; // XXX hack;
        }
      }

      if (!mo)
        mo = memory->allocate(&state, size, false, true, &*i);
      if(!mo)
	klee_message("cannot allocate memory for global %s",
                     i->getName().str().c_str());
      assert(mo && "out of memory");
      ObjectState *os = bindObjectInState(state, addrspace, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      if (!i->hasInitializer())
          os->initializeToRandom();
    }
  }
  
  // link aliases to their definitions (if bound)
  for (Module::alias_iterator i = m->alias_begin(), ie = m->alias_end(); 
       i != ie; ++i) {
    // Map the alias to its aliasee's address. This works because we have
    // addresses for everything, even undefined functions. 
    globalAddresses.insert(std::make_pair(i, evalConstant(kmodule(state), i->getAliasee())));
  }

  // once all objects are allocated, do the actual initialization
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->hasInitializer()) {
      unsigned addrspace = i->getType()->getAddressSpace();
      MemoryObject *mo = globalObjects.find(i)->second;
      const ObjectState *os = state.addressSpace(addrspace).findObject(mo);
      assert(os);
      ObjectState *wos = state.addressSpace(addrspace).getWriteable(mo, os);
      
      initializeGlobalObject(state, wos, i->getInitializer(), 0);
      // if(i->isConstant()) os->setReadOnly(true);
    }
  }
}

// TODO: merge with Executor::initializeGlobals?
void Executor::bindGlobalsInNewAddressSpace(ExecutionState &state, unsigned addrspace, AddressSpace &as) {
  for (std::vector<KModule*>::iterator mi = kmodules.begin(),
       me = kmodules.end(); mi != me; ++mi) {
    Module *m = (*mi)->module;
    for (Module::const_global_iterator i = m->global_begin(),
           e = m->global_end();
         i != e; ++i) {
      unsigned objAS = i->getType()->getAddressSpace();
      if (addrspace != objAS) continue;
   
      MemoryObject *mo = globalObjects.find(i)->second;
      ObjectState *os = new ObjectState(mo);
      as.bindObject(mo, os);
   
      if (i->hasInitializer())
        initializeGlobalObject(state, os, i->getInitializer(), 0);
    }
  }
}

void Executor::initializeExternals(ExecutionState &state) {
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  int *errno_addr = __errno_location();
  addExternalObject(state, (void *)errno_addr, sizeof *errno_addr, false);

  /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
  const uint16_t **addr = __ctype_b_loc();
  addExternalObject(state, (void *)(*addr-128), 
                    384 * sizeof **addr, true);
  addExternalObject(state, addr, sizeof(*addr), true);
    
  const int32_t **lower_addr = __ctype_tolower_loc();
  addExternalObject(state, (void *)(*lower_addr-128), 
                    384 * sizeof **lower_addr, true);
  addExternalObject(state, lower_addr, sizeof(*lower_addr), true);
  
  const int32_t **upper_addr = __ctype_toupper_loc();
  addExternalObject(state, (void *)(*upper_addr-128), 
                    384 * sizeof **upper_addr, true);
  addExternalObject(state, upper_addr, sizeof(*upper_addr), true);
#endif
#endif
#endif
}

void Executor::branch(ExecutionState &state, 
                      const std::vector< ref<Expr> > &conditions,
                      std::vector<ExecutionState*> &result, int reason) {
  TimerStatIncrementer timer(stats::forkTime);
  unsigned N = conditions.size();
  assert(N);

  stats::forks += N-1;

  ForkTag tag = getForkTag(state, reason);

  // XXX do proper balance or keep random?
  result.push_back(&state);
  for (unsigned i=1; i<N; ++i) {
    ExecutionState *es = result[theRNG.getInt32() % i];
    ExecutionState *ns = es->branch();
    addedStates.insert(ns);
    result.push_back(ns);
    es->ptreeNode->data = 0;
    std::pair<PTree::Node*,PTree::Node*> res = 
      processTree->split(es->ptreeNode, ns, es, tag);
    ns->ptreeNode = res.first;
    es->ptreeNode = res.second;
  }

  // If necessary redistribute seeds to match conditions, killing
  // states if necessary due to OnlyReplaySeeds (inefficient but
  // simple).
  
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    std::vector<SeedInfo> seeds = it->second;
    seedMap.erase(it);

    // Assume each seed only satisfies one condition (necessarily true
    // when conditions are mutually exclusive and their conjunction is
    // a tautology).
    for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
           siie = seeds.end(); siit != siie; ++siit) {
      unsigned i;
      for (i=0; i<N; ++i) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(state, siit->assignment.evaluate(conditions[i]), 
                           res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue())
          break;
      }
      
      // If we didn't find a satisfying condition randomly pick one
      // (the seed will be patched).
      if (i==N)
        i = theRNG.getInt32() % N;

      seedMap[result[i]].push_back(*siit);
    }

    if (OnlyReplaySeeds) {
      for (unsigned i=0; i<N; ++i) {
        if (!seedMap.count(result[i])) {
          terminateState(*result[i]);
          result[i] = NULL;
        }
      } 
    }
  }

  for (unsigned i=0; i<N; ++i)
    if (result[i])
      addConstraint(*result[i], conditions[i]);
}

Executor::StatePair 
Executor::fork(ExecutionState &current, ref<Expr> condition, bool isInternal,
    int reason) {
  Solver::Validity res;
  ForkTag tag = getForkTag(current, reason);

  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&current);
  bool isSeeding = it != seedMap.end();

  if (!isSeeding && !isa<ConstantExpr>(condition) && 
      (MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > 60.) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.stack().back().callPathNode;
    if ((MaxStaticForkPct<1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) > 
         stats::forks*MaxStaticForkPct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::forks) > 
                 stats::forks*MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct<1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) > 
         stats::solverTime*MaxStaticSolvePct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::solverTime) > 
                 stats::solverTime*MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value; 
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }      
  }

  double timeout = stpTimeout;
  if (isSeeding)
    timeout *= it->second.size();
  solver->setTimeout(timeout);
  bool success = solver->evaluate(current, condition, res);
  solver->setTimeout(0);
  if (!success) {
    current.pc() = current.prevPC();
    terminateStateEarly(current, "query timed out");
    return StatePair(0, 0);
  }

  if (!isSeeding) {
    if (replayPath && !isInternal) {
      assert(replayPosition<replayPath->size() &&
             "ran out of branches in replay path mode");
      bool branch = (*replayPath)[replayPosition++];
      
      if (res==Solver::True) {
        assert(branch && "hit invalid branch in replay path mode");
      } else if (res==Solver::False) {
        assert(!branch && "hit invalid branch in replay path mode");
      } else {
        // add constraints
        if(branch) {
          res = Solver::True;
          addConstraint(current, condition);
        } else  {
          res = Solver::False;
          addConstraint(current, Expr::createIsZero(condition));
        }
      }
    } else if (res==Solver::Unknown) {
      assert(!replayOut && "in replay mode, only one branch can be true.");
      
      if ((MaxMemoryInhibit && atMemoryLimit) || 
          current.forkDisabled ||
          inhibitForking || 
          (MaxForks!=~0u && stats::forks >= MaxForks)) {

	if (MaxMemoryInhibit && atMemoryLimit)
	  klee_warning_once(0, "skipping fork (memory cap exceeded)");
	else if (current.forkDisabled)
	  klee_warning_once(0, "skipping fork (fork disabled on current path)");
	else if (inhibitForking)
	  klee_warning_once(0, "skipping fork (fork disabled globally)");
	else 
	  klee_warning_once(0, "skipping fork (max-forks reached)");

        TimerStatIncrementer timer(stats::forkTime);
        if (theRNG.getBool()) {
          addConstraint(current, condition);
          res = Solver::True;        
        } else {
          addConstraint(current, Expr::createIsZero(condition));
          res = Solver::False;
        }
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  if (isSeeding && 
      (current.forkDisabled || OnlyReplaySeeds) && 
      res == Solver::Unknown) {
    bool trueSeed=false, falseSeed=false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success = 
        solver->getValue(current, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);
      
      res = trueSeed ? Solver::True : Solver::False;
      addConstraint(current, trueSeed ? condition : Expr::createIsZero(condition));
    }
  }


  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res==Solver::True) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "1";
      }
    }

    return StatePair(&current, (klee::ExecutionState*)NULL);
  } else if (res==Solver::False) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "0";
      }
    }

    return StatePair((klee::ExecutionState*)NULL, &current);
  } else {
    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState, *trueState = &current;

    ++stats::forks;

    falseState = trueState->branch();
    addedStates.insert(falseState);

    if (RandomizeFork && theRNG.getBool())
      std::swap(trueState, falseState);

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
             siie = seeds.end(); siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(current, siit->assignment.evaluate(condition), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }
      
      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState) swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState) swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    current.ptreeNode->data = 0;
    std::pair<PTree::Node*, PTree::Node*> res =
      processTree->split(current.ptreeNode, falseState, trueState, tag);
    falseState->ptreeNode = res.first;
    trueState->ptreeNode = res.second;

    if (!isInternal) {
      if (pathWriter) {
        falseState->pathOS = pathWriter->open(current.pathOS);
        trueState->pathOS << "1";
        falseState->pathOS << "0";
      }      
      if (symPathWriter) {
        falseState->symPathOS = symPathWriter->open(current.symPathOS);
        trueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }

    addConstraint(*trueState, condition);
    addConstraint(*falseState, Expr::createIsZero(condition));

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth<=trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded");
      terminateStateEarly(*falseState, "max-depth exceeded");
      return StatePair(0, 0);
    }

    return StatePair(trueState, falseState);
  }
}

Executor::StatePair
Executor::fork(ExecutionState &current, int reason) {
  ExecutionState *lastState = &current;
  ForkTag tag = getForkTag(current, reason);

  ExecutionState *newState = lastState->branch();

  addedStates.insert(newState);

  lastState->ptreeNode->data = 0;
  std::pair<PTree::Node*,PTree::Node*> res =
   processTree->split(lastState->ptreeNode, newState, lastState, tag);
  newState->ptreeNode = res.first;
  lastState->ptreeNode = res.second;

  return StatePair(newState, lastState);
}

ForkTag Executor::getForkTag(ExecutionState &current, int reason) {
  ForkTag tag((ForkClass)reason);

  if (current.crtThreadIt == current.threads.end())
    return tag;

  tag.location = current.stack().back().kf;

  if (tag.forkClass == KLEE_FORK_FAULTINJ) {
    tag.fiVulnerable = false;
    // Check to see whether we are in a vulnerable call

    for (ExecutionState::stack_ty::iterator it = current.stack().begin();
        it != current.stack().end(); it++) {
      if (!it->caller)
        continue;

      KCallInstruction *callInst = static_cast<KCallInstruction*>((KInstruction*)it->caller);
      assert(callInst);

      if (callInst->vulnerable) {
        tag.fiVulnerable = true;
        break;
      }
    }
  }

  return tag;
}

void Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    assert(CE->isTrue() && "attempt to add invalid constraint");
    return;
  }

  // Check to see if this constraint violates seeds.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      bool res;
      bool success = 
        solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint"); 
  }

  state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition, 
                                 ConstantExpr::alloc(1, Expr::Bool));
}

ref<klee::ConstantExpr> Executor::evalConstant(const KModule *kmodule, const Constant *c) {
  if (const llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
    return evalConstantExpr(kmodule, ce);
  } else {
    if (const ConstantInt *ci = dyn_cast<ConstantInt>(c)) {
      return ConstantExpr::alloc(ci->getValue());
    } else if (const ConstantFP *cf = dyn_cast<ConstantFP>(c)) {
      return ConstantExpr::create(cf->getValueAPF());
    } else if (const ConstantVector *cv = dyn_cast<ConstantVector>(c)) {
      SmallVector<Constant *, 4> elts;
      cv->getVectorElements(elts);
      ref<Expr> *kids = new ref<Expr>[elts.size()];
      for (int i = 0, e = elts.size(); i < e; ++i)
        kids[i] = evalConstant(kmodule, elts[i]);
      ref<Expr> res = ConcatExpr::createN(elts.size(), kids);
      delete[] kids;
      assert(isa<ConstantExpr>(res) && "result of constant vector build not a constant");
      return cast<ConstantExpr>(res);
    } else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      return globalAddresses.find(gv)->second;
    } else if (isa<ConstantPointerNull>(c)) {
      return Expr::createPointer(0);
    } else if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
      return ConstantExpr::create(0, getWidthForLLVMType(kmodule, c->getType()));
    } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(cs->getType());
      llvm::SmallVector<ref<Expr>, 4> kids;
      for (unsigned i = cs->getNumOperands(); i != 0; --i) {
        unsigned op = i-1;
        ref<Expr> kid = evalConstant(kmodule, cs->getOperand(op));

        uint64_t thisOffset = sl->getElementOffsetInBits(op),
                 nextOffset = (op == cs->getNumOperands() - 1)
                              ? sl->getSizeInBits()
                              : sl->getElementOffsetInBits(op+1);
        if (nextOffset-thisOffset > kid->getWidth()) {
          uint64_t paddingWidth = nextOffset-thisOffset-kid->getWidth();
          kids.push_back(ConstantExpr::create(0, paddingWidth));
        }

        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
    } else {
      // ConstantArray
      assert(0 && "invalid argument to evalConstant()");
    }
  }
}

const Cell& Executor::eval(KInstruction *ki, unsigned index, 
                           ExecutionState &state) const {
  assert(index < ki->inst->getNumOperands());
  int vnumber = ki->operands[index];

  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule(state)->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack().back();
    return sf.locals[index];
  }
}

void Executor::bindLocal(KInstruction *target, ExecutionState &state, 
                         ref<Expr> value) {
  getDestCell(state, target).value = value;
}

void Executor::bindArgument(KFunction *kf, unsigned index, 
                            ExecutionState &state, ref<Expr> value) {
  getArgumentCell(state, kf, index).value = value;
}


void Executor::bindArgumentToPthreadCreate(KFunction *kf, unsigned index, 
					   StackFrame &sf, ref<Expr> value) {
  getArgumentCell(sf, kf, index).value = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state, 
                             ref<Expr> &e) {
  ref<Expr> result = e;

  if (!isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool isTrue = false;

    solver->setTimeout(stpTimeout);      
    if (solver->getValue(state, e, value) &&
        solver->mustBeTrue(state, EqExpr::create(e, value), isTrue) &&
        isTrue)
      result = value;
    solver->setTimeout(0);
  }
  
  return result;
}


/* Concretize the given expression, and return a possible constant value. 
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr> 
Executor::toConstant(ExecutionState &state, 
                     ref<Expr> e,
                     const char *reason) {
  e = state.constraints().simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;
    
  std::ostringstream os;
  os << "silently concretizing (reason: " << reason << ") expression " << e 
     << " to value " << value 
     << " (" << (*(state.pc())).info->file << ":" << (*(state.pc())).info->line << ")";
      
  if (AllExternalWarnings)
    klee_warning(reason, os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));
    
  return value;
}

void Executor::executeGetValue(ExecutionState &state,
                               ref<Expr> e,
                               KInstruction *target) {
  e = state.constraints().simplifyExpr(e);
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it==seedMap.end() || isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, e, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    bindLocal(target, state, value);
  } else {
    std::set< ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> value;
      bool success = 
        solver->getValue(state, siit->assignment.evaluate(e), value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      values.insert(value);
    }
    
    std::vector< ref<Expr> > conditions;
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit)
      conditions.push_back(EqExpr::create(e, *vit));

    std::vector<ExecutionState*> branches;
    branch(state, conditions, branches, KLEE_FORK_INTERNAL);
    
    std::vector<ExecutionState*>::iterator bit = branches.begin();
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es)
        bindLocal(target, *es, *vit);
      ++bit;
    }
  }
}

void Executor::stepInstruction(ExecutionState &state) {
  if (DebugPrintInstructions) {
    printFileLine(state, state.pc());
    std::cerr << std::setw(10) << stats::instructions << " ";
    llvm::errs() << *(state.pc()->inst);
  }

  if (statsTracker)
    statsTracker->stepInstruction(state);

  ++stats::instructions;
  state.prevPC() = state.pc();
  ++state.pc();

  if (stats::instructions==StopAfterNInstructions)
    haltExecution = true;
}

KFunction *Executor::getKFunction(Function *function, unsigned &moduleId) {
  unsigned curModuleId = 0;
  for (std::vector<KModule*>::iterator i=kmodules.begin(), e=kmodules.end();
       i != e; ++i) {
    if (*i) {
      typedef std::map<llvm::Function*, KFunction*> FMap;
      FMap &fm = (*i)->functionMap;
      FMap::iterator fi = fm.find(function);
      if (fi != fm.end()) {
        moduleId = curModuleId;
        return fi->second;
      }
    }
    ++curModuleId;
  }
  return NULL;
}

void Executor::executeCall(ExecutionState &state, 
                           KInstruction *ki,
                           Function *f,
                           std::vector< ref<Expr> > &arguments) {
  Instruction *i = NULL;
  if (ki)
      i = ki->inst;

  if (ki && f && f->isDeclaration()) {
    switch(f->getIntrinsicID()) {
    case Intrinsic::not_intrinsic:
      // state may be destroyed by this call, cannot touch
      callExternalFunction(state, ki, f, arguments);
      break;
        
    case Intrinsic::x86_sse_sqrt_ps:
    case Intrinsic::sqrt: {
      bindLocal(ki, state, FUnSIMDOperation(this, kmodule(state), FSqrtExpr::create).eval(i->getType(), arguments[0]));
      break;
    }
      // va_arg is handled by caller and intrinsic lowering, see comment for
      // ExecutionState::varargs
    case Intrinsic::vastart:  {
      StackFrame &sf = state.stack().back();
      assert(sf.varargs && 
             "vastart called in function with no vararg object");

      // FIXME: This is really specific to the architecture, not the pointer
      // size. This happens to work fir x86-32 and x86-64, however.
      Expr::Width WordSize = Context::get().getPointerWidth();
      if (WordSize == Expr::Int32) {
        executeMemoryOperation(state, true, 0, arguments[0], 
                               sf.varargs->getBaseExpr(), 0);
      } else {
        assert(WordSize == Expr::Int64 && "Unknown word size!");

        // X86-64 has quite complicated calling convention. However,
        // instead of implementing it, we can do a simple hack: just
        // make a function believe that all varargs are on stack.
        executeMemoryOperation(state, true, 0, arguments[0],
                               ConstantExpr::create(48, 32), 0); // gp_offset
        executeMemoryOperation(state, true, 0,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(4, 64)),
                               ConstantExpr::create(304, 32), 0); // fp_offset
        executeMemoryOperation(state, true, 0,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(8, 64)),
                               sf.varargs->getBaseExpr(), 0); // overflow_arg_area
        executeMemoryOperation(state, true, 0,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(16, 64)),
                               ConstantExpr::create(0, 64), 0); // reg_save_area
      }
      break;
    }
    case Intrinsic::vaend:
      // va_end is a noop for the interpreter.
      //
      // FIXME: We should validate that the target didn't do something bad
      // with vaeend, however (like call it twice).
      break;
        
    case Intrinsic::vacopy:
      // va_copy should have been lowered.
      //
      // FIXME: It would be nice to check for errors in the usage of this as
      // well.
    default:
      klee_error("unknown intrinsic: %s", f->getName().data());
    }

    if (InvokeInst *ii = dyn_cast<InvokeInst>(i))
      transferToBasicBlock(ii->getNormalDest(), i->getParent(), state);
  } else {
    // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
    // guess. This just done to avoid having to pass KInstIterator everywhere
    // instead of the actual instruction, since we can't make a KInstIterator
    // from just an instruction (unlike LLVM).
    unsigned moduleId;
    KFunction *kf = getKFunction(f, moduleId);
    assert(kf && "KFunction not found!");

    state.pushFrame(state.prevPC(), kf, moduleId);
    state.pc() = kf->instructions;
        
    if (statsTracker)
      statsTracker->framePushed(state, &state.stack()[state.stack().size()-2]); //XXX TODO fix this ugly stuff
 
     // TODO: support "byval" parameter attribute
     // TODO: support zeroext, signext, sret attributes
        
    unsigned callingArgs = arguments.size();
    unsigned funcArgs = f->arg_size();
    if (!f->isVarArg()) {
      if (callingArgs > funcArgs) {
        klee_warning_once(f, "calling %s with extra arguments.", 
                          f->getName().data());
      } else if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments", 
                              "user.err");
        return;
      }
    } else {
      if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments", 
                              "user.err");
        return;
      }
            
      StackFrame &sf = state.stack().back();
      unsigned size = 0;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work fir x86-32 and x86-64, however.
        Expr::Width WordSize = Context::get().getPointerWidth();
        if (WordSize == Expr::Int32) {
          size += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          size += llvm::RoundUpToAlignment(arguments[i]->getWidth(), 
                                           WordSize) / 8;
        }
      }

      MemoryObject *mo = sf.varargs = memory->allocate(&state, size, true, false,
                                                       state.prevPC()->inst);
      if (!mo) {
        terminateStateOnExecError(state, "out of memory (varargs)");
        return;
      }
      ObjectState *os = bindObjectInState(state, 0, mo, true);
      unsigned offset = 0;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work fir x86-32 and x86-64, however.
        Expr::Width WordSize = Context::get().getPointerWidth();
        if (WordSize == Expr::Int32) {
          os->write(offset, arguments[i], &state, solver);
          offset += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          assert(WordSize == Expr::Int64 && "Unknown word size!");
          os->write(offset, arguments[i], &state, solver);
          offset += llvm::RoundUpToAlignment(arguments[i]->getWidth(), 
                                             WordSize) / 8;
        }
      }
    }

    unsigned numFormals = f->arg_size();
    for (unsigned i=0; i<numFormals; ++i) 
      bindArgument(kf, i, state, arguments[i]);
  }
}

void Executor::transferToBasicBlock(BasicBlock *dst, BasicBlock *src, 
                                    ExecutionState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.
  
  // XXX this lookup has to go ?
  KFunction *kf = state.stack().back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.pc() = &kf->instructions[entry];
  if (state.pc()->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode*>(state.pc()->inst);
    state.crtThread().incomingBBIndex = first->getBasicBlockIndex(src);
  }
}

void Executor::printFileLine(ExecutionState &state, KInstruction *ki) {
  const InstructionInfo &ii = *ki->info;
  if (ii.file != "") 
    std::cerr << "     " << ii.file << ":" << ii.line << ":";
  else
    std::cerr << "     [no debug info]:";
}

/// Compute the true target of a function call, resolving LLVM and KLEE aliases
/// and bitcasts.
Function* Executor::getTargetFunction(Value *calledVal, ExecutionState &state) {
  SmallPtrSet<const GlobalValue*, 3> Visited;

  Constant *c = dyn_cast<Constant>(calledVal);
  if (!c)
    return 0;

  while (true) {
    if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      if (!Visited.insert(gv))
        return 0;

      std::string alias = state.getFnAlias(gv->getName());
      if (alias != "") {
        llvm::Module* currModule = kmodule(state)->module;
        GlobalValue *old_gv = gv;
        gv = currModule->getNamedValue(alias);
        if (!gv) {
          llvm::errs() << "Function " << alias << "(), alias for " 
                       << old_gv->getName() << " not found!\n";
          assert(0 && "function alias not found");
        }
      }
     
      if (Function *f = dyn_cast<Function>(gv))
        return f;
      else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
        c = ga->getAliasee();
      else
        return 0;
    } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      if (ce->getOpcode()==Instruction::BitCast)
        c = ce->getOperand(0);
      else
        return 0;
    } else
      return 0;
  }
}

static bool isDebugIntrinsic(const Function *f, KModule *KM) {
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 7)
  // Fast path, getIntrinsicID is slow.
  if (f == KM->dbgStopPointFn)
    return true;

  switch (f->getIntrinsicID()) {
  case Intrinsic::dbg_stoppoint:
  case Intrinsic::dbg_region_start:
  case Intrinsic::dbg_region_end:
  case Intrinsic::dbg_func_start:
  case Intrinsic::dbg_declare:
    return true;

  default:
    return false;
  }
#else
  return false;
#endif
}

static inline const llvm::fltSemantics * fpWidthToSemantics(unsigned width) {
  switch(width) {
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
  default:
    return 0;
  }
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki) {
  Instruction *i = ki->inst;
  switch (i->getOpcode()) {
    // Control flow
  case Instruction::Ret: {
    ReturnInst *ri = cast<ReturnInst>(i);
    KInstIterator kcaller = state.stack().back().caller;
    Instruction *caller = kcaller ? kcaller->inst : 0;
    bool isVoidReturn = (ri->getNumOperands() == 0);
    ref<Expr> result = ConstantExpr::alloc(0, Expr::Bool);

    if (!isVoidReturn) {
      result = eval(ki, 0, state).value;
    }
    
    if (state.stack().size() <= 1) {
      assert(!caller && "caller set on initial stack frame");
      
      if (state.threads.size() == 1) {
        //main exit
        terminateStateOnExit(state);
      } else if (state.crtProcess().threads.size() == 1){
        // Invoke exit()
        Function *f = kmodule(state)->module->getFunction("exit");
        std::vector<ref<Expr> > arguments;
        arguments.push_back(result);

        executeCall(state, NULL, f, arguments);
      } else {
        // Invoke pthread_exit()
        Function *f = kmodule(state)->module->getFunction("pthread_exit");
        std::vector<ref<Expr> > arguments;
        arguments.push_back(result);

        executeCall(state, NULL, f, arguments);
      }
    } else {
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
        transferToBasicBlock(ii->getNormalDest(), caller->getParent(), state);
      } else {
        state.pc() = kcaller;
        ++state.pc();
      }

      if (!isVoidReturn) {
        LLVM_TYPE_Q Type *t = caller->getType();
        if (t != Type::getVoidTy(getGlobalContext())) {
          // may need to do coercion due to bitcasts
          Expr::Width from = result->getWidth();
          Expr::Width to = getWidthForLLVMType(kmodule(state), t);
            
          if (from != to) {
            CallSite cs = (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller)) : 
                           CallSite(cast<CallInst>(caller)));

            // XXX need to check other param attrs ?
            if (cs.paramHasAttr(0, llvm::Attribute::SExt)) {
              result = SExtExpr::create(result, to);
            } else {
              result = ZExtExpr::create(result, to);
            }
          }

          bindLocal(kcaller, state, result);
        }
      } else {
        // We check that the return value has no users instead of
        // checking the type, since C defaults to returning int for
        // undeclared functions.
        if (!caller->use_empty()) {
          terminateStateOnExecError(state, "return void when caller expected a result");
        }
      }
    }      
    break;
  }
  case Instruction::Unwind: {
    for (;;) {
      KInstruction *kcaller = state.stack().back().caller;
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (state.stack().empty()) {
        terminateStateOnExecError(state, "unwind from initial stack frame");
        break;
      } else {
        Instruction *caller = kcaller->inst;
        if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
          transferToBasicBlock(ii->getUnwindDest(), caller->getParent(), state);
          break;
        }
      }
    }
    break;
  }
  case Instruction::Br: {
    BranchInst *bi = cast<BranchInst>(i);
    int reason = KLEE_FORK_DEFAULT;

    if (state.crtSpecialFork == i) {
      reason = state.crtForkReason;
      state.crtSpecialFork = NULL;
    } else {
      assert(!state.crtForkReason && "another branching instruction between a klee_branch and its corresponding 'if'");
    }

    if (bi->isUnconditional()) {
      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), state);
    } else {
      // FIXME: Find a way that we don't have this hidden dependency.
      assert(bi->getCondition() == bi->getOperand(0) &&
             "Wrong operand index!");
      ref<Expr> cond = eval(ki, 0, state).value;
      Executor::StatePair branches = fork(state, cond, false, reason);

      // NOTE: There is a hidden dependency here, markBranchVisited
      // requires that we still be in the context of the branch
      // instruction (it reuses its statistic id). Should be cleaned
      // up with convenient instruction specific data.
      if (statsTracker && state.stack().back().kf->trackCoverage)
        statsTracker->markBranchVisited(branches.first, branches.second);

      if (branches.first)
        transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), *branches.first);
      if (branches.second)
        transferToBasicBlock(bi->getSuccessor(1), bi->getParent(), *branches.second);
    }
    break;
  }
  case Instruction::Switch: {
    SwitchInst *si = cast<SwitchInst>(i);
    ref<Expr> cond = eval(ki, 0, state).value;
    unsigned cases = si->getNumCases();
    BasicBlock *bb = si->getParent();

    cond = toUnique(state, cond);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // Somewhat gross to create these all the time, but fine till we
      // switch to an internal rep.
      LLVM_TYPE_Q llvm::IntegerType *Ty = 
        cast<IntegerType>(si->getCondition()->getType());
      ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
      unsigned index = si->findCaseValue(ci);
      transferToBasicBlock(si->getSuccessor(index), si->getParent(), state);
    } else {
      std::vector<std::pair<BasicBlock*, ref<Expr> > > targets;

      ref<Expr> isDefault = ConstantExpr::alloc(1, Expr::Bool);

      for (unsigned i = 1; i < cases; ++i) {
        ref<Expr> value = evalConstant(kmodule(state), si->getCaseValue(i));
        ref<Expr> match = EqExpr::create(cond, value);
        isDefault = AndExpr::create(isDefault, Expr::createIsZero(match));
        bool result;
        bool success = solver->mayBeTrue(state, match, result);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;

        if (result) {
          unsigned k = 0;
          for (k = 0; k < targets.size(); k++) {
            if (targets[k].first == si->getSuccessor(i)) {
              targets[k].second = OrExpr::create(match, targets[k].second);
              break;
            }
          }

          if (k == targets.size()) {
            targets.push_back(std::make_pair(si->getSuccessor(i), match));
          }
        }
      }

      bool res;
      bool success = solver->mayBeTrue(state, isDefault, res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        unsigned k = 0;
        for (k = 0; k < targets.size(); k++) {
          if (targets[k].first == si->getSuccessor(0)) {
            targets[k].second = OrExpr::create(isDefault, targets[k].second);
            break;
          }
        }

        if (k == targets.size()) {
          targets.push_back(std::make_pair(si->getSuccessor(0), isDefault));
        }
      }
      
      std::vector< ref<Expr> > conditions;
      for (std::vector<std::pair<BasicBlock*, ref<Expr> > >::iterator it =
             targets.begin(), ie = targets.end();
           it != ie; ++it)
        conditions.push_back(it->second);
      
      std::vector<ExecutionState*> branches;
      branch(state, conditions, branches, KLEE_FORK_DEFAULT);
        
      std::vector<ExecutionState*>::iterator bit = branches.begin();
      for (std::vector<std::pair<BasicBlock*, ref<Expr> > >::iterator it =
             targets.begin(), ie = targets.end();
           it != ie; ++it) {
        ExecutionState *es = *bit;
        if (es)
          transferToBasicBlock(it->first, bb, *es);
        ++bit;
      }
    }
    break;
 }
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call: {
    CallSite cs(i);

    unsigned numArgs = cs.arg_size();
    Value *fp = cs.getCalledValue();
    Function *f = getTargetFunction(fp, state);

    // Skip debug intrinsics, we can't evaluate their metadata arguments.
    if (f && isDebugIntrinsic(f, kmodule(state)))
      break;

    // evaluate arguments
    std::vector< ref<Expr> > arguments;
    arguments.reserve(numArgs);

    for (unsigned j=0; j<numArgs; ++j)
      arguments.push_back(eval(ki, j+1, state).value);

    if (f) {
      const FunctionType *fType = 
        dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
      const FunctionType *fpType =
        dyn_cast<FunctionType>(cast<PointerType>(fp->getType())->getElementType());

      // special case the call with a bitcast case
      if (fType != fpType) {
        assert(fType && fpType && "unable to get function type");

        // XXX check result coercion

        // XXX this really needs thought and validation
        unsigned i=0;
        for (std::vector< ref<Expr> >::iterator
               ai = arguments.begin(), ie = arguments.end();
             ai != ie; ++ai) {
          Expr::Width to, from = (*ai)->getWidth();
            
          if (i<fType->getNumParams()) {
            to = getWidthForLLVMType(kmodule(state), fType->getParamType(i));

            if (from != to) {
              // XXX need to check other param attrs ?
              if (cs.paramHasAttr(i+1, llvm::Attribute::SExt)) {
                arguments[i] = SExtExpr::create(arguments[i], to);
              } else {
                arguments[i] = ZExtExpr::create(arguments[i], to);
              }
            }
          }
            
          i++;
        }
      } else if (isa<InlineAsm>(fp)) {
        terminateStateOnExecError(state, "inline assembly is unsupported");
        break;
      }

      executeCall(state, ki, f, arguments);
    } else {
      ref<Expr> v = eval(ki, 0, state).value;

      ExecutionState *free = &state;
      bool hasInvalid = false, first = true;

      /* XXX This is wasteful, no need to do a full evaluate since we
         have already got a value. But in the end the caches should
         handle it for us, albeit with some overhead. */
      do {
        ref<ConstantExpr> value;
        bool success = solver->getValue(*free, v, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        StatePair res = fork(*free, EqExpr::create(v, value), true, KLEE_FORK_INTERNAL);
        if (res.first) {
          uint64_t addr = value->getZExtValue();
          if (legalFunctions.count(addr)) {
            f = (Function*) addr;

            // Don't give warning on unique resolution
            if (res.second || !first)
              klee_warning_once((void*) (unsigned long) addr, 
                                "resolved symbolic function pointer to: %s",
                                f->getName().data());

            executeCall(*res.first, ki, f, arguments);
          } else {
            if (!hasInvalid) {
              terminateStateOnExecError(state, "invalid function pointer");
              hasInvalid = true;
            }
          }
        }

        first = false;
        free = res.second;
      } while (free);
    }
    break;
  }
  case Instruction::PHI: {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
    ref<Expr> result = eval(ki, state.crtThread().incomingBBIndex, state).value;
#else
    ref<Expr> result = eval(ki, state.crtThread().incomingBBIndex * 2, state).value;
#endif
    bindLocal(ki, state, result);
    break;
  }

    // Special instructions
  case Instruction::Select: {
    SelectInst *SI = cast<SelectInst>(ki->inst);
    assert(SI->getCondition() == SI->getOperand(0) &&
           "Wrong operand index!");
    ref<Expr> cond = eval(ki, 0, state).value;
    ref<Expr> tExpr = eval(ki, 1, state).value;
    ref<Expr> fExpr = eval(ki, 2, state).value;

#if 0
    Expr::Kind condKind = cond->getKind();
    if (condKind == Expr::FOrd1 || (condKind >= Expr::FOrd && condKind <= Expr::FUne)) {
      Executor::StatePair branches = fork(state, cond, true);
      if (branches.first)
        bindLocal(ki, *branches.first, tExpr);
      if (branches.second)
        bindLocal(ki, *branches.second, fExpr);
    } else
#endif
    {
      ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
      bindLocal(ki, state, result);
    }
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

    // Arithmetic / logical

  case Instruction::Add: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, ISIMDOperation(this, kmodule(state), AddExpr::create).eval(i->getType(), left, right));
    break;
  }

  case Instruction::Sub: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, ISIMDOperation(this, kmodule(state), SubExpr::create).eval(i->getType(), left, right));
    break;
  }
 
  case Instruction::Mul: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, ISIMDOperation(this, kmodule(state), MulExpr::create).eval(i->getType(), left, right));
    break;
  }

  case Instruction::UDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ISIMDOperation(this, kmodule(state), UDivExpr::create).eval(i->getType(), left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::SDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ISIMDOperation(this, kmodule(state), SDivExpr::create).eval(i->getType(), left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::URem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ISIMDOperation(this, kmodule(state), URemExpr::create).eval(i->getType(), left, right);
    bindLocal(ki, state, result);
    break;
  }
 
  case Instruction::SRem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ISIMDOperation(this, kmodule(state), SRemExpr::create).eval(i->getType(), left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::And: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AndExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Or: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = OrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Xor: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = XorExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Shl: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ISIMDOperation(this, kmodule(state), ShlExpr::create).eval(i->getType(), left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::LShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ISIMDOperation(this, kmodule(state), LShrExpr::create).eval(i->getType(), left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::AShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ISIMDOperation(this, kmodule(state), AShrExpr::create).eval(i->getType(), left, right);
    bindLocal(ki, state, result);
    break;
  }

    // Compare

  case Instruction::ICmp: {
    CmpInst *ci = cast<CmpInst>(i);
    ICmpInst *ii = cast<ICmpInst>(ci);
 
    switch(ii->getPredicate()) {
    case ICmpInst::ICMP_EQ: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), EqExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_NE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), NeExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_UGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), UgtExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state,result);
      break;
    }

    case ICmpInst::ICMP_UGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), UgeExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), UltExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), UleExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), SgtExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), SgeExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), SltExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = ISIMDOperation(this, kmodule(state), SleExpr::create).eval(i->getType(), left, right);
      bindLocal(ki, state, result);
      break;
    }

    default:
      terminateStateOnExecError(state, "invalid ICmp predicate");
    }
    break;
  }
 
    // Memory instructions...
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 7)
  case Instruction::Malloc:
  case Instruction::Alloca: {
    AllocationInst *ai = cast<AllocationInst>(i);
#else
  case Instruction::Alloca: {
    AllocaInst *ai = cast<AllocaInst>(i);
#endif
    unsigned elementSize = 
      kmodule(state)->targetData->getTypeStoreSize(ai->getAllocatedType());
    ref<Expr> size = Expr::createPointer(elementSize);
    if (ai->isArrayAllocation()) {
      ref<Expr> count = eval(ki, 0, state).value;
      count = Expr::createZExtToPointerWidth(count);
      size = MulExpr::create(size, count);
    }
    bool isLocal = i->getOpcode()==Instruction::Alloca;
    executeAlloc(state, size, isLocal, ki);
    break;
  }
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 7)
  case Instruction::Free: {
    executeFree(state, eval(ki, 0, state).value);
    break;
  }
#endif

  case Instruction::Load: {
    LoadInst *li = cast<LoadInst>(i);
    unsigned addrspace = li->getPointerAddressSpace();
    ref<Expr> base = eval(ki, 0, state).value;
    executeMemoryOperation(state, false, addrspace, base, 0, ki);
    break;
  }
  case Instruction::Store: {
    StoreInst *si = cast<StoreInst>(i);
    unsigned addrspace = si->getPointerAddressSpace();
    ref<Expr> base = eval(ki, 1, state).value;
    ref<Expr> value = eval(ki, 0, state).value;
    executeMemoryOperation(state, true, addrspace, base, value, 0);
    break;
  }

  case Instruction::GetElementPtr: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
    ref<Expr> base = eval(ki, 0, state).value;

    for (std::vector< std::pair<unsigned, uint64_t> >::iterator 
           it = kgepi->indices.begin(), ie = kgepi->indices.end(); 
         it != ie; ++it) {
      uint64_t elementSize = it->second;
      ref<Expr> index = eval(ki, it->first, state).value;
      base = AddExpr::create(base,
                             MulExpr::create(Expr::createSExtToPointerWidth(index),
                                             Expr::createPointer(elementSize)));
    }
    if (kgepi->offset)
      base = AddExpr::create(base,
                             Expr::createPointer(kgepi->offset));
    bindLocal(ki, state, base);
    break;
  }

    // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ExtractExpr::create(eval(ki, 0, state).value,
                                           0,
                                           getWidthForLLVMType(kmodule(state), ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ZExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ZExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(kmodule(state), ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::SExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = SExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(kmodule(state), ci->getType()));
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::IntToPtr: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width pType = getWidthForLLVMType(kmodule(state), ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, pType));
    break;
  } 
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width iType = getWidthForLLVMType(kmodule(state), ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, iType));
    break;
  }

  case Instruction::BitCast: {
    ref<Expr> result = eval(ki, 0, state).value;
    bindLocal(ki, state, result);
    break;
  }

    // Floating point instructions

  case Instruction::FAdd: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right  = eval(ki, 1, state).value;
    bindLocal(ki, state, FSIMDOperation(this, kmodule(state), FAddExpr::create).eval(i->getType(), left, right));
    break;
  }

  case Instruction::FSub: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right  = eval(ki, 1, state).value;
    bindLocal(ki, state, FSIMDOperation(this, kmodule(state), FSubExpr::create).eval(i->getType(), left, right));
    break;
  }

  case Instruction::FMul: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right  = eval(ki, 1, state).value;
    bindLocal(ki, state, FSIMDOperation(this, kmodule(state), FMulExpr::create).eval(i->getType(), left, right));
    break;
  }

  case Instruction::FDiv: {
    if (i->getMetadata("fpaccuracy")) {
      ref<Expr> undef =
        AnyExpr::create(getWidthForLLVMType(kmodule(state), i->getType()));
      bindLocal(ki, state, undef);
    } else {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right  = eval(ki, 1, state).value;
      bindLocal(ki, state, FSIMDOperation(this, kmodule(state), FDivExpr::create).eval(i->getType(), left, right));
    }
    break;
  }

  case Instruction::FRem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right  = eval(ki, 1, state).value;
    bindLocal(ki, state, FSIMDOperation(this, kmodule(state), FRemExpr::create).eval(i->getType(), left, right));
    break;
  }

  case Instruction::FPTrunc:
  case Instruction::FPExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> arg = eval(ki, 0, state).value;
    const llvm::Type *type = i->getType();
    const fltSemantics *sem = TypeToFloatSemantics(type);
    bindLocal(ki, state,
       (i->getOpcode() == Instruction::FPTrunc
      ? FPTruncExpr::create
      : FPExtExpr::create)(arg, sem, ci->getSrcTy()->isFP128Ty()));
    break;
  }

  case Instruction::FPToUI:
  case Instruction::FPToSI: {
    ref<Expr> arg = eval(ki, 0, state).value;
    LLVM_TYPE_Q llvm::Type *type = i->getType();
    bindLocal(ki, state, F2ISIMDOperation(this, kmodule(state),
       (i->getOpcode() == Instruction::FPToUI
      ? FPToUIExpr::create
      : FPToSIExpr::create),
       i->getMetadata("round_nearest")).eval(type, arg));
    break;
  }

  case Instruction::UIToFP:
  case Instruction::SIToFP: {
    ref<Expr> arg = eval(ki, 0, state).value;
    LLVM_TYPE_Q llvm::Type *type = i->getType();
    bindLocal(ki, state, I2FSIMDOperation(this, kmodule(state),
       (i->getOpcode() == Instruction::UIToFP
      ? UIToFPExpr::create
      : SIToFPExpr::create)).eval(type, arg));
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;

    ref<Expr> Result = FCmpSIMDOperation(this, kmodule(state), fi->getPredicate()).eval(i->getType(), fi->getOperand(0)->getType(), left, right);
    bindLocal(ki, state, Result);
    break;
  }
 
    // Other instructions...
    // Unhandled
  case Instruction::ExtractElement: {
    ExtractElementInst *eei = cast<ExtractElementInst>(i);
    ref<Expr> vec = eval(ki, 0, state).value;
    ref<Expr> idx = eval(ki, 1, state).value;

    assert(isa<ConstantExpr>(idx) && "symbolic index unsupported");
    ConstantExpr *cIdx = cast<ConstantExpr>(idx);
    uint64_t iIdx = cIdx->getZExtValue();

    const llvm::VectorType *vt = eei->getVectorOperandType();
    unsigned EltBits = getWidthForLLVMType(kmodule(state), vt->getElementType());

    ref<Expr> Result = ExtractExpr::create(vec, EltBits*iIdx, EltBits);

    bindLocal(ki, state, Result);
    break;
  }
  case Instruction::InsertElement: {
    InsertElementInst *iei = cast<InsertElementInst>(i);
    ref<Expr> vec = eval(ki, 0, state).value;
    ref<Expr> newElt = eval(ki, 1, state).value;
    ref<Expr> idx = eval(ki, 2, state).value;

    assert(isa<ConstantExpr>(idx) && "symbolic index unsupported");
    ConstantExpr *cIdx = cast<ConstantExpr>(idx);
    uint64_t iIdx = cIdx->getZExtValue();

    const llvm::VectorType *vt = iei->getType();
    unsigned EltBits = getWidthForLLVMType(kmodule(state), vt->getElementType());

    unsigned ElemCount = vt->getNumElements();
    ref<Expr> *elems = new ref<Expr>[vt->getNumElements()];
    for (unsigned i = 0; i < ElemCount; ++i)
      elems[ElemCount-i-1] = i == iIdx
                             ? newElt
                             : ExtractExpr::create(vec, EltBits*i, EltBits);

    ref<Expr> Result = ConcatExpr::createN(ElemCount, elems);
    delete[] elems;

    bindLocal(ki, state, Result);
    break;
  }
  case Instruction::ShuffleVector: {
    ShuffleVectorInst *svi = cast<ShuffleVectorInst>(i);

    ref<Expr> vec1 = eval(ki, 0, state).value;
    ref<Expr> vec2 = eval(ki, 1, state).value;
    const llvm::VectorType *vt = svi->getType();
    unsigned EltBits = getWidthForLLVMType(kmodule(state), vt->getElementType());

    unsigned ElemCount = vt->getNumElements();
    ref<Expr> *elems = new ref<Expr>[vt->getNumElements()];
    for (unsigned i = 0; i < ElemCount; ++i) {
      int MaskValI = svi->getMaskValue(i);
      ref<Expr> &el = elems[ElemCount-i-1];
      if (MaskValI < 0)
	el = ConstantExpr::alloc(0, EltBits);
      else {
	unsigned MaskVal = (unsigned) MaskValI;
	if (MaskVal < ElemCount)
          el = ExtractExpr::create(vec1, EltBits*MaskVal, EltBits);
        else
	  el = ExtractExpr::create(vec2, EltBits*(MaskVal-ElemCount), EltBits);
      }
    }

    ref<Expr> Result = ConcatExpr::createN(ElemCount, elems);
    delete[] elems;

    bindLocal(ki, state, Result);
    break;
  }
  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;
    ref<Expr> val = eval(ki, 1, state).value;

    ref<Expr> l = NULL, r = NULL;
    unsigned lOffset = kgepi->offset*8, rOffset = kgepi->offset*8 + val->getWidth();

    if (lOffset > 0)
      l = ExtractExpr::create(agg, 0, lOffset);
    if (rOffset < agg->getWidth())
      r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

    ref<Expr> result;
    if (!l.isNull() && !r.isNull())
      result = ConcatExpr::create(r, ConcatExpr::create(val, l));
    else if (!l.isNull())
      result = ConcatExpr::create(val, l);
    else if (!r.isNull())
      result = ConcatExpr::create(r, val);
    else
      result = val;

    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;

    ref<Expr> result = ExtractExpr::create(agg, kgepi->offset*8, getWidthForLLVMType(kmodule(state), i->getType()));

    bindLocal(ki, state, result);
    break;
  }
  default:
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

void Executor::updateStates(ExecutionState *current) {
  if (searcher) {
    searcher->update(current, addedStates, removedStates);
  }
  
  states.insert(addedStates.begin(), addedStates.end());
  addedStates.clear();
  
  for (std::set<ExecutionState*>::iterator
         it = removedStates.begin(), ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState*>::iterator it2 = states.find(es);
    assert(it2!=states.end());
    states.erase(it2);
    std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    processTree->remove(es->ptreeNode);
    delete es;
  }
  removedStates.clear();
}

template <typename TypeIt>
void Executor::computeOffsets(KModule *kmodule, KGEPInstruction *kgepi,
                              TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
    ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (LLVM_TYPE_Q StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned) ci->getZExtValue());
      constantOffset = constantOffset->Add(ConstantExpr::alloc(addend,
                                                               Context::get().getPointerWidth()));
    } else {
      const SequentialType *set = cast<SequentialType>(*ii);
      uint64_t elementSize = 
        kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index = evalConstant(kmodule, c);
        assert(isa<ConstantExpr>(index) && "index not an integer");
        ref<ConstantExpr> indexExt =
          cast<ConstantExpr>(index)->SExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend = 
          indexExt->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
    }
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KModule *kmodule, KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kmodule, kgepi, gep_type_begin(gepi), gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kmodule, kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kmodule, kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants(KModule *kmodule) {
  for (std::vector<KFunction*>::iterator it = kmodule->functions.begin(), 
         ie = kmodule->functions.end(); it != ie; ++it) {
    KFunction *kf = *it;
    for (unsigned i=0; i<kf->numInstructions; ++i)
      bindInstructionConstants(kmodule, kf->instructions[i]);
  }

  kmodule->constantTable = new Cell[kmodule->constants.size()];
  for (unsigned i=0; i<kmodule->constants.size(); ++i) {
    Cell &c = kmodule->constantTable[i];
    c.value = evalConstant(kmodule, kmodule->constants[i]);
  }
}

void Executor::bindModuleConstants(unsigned moduleId) {
  bindModuleConstants(kmodules[moduleId]);
}

void Executor::run(ExecutionState &initialState) {
  bindModuleConstants(kmodule(initialState));

  // Delay init till now so that ticks don't accrue during
  // optimization and such.
  initTimers();

  states.insert(&initialState);

  if (usingSeeds) {
    std::vector<SeedInfo> &v = seedMap[&initialState];
    
    for (std::vector<KTest*>::const_iterator it = usingSeeds->begin(), 
           ie = usingSeeds->end(); it != ie; ++it)
      v.push_back(SeedInfo(*it));

    int lastNumSeeds = usingSeeds->size()+10;
    double lastTime, startTime = lastTime = util::getWallTime();
    ExecutionState *lastState = 0;
    while (!seedMap.empty()) {
      if (haltExecution) goto dump;

      std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it = 
        seedMap.upper_bound(lastState);
      if (it == seedMap.end())
        it = seedMap.begin();
      lastState = it->first;
      unsigned numSeeds = it->second.size();
      ExecutionState &state = *lastState;
      KInstruction *ki = state.pc();
      stepInstruction(state);

      executeInstruction(state, ki);
      state.stateTime++;
      processTimers(&state, MaxInstructionTime * numSeeds);
      updateStates(&state);

      if ((stats::instructions % 1000) == 0) {
        int numSeeds = 0, numStates = 0;
        for (std::map<ExecutionState*, std::vector<SeedInfo> >::iterator
               it = seedMap.begin(), ie = seedMap.end();
             it != ie; ++it) {
          numSeeds += it->second.size();
          numStates++;
        }
        double time = util::getWallTime();
        if (SeedTime>0. && time > startTime + SeedTime) {
          klee_warning("seed time expired, %d seeds remain over %d states",
                       numSeeds, numStates);
          break;
        } else if (numSeeds<=lastNumSeeds-10 ||
                   time >= lastTime+10) {
          lastTime = time;
          lastNumSeeds = numSeeds;          
          klee_message("%d seeds remaining over: %d states", 
                       numSeeds, numStates);
        }
      }
    }

    klee_message("seeding done (%d states remain)", (int) states.size());

    // XXX total hack, just because I like non uniform better but want
    // seed results to be equally weighted.
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      (*it)->weight = 1.;
    }

    if (OnlySeed)
      goto dump;
  }

  searcher = constructUserSearcher(*this);

  searcher->update(0, states, std::set<ExecutionState*>());

  while (!states.empty() && !haltExecution) {
    ExecutionState &state = searcher->selectState();
    KInstruction *ki = state.pc();
    stepInstruction(state);

    executeInstruction(state, ki);
    processTimers(&state, MaxInstructionTime);

    if (MaxMemory) {
        if ((stats::instructions & 0xFFFF) == 0) {
            // We need to avoid calling GetMallocUsage() often because it
            // is O(elts on freelist). This is really bad since we start
            // to pummel the freelist once we hit the memory cap.
            unsigned mbs = sys::Process::GetTotalMemoryUsage() >> 20;

            if (mbs > MaxMemory) {
                if (mbs > MaxMemory + 100) {
                    // just guess at how many to kill
                    unsigned numStates = states.size();
                    unsigned toKill = std::max(1U, numStates - numStates
                            * MaxMemory / mbs);

                    if (MaxMemoryInhibit)
                        klee_warning("killing %d states (over memory cap)",
                                toKill);

                    std::vector<ExecutionState*> arr(states.begin(),
                            states.end());
                    for (unsigned i = 0, N = arr.size(); N && i < toKill; ++i, --N) {
                        unsigned idx = rand() % N;

                        // Make two pulls to try and not hit a state that
                        // covered new code.
                        if (arr[idx]->coveredNew)
                            idx = rand() % N;

                        std::swap(arr[idx], arr[N - 1]);

                        terminateStateEarly(*arr[N - 1], "memory limit");
                    }
                }
                atMemoryLimit = true;
            } else {
                atMemoryLimit = false;
            }
        }
    }

    updateStates(&state);
  }

  delete searcher;
  searcher = 0;
  
 dump:
  if (DumpStatesOnHalt && !states.empty()) {
    std::cerr << "KLEE: halting execution, dumping remaining states\n";
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      ExecutionState &state = **it;
      stepInstruction(state); // keep stats rolling
      terminateStateEarly(state, "execution halting");
    }
    updateStates(0);
  }
}

std::string Executor::getAddressInfo(ExecutionState &state, 
                                     ref<Expr> address) const{
  std::ostringstream info;
  info << "\taddress: " << address << "\n";
  uint64_t example;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
    example = CE->getZExtValue();
  } else {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, address, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    example = value->getZExtValue();
    info << "\texample: " << example << "\n";
    std::pair< ref<Expr>, ref<Expr> > res = solver->getRange(state, address);
    info << "\trange: [" << res.first << ", " << res.second <<"]\n";
  }
  
  MemoryObject hack((unsigned) example);    
  MemoryMap::iterator lower = state.addressSpace().objects.upper_bound(&hack);
  info << "\tnext: ";
  if (lower==state.addressSpace().objects.end()) {
    info << "none\n";
  } else {
    const MemoryObject *mo = lower->first;
    std::string alloc_info;
    mo->getAllocInfo(alloc_info);
    info << "object at " << mo->address
         << " of size " << mo->size << "\n"
         << "\t\t" << alloc_info << "\n";
  }
  if (lower!=state.addressSpace().objects.begin()) {
    --lower;
    info << "\tprev: ";
    if (lower==state.addressSpace().objects.end()) {
      info << "none\n";
    } else {
      const MemoryObject *mo = lower->first;
      std::string alloc_info;
      mo->getAllocInfo(alloc_info);
      info << "object at " << mo->address 
           << " of size " << mo->size << "\n"
           << "\t\t" << alloc_info << "\n";
    }
  }

  return info.str();
}

void Executor::terminateState(ExecutionState &state) {
    if (replayOut && replayPosition!=replayOut->numObjects) {
      klee_warning_once(replayOut,
                        "replay did not consume all objects in test input.");
    }

	interpreterHandler->incPathsExplored();

	std::set<ExecutionState*>::iterator it = addedStates.find(&state);
	if (it == addedStates.end()) {
	  state.pc() = state.prevPC();

	  removedStates.insert(&state);
	} else {
		// never reached searcher, just delete immediately
		std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 =
				seedMap.find(&state);
		if (it3 != seedMap.end())
			seedMap.erase(it3);
		addedStates.erase(it);
		processTree->remove(state.ptreeNode);
		delete &state;
	}
}

void Executor::terminateStateEarly(ExecutionState &state, 
                                   const Twine &message) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
  terminateState(state);
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew || 
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, 0, 0);
  terminateState(state);
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     const char *suffix,
                                     const llvm::Twine &info) {
  std::string message = messaget.str();
  static std::set< std::pair<Instruction*, std::string> > emittedErrors;

  assert(state.crtThreadIt != state.threads.end());

  const InstructionInfo &ii = *state.prevPC()->info;

  if (EmitAllErrors ||
      emittedErrors.insert(std::make_pair(state.prevPC()->inst, message)).second) {
    if (ii.file != "") {
      klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line, message.c_str());
    } else {
      klee_message("ERROR: %s", message.c_str());
    }
    if (!EmitAllErrors)
      klee_message("NOTE: now ignoring this error at this location");

    std::ostringstream msg;
    msg << "Error: " << message << "\n";
    if (ii.file != "") {
      msg << "File: " << ii.file << "\n";
      msg << "Line: " << ii.line << "\n";
    }
    msg << "Stack: \n";
    state.crtThread().getStackTrace().dump(msg);

    std::string info_str = info.str();
    if (info_str != "")
      msg << "Info: \n" << info_str;
    interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
  }
    
  terminateState(state);
}

// XXX shoot me
static const char *okExternalsList[] = { "printf", 
                                         "fprintf", 
                                         "puts",
                                         "getpid" };
static std::set<std::string> okExternals(okExternalsList,
                                         okExternalsList + 
                                         (sizeof(okExternalsList)/sizeof(okExternalsList[0])));

void Executor::callExternalFunction(ExecutionState &state,
                                    KInstruction *target,
                                    Function *function,
                                    std::vector< ref<Expr> > &arguments) {
  // check if specialFunctionHandler wants it
  if (specialFunctionHandler->handle(state, function, target, arguments))
    return;
  
  callUnmodelledFunction(state, target, function, arguments);
}

void Executor::callUnmodelledFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            std::vector<ref<Expr> > &arguments) {

  if (NoExternals && !okExternals.count(function->getName())) {
    std::cerr << "KLEE:ERROR: Calling not-OK external function : " 
              << function->getName().str() << "\n";
    terminateStateOnError(state, "externals disallowed", "user.err");
    return;
  }

  // normal external function handling path
  // allocate 128 bits for each argument (+return value) to support fp80's;
  // we could iterate through all the arguments first and determine the exact
  // size we need, but this is faster, and the memory usage isn't significant.
  uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
  memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
  unsigned wordIndex = 2;
  for (std::vector<ref<Expr> >::iterator ai = arguments.begin(), 
       ae = arguments.end(); ai!=ae; ++ai) {
    if (AllowExternalSymCalls) { // don't bother checking uniqueness
      ref<ConstantExpr> ce;
      bool success = solver->getValue(state, *ai, ce);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      ce->toMemory(&args[wordIndex]);
      wordIndex += (ce->getWidth()+63)/64;
    } else {
      ref<Expr> arg = toUnique(state, *ai);
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
        // XXX kick toMemory functions from here
        ce->toMemory(&args[wordIndex]);
        wordIndex += (ce->getWidth()+63)/64;
      } else {
        terminateStateOnExecError(state, 
                                  "external call with symbolic argument: " + 
                                  function->getName());
        return;
      }
    }
  }

  bool isReadNone = function->hasFnAttr(Attribute::ReadNone),
       isReadOnly = function->hasFnAttr(Attribute::ReadOnly);

  if (!isReadNone)
    state.addressSpace().copyOutConcretes(&state.addressPool);

  if (!SuppressExternalWarnings) {
    std::ostringstream os;
    os << "calling external: " << function->getName().str() << "(";
    for (unsigned i=0; i<arguments.size(); i++) {
      os << arguments[i];
      if (i != arguments.size()-1)
    os << ", ";
    }
    os << ")";
    
    if (AllExternalWarnings)
      klee_warning("%s", os.str().c_str());
    else
      klee_warning_once(function, "%s", os.str().c_str());
  }
  
  bool success = externalDispatcher->executeCall(function, target->inst, args);
  if (!success) {
    terminateStateOnError(state, "failed external call: " + function->getName(),
                          "external.err");
    return;
  }

  if (!isReadNone && !isReadOnly) {
    if (!state.addressSpace().copyInConcretes(&state.addressPool)) {
      terminateStateOnError(state, "external modified read-only object",
                            "external.err");
      return;
    }
  }

  LLVM_TYPE_Q Type *resultType = target->inst->getType();
  if (resultType != Type::getVoidTy(getGlobalContext())) {
    ref<Expr> e = ConstantExpr::fromMemory((void*) args, 
                                           getWidthForLLVMType(kmodule(state), resultType));
    bindLocal(target, state, e);
  }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state, 
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayOut || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason too?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() %  n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.
  
  static unsigned id;
  const Array *array = new Array("rrws_arr" + llvm::utostr(++id), 
                                 Expr::getMinBytesForWidth(e->getWidth()));
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  std::cerr << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);
  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state, 
                                         unsigned addrspace,
                                         const MemoryObject *mo,
                                         bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace(addrspace).bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    state.stack().back().allocas.push_back(mo);

  return os;
}

/// Similar to Executor::bindObjectInState, but binds in all "variants"
/// of the given address space (within the current process, where possible).
/// A list of all ObjectStates created is returned through states.
void Executor::bindAllObjectStates(ExecutionState &state, 
                                   unsigned addrspace,
                                   const MemoryObject *mo,
                                   bool isLocal,
                                   std::vector<ObjectState *> &states,
                                   const Array *array) {
  std::vector<AddressSpace *> addrspaces;
  switch (addrspace) {
  case 0:
    addrspaces.push_back(&state.crtProcess().addressSpace);
    break;
  case 1:
    addrspaces = state.wgAddressSpaces;
    break;
  case 4: {
    std::set<thread_uid_t> &thrs = state.crtProcess().threads;
    for (std::set<thread_uid_t>::iterator i = thrs.begin(), e = thrs.end();
         i != e; ++i) {
      ExecutionState::threads_ty::iterator thrIt = state.threads.find(*i);
      addrspaces.push_back(&thrIt->second.threadLocalAddressSpace);
    }
    break;
  }
  }

  for (std::vector<AddressSpace *>::iterator i = addrspaces.begin(),
       e = addrspaces.end(); i != e; ++i) {
    ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
    (*i)->bindObject(mo, os);
    states.push_back(os);
  }

  if (isLocal)
    state.stack().back().allocas.push_back(mo);
}

void Executor::executeAlloc(ExecutionState &state,
                            ref<Expr> size,
                            bool isLocal,
                            KInstruction *target,
                            unsigned addrspace,
                            bool zeroMemory,
                            const ObjectState *reallocFrom) {
  size = toUnique(state, size);
  assert(isa<PointerType>(target->inst->getType()) && "alloc nonpointer type?");
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    MemoryObject *mo = memory->allocate(&state, CE->getZExtValue(), isLocal, false,
                                        state.prevPC()->inst);
    if (!mo) {
      bindLocal(target, state, 
                ConstantExpr::alloc(0, Context::get().getPointerWidth()));
    } else {
      std::vector<ObjectState *> states;
      bindAllObjectStates(state, addrspace, mo, isLocal, states);
      for (std::vector<ObjectState *>::iterator i = states.begin(),
           e = states.end(); i != e; ++i) {
        if (zeroMemory) {
          (*i)->initializeToZero();
        } else {
          (*i)->initializeToRandom();
        }
      }
      bindLocal(target, state, mo->getBaseExpr());
      
      if (reallocFrom) {
        assert(states.size() == 1 && "realloc not supported in this addrspace");
        ObjectState *os = states[0];
        unsigned count = std::min(reallocFrom->size, os->size);
        for (unsigned i=0; i<count; i++)
          os->write(i, reallocFrom->read8(i, &state, solver), &state, solver);
        state.addressSpace(addrspace).unbindObject(reallocFrom->getObject());
      }
    }
  } else {
    // XXX For now we just pick a size. Ideally we would support
    // symbolic sizes fully but even if we don't it would be better to
    // "smartly" pick a value, for example we could fork and pick the
    // min and max values and perhaps some intermediate (reasonable
    // value).
    // 
    // It would also be nice to recognize the case when size has
    // exactly two values and just fork (but we need to get rid of
    // return argument first). This shows up in pcre when llvm
    // collapses the size expression with a select.

    ref<ConstantExpr> example;
    bool success = solver->getValue(state, size, example);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    
    // Try and start with a small example.
    Expr::Width W = example->getWidth();
    while (example->Ugt(ConstantExpr::alloc(128, W))->isTrue()) {
      ref<ConstantExpr> tmp = example->LShr(ConstantExpr::alloc(1, W));
      bool res;
      bool success = solver->mayBeTrue(state, EqExpr::create(tmp, size), res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (!res)
        break;
      example = tmp;
    }

    StatePair fixedSize = fork(state, EqExpr::create(example, size), true, KLEE_FORK_INTERNAL);
    
    if (fixedSize.second) { 
      // Check for exactly two values
      ref<ConstantExpr> tmp;
      bool success = solver->getValue(*fixedSize.second, size, tmp);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      bool res;
      success = solver->mustBeTrue(*fixedSize.second, 
                                   EqExpr::create(tmp, size),
                                   res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (res) {
        executeAlloc(*fixedSize.second, tmp, isLocal,
                     target, addrspace, zeroMemory, reallocFrom);
      } else {
        // See if a *really* big value is possible. If so assume
        // malloc will fail for it, so lets fork and return 0.
        StatePair hugeSize = 
          fork(*fixedSize.second, 
               UltExpr::create(ConstantExpr::alloc(1<<31, W), size), 
               true, KLEE_FORK_INTERNAL);
        if (hugeSize.first) {
          klee_message("NOTE: found huge malloc, returning 0");
          bindLocal(target, *hugeSize.first, 
                    ConstantExpr::alloc(0, Context::get().getPointerWidth()));
        }
        
        if (hugeSize.second) {
          std::ostringstream info;
          ExprPPrinter::printOne(info, "  size expr", size);
          info << "  concretization : " << example << "\n";
          info << "  unbound example: " << tmp << "\n";
          terminateStateOnError(*hugeSize.second, 
                                "concretized symbolic size", 
                                "model.err", 
                                info.str());
        }
      }
    }

    if (fixedSize.first) // can be zero when fork fails
      executeAlloc(*fixedSize.first, example, isLocal, 
                   target, addrspace, zeroMemory, reallocFrom);
  }
}

void Executor::executeFree(ExecutionState &state,
                           ref<Expr> address,
                           KInstruction *target) {
  StatePair zeroPointer = fork(state, Expr::createIsZero(address), true, KLEE_FORK_INTERNAL);
  if (zeroPointer.first) {
    if (target)
      bindLocal(target, *zeroPointer.first, Expr::createPointer(0));
  }
  if (zeroPointer.second) { // address != 0
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, address, rl, "free");
    
    for (Executor::ExactResolutionList::iterator it = rl.begin(), 
           ie = rl.end(); it != ie; ++it) {
      const MemoryObject *mo = it->first.first;
      if (mo->isLocal) {
        terminateStateOnError(*it->second, 
                              "free of alloca", 
                              "free.err",
                              getAddressInfo(*it->second, address));
      } else if (mo->isGlobal) {
        terminateStateOnError(*it->second, 
                              "free of global", 
                              "free.err",
                              getAddressInfo(*it->second, address));
      } else {
        it->second->addressSpace().unbindObject(mo);
        if (target)
          bindLocal(target, *it->second, Expr::createPointer(0));
      }
    }
  }
}

void Executor::resolveExact(ExecutionState &state,
                            ref<Expr> p,
                            ExactResolutionList &results,
                            const std::string &name) {
  // XXX we may want to be capping this?
  ResolutionList rl;
  state.addressSpace().resolve(state, solver, p, rl);
  
  ExecutionState *unbound = &state;
  for (ResolutionList::iterator it = rl.begin(), ie = rl.end(); 
       it != ie; ++it) {
    ref<Expr> inBounds = EqExpr::create(p, it->first->getBaseExpr());
    
    /*
     *  Assume branches.first is path where inBounds is true
     *  Assume branches.second is path where inBound is false
     *  Is this correct with the randomised swapping in fork() ?
     *
     *  Notice that that "unbound" is being reused in the loop.
     *  Unbound will accumulate a constraint (p does not point
     *  to a particular MemoryObject) on every iteration.
     */
    StatePair branches = fork(*unbound, inBounds, true, KLEE_FORK_INTERNAL);
    
    if (branches.first)
      results.push_back(std::make_pair(*it, branches.first));

    unbound = branches.second;
    if (!unbound) // Fork failure
    {
        // A state does not exist where inBounds is false.
        // No need to search for more states where p could
        // point to other MemoryObjects.
        break;
    }
  }

  // If we've finished looping through all memory objects
  // and a state exists where p does not point to any of those
  // objects then the pointer can point to an invalid point in
  // memory.
  if (unbound) {
    terminateStateOnError(*unbound,
                          "memory error: invalid pointer: " + name,
                          "ptr.err",
                          getAddressInfo(*unbound, p));
  }
}

//pthread handlers
void Executor::executeThreadCreate(ExecutionState &state, thread_id_t tid,
				     ref<Expr> start_function, ref<Expr> arg)
{
  assert(isa<ConstantExpr>(start_function) && "start_function non-constant");
  Function *f = (Function *) cast<ConstantExpr>(start_function)->getZExtValue();
  unsigned moduleId;
  KFunction *kf = getKFunction(f, moduleId);
  assert(kf && "cannot resolve thread start function");

  Thread &t = state.createThread(tid, kf, moduleId);
  bindGlobalsInNewAddressSpace(state, 4, t.threadLocalAddressSpace);
 
  bindArgumentToPthreadCreate(kf, 0, t.stack.back(), arg);

  if (statsTracker)
    statsTracker->framePushed(&t.stack.back(), 0);
}

void Executor::executeThreadExit(ExecutionState &state) {
  //terminate this thread and schedule another one

  if (state.threads.size() == 1) {
    klee_message("terminating state");
    terminateStateOnExit(state);
    return;
  }

  assert(state.threads.size() > 1);

  ExecutionState::threads_ty::iterator thrIt = state.crtThreadIt;
  thrIt->second.enabled = false;

  if (!schedule(state, false))
    return;

  state.terminateThread(thrIt);
}

void Executor::executeProcessExit(ExecutionState &state) {
  if (state.processes.size() == 1) {
    terminateStateOnExit(state);
    return;
  }

  ExecutionState::processes_ty::iterator procIt = state.crtProcessIt;

  // Disable all the threads of the current process
  for (std::set<thread_uid_t>::iterator it = procIt->second.threads.begin();
      it != procIt->second.threads.end(); it++) {
    ExecutionState::threads_ty::iterator thrIt = state.threads.find(*it);

    if (thrIt->second.enabled) {
      // Disable any enabled thread
      thrIt->second.enabled = false;
    } else {
      // If the thread is disabled, remove it from any waiting list
      wlist_id_t wlist = thrIt->second.waitingList;

      if (wlist > 0) {
        state.waitingLists[wlist].erase(thrIt->first);
        if (state.waitingLists[wlist].size() == 0)
          state.waitingLists.erase(wlist);

        thrIt->second.waitingList = 0;
      }
    }
  }

  if (!schedule(state, false))
    return;

  state.terminateProcess(procIt);
}

void Executor::executeProcessFork(ExecutionState &state, KInstruction *ki,
    process_id_t pid) {

  Thread &pThread = state.crtThread();

  Process &child = state.forkProcess(pid);

  Thread &cThread = state.threads.find(*child.threads.begin())->second;

  // Set return value in the child
  state.scheduleNext(state.threads.find(cThread.tuid));
  bindLocal(ki, state, ConstantExpr::create(0,
      getWidthForLLVMType(kmodule(state), ki->inst->getType())));

  // Set return value in the parent
  state.scheduleNext(state.threads.find(pThread.tuid));
  bindLocal(ki, state, ConstantExpr::create(child.pid,
      getWidthForLLVMType(kmodule(state), ki->inst->getType())));
}

void Executor::executeFork(ExecutionState &state, KInstruction *ki, int reason) {
  // Check to see if we really should fork
  if (reason == KLEE_FORK_DEFAULT) {
    StatePair sp = fork(state, reason);

    // Return 0 in the original
    bindLocal(ki, *sp.first, ConstantExpr::create(0,
        getWidthForLLVMType(kmodule(state), ki->inst->getType())));

    // Return 1 otherwise
    bindLocal(ki, *sp.second, ConstantExpr::create(1,
        getWidthForLLVMType(kmodule(state), ki->inst->getType())));
  } else {
    bindLocal(ki, state, ConstantExpr::create(0,
        getWidthForLLVMType(kmodule(state), ki->inst->getType())));
  }
}


bool Executor::schedule(ExecutionState &state, bool yield) {

  int enabledCount = 0;
  for(ExecutionState::threads_ty::iterator it = state.threads.begin();
      it != state.threads.end();  it++) {
    if(it->second.enabled) {
      enabledCount++;
    }
  }
  
  //CLOUD9_DEBUG("Scheduling " << state.threads.size() << " threads (" <<
  //    enabledCount << " enabled) in " << state.processes.size() << " processes ...");

  if (enabledCount == 0) {
    terminateStateOnError(state, " ******** hang (possible deadlock?)", "user.err");
    return false;
  }
  
  bool forkSchedule = false;
  bool incPreemptions = false;

  ExecutionState::threads_ty::iterator oldIt = state.crtThreadIt;

  if(!state.crtThread().enabled || yield) {
    ExecutionState::threads_ty::iterator it = state.nextThread(state.crtThreadIt);

    while (!it->second.enabled)
      it = state.nextThread(it);

    state.scheduleNext(it);

    if (ForkOnSchedule)
      forkSchedule = true;
  } else {
    if (state.preemptions < MaxPreemptions) {
      forkSchedule = true;
      incPreemptions = true;
    }
  }

  if (forkSchedule) {
    ExecutionState::threads_ty::iterator finalIt = state.crtThreadIt;
    ExecutionState::threads_ty::iterator it = state.nextThread(finalIt);
    ExecutionState *lastState = &state;

    ForkClass forkClass = KLEE_FORK_SCHEDULE;

    while (it != finalIt) {
      // Choose only enabled states, and, in the case of yielding, do not
      // reschedule the same thread
      if (it->second.enabled && (!yield || it != oldIt)) {
        StatePair sp = fork(*lastState, forkClass);

        if (incPreemptions)
          sp.first->preemptions = state.preemptions + 1;

        sp.first->scheduleNext(sp.first->threads.find(it->second.tuid));

        lastState = sp.first;

        if (forkClass == KLEE_FORK_SCHEDULE) {
          forkClass = KLEE_FORK_MULTI;   // Avoid appearing like multiple schedules
        }
      }

      it = state.nextThread(it);
    }
  }

  return true;
}

void Executor::executeThreadNotifyOne(ExecutionState &state, wlist_id_t wlist) {
  // Copy the waiting list
  std::set<thread_uid_t> wl = state.waitingLists[wlist];

  if (!ForkOnSchedule || wl.size() <= 1) {
    if (wl.size() == 0)
      state.waitingLists.erase(wlist);
    else
      state.notifyOne(wlist, *wl.begin()); // Deterministically pick the first thread in the queue

    return;
  }

  ExecutionState *lastState = &state;

  for (std::set<thread_uid_t>::iterator it = wl.begin(); it != wl.end();) {
    thread_uid_t tuid = *it++;

    if (it != wl.end()) {
      StatePair sp = fork(*lastState, KLEE_FORK_SCHEDULE);

      sp.second->notifyOne(wlist, tuid);

      lastState = sp.first;
    } else {
      lastState->notifyOne(wlist, tuid);
    }
  }
}


void Executor::executeMemoryOperation(ExecutionState &state,
                                      bool isWrite,
                                      unsigned addrspace,
                                      ref<Expr> address,
                                      ref<Expr> value /* undef if read */,
                                      KInstruction *target /* undef if write */) {
  Expr::Width type = (isWrite ? value->getWidth() : 
                     getWidthForLLVMType(kmodule(state), target->inst->getType()));
  unsigned bytes = Expr::getMinBytesForWidth(type);

  if (!state.watchpoint.isNull() && isWrite) {
    if (address == state.watchpoint) {
      puts("Hit watchpoint, value = ");
      value->dump();
    } else if (isa<ConstantExpr>(state.watchpoint) && isa<ConstantExpr>(address)) {
      uint64_t wpConst = cast<ConstantExpr>(state.watchpoint)->getZExtValue(),
               adConst = cast<ConstantExpr>(address)->getZExtValue();
      if (wpConst + state.watchpointSize >= adConst && wpConst < adConst + value->getWidth()/8) {
        printf("Hit watchpoint (inexact), wp addr = %lu, wr addr = %lu, value =\n", wpConst, adConst);
        value->dump();
      }
    }
  }

  if (SimplifySymIndices) {
    if (!isa<ConstantExpr>(address))
      address = state.constraints().simplifyExpr(address);
    if (isWrite && !isa<ConstantExpr>(value))
      value = state.constraints().simplifyExpr(value);
  }

  // fast path: single in-bounds resolution
  ObjectPair op;
  bool success;
  solver->setTimeout(stpTimeout);
  if (!state.addressSpace(addrspace).resolveOne(state, solver, address, op, success)) {
    address = toConstant(state, address, "resolveOne failure");
    success = state.addressSpace(addrspace).resolveOne(cast<ConstantExpr>(address), op);
  }
  solver->setTimeout(0);

  if (success) {
    const MemoryObject *mo = op.first;

    if (MaxSymArraySize && mo->size>=MaxSymArraySize) {
      address = toConstant(state, address, "max-sym-array-size");
    }
    
    ref<Expr> offset = mo->getOffsetExpr(address);

    bool inBounds;
    solver->setTimeout(stpTimeout);
    bool success = solver->mustBeTrue(state, 
                                      mo->getBoundsCheckOffset(offset, bytes),
                                      inBounds);
    solver->setTimeout(0);
    if (!success) {
      state.pc() = state.prevPC();
      terminateStateEarly(state, "query timed out");
      return;
    }

    if (inBounds) {
      const ObjectState *os = op.second;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(state,
                                "memory error: object read only",
                                "readonly.err");
        } else {
          ObjectState *wos = state.addressSpace(addrspace).getWriteable(mo, os);
          wos->write(offset, value, &state, solver);

	}          
      } else {
	ref<Expr> result = os->read(offset, type, &state, solver);

        if (interpreterOpts.MakeConcreteSymbolic)
          result = replaceReadWithSymbolic(state, result);
        
        bindLocal(target, state, result);
      }

      return;
    }
  } 

  // we are on an error path (no resolution, multiple resolution, one
  // resolution with out of bounds)
  
  ResolutionList rl;  
  solver->setTimeout(stpTimeout);
  bool incomplete = state.addressSpace(addrspace).resolve(state, solver, address, rl,
                                               0, stpTimeout);
  solver->setTimeout(0);
  
  // XXX there is some query wasteage here. who cares?
  ExecutionState *unbound = &state;
  
  for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
    const MemoryObject *mo = i->first;
    const ObjectState *os = i->second;
    ref<Expr> inBounds = mo->getBoundsCheckPointer(address, bytes);
    
    StatePair branches = fork(*unbound, inBounds, true, KLEE_FORK_INTERNAL);
    ExecutionState *bound = branches.first;

    // bound can be 0 on failure or overlapped 
    if (bound) {
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(*bound,
                                "memory error: object read only",
                                "readonly.err");
        } else {
          ObjectState *wos = bound->addressSpace(addrspace).getWriteable(mo, os);
          wos->write(mo->getOffsetExpr(address), value, &state, solver);
        }
      } else {
        ref<Expr> result = os->read(mo->getOffsetExpr(address), type, &state, solver);
        bindLocal(target, *bound, result);
      }
    }

    unbound = branches.second;
    if (!unbound)
      break;
  }
  
  // XXX should we distinguish out of bounds and overlapped cases?
  if (unbound) {
    if (incomplete) {
      terminateStateEarly(*unbound, "query timed out (resolve)");
    } else {
      terminateStateOnError(*unbound,
                            "memory error: out of bound pointer",
                            "ptr.err",
                            getAddressInfo(*unbound, address));
    }
  }
}

void Executor::executeMakeSymbolic(ExecutionState &state, 
                                   const MemoryObject *mo,
                                   const std::string &name,
                                   bool shared) {
  // Create a new object state for the memory object (instead of a copy).
  if (!replayOut) {
    // Find a unique name for this array.  First try the original name,
    // or if that fails try adding a unique identifier.
    unsigned id = 0;
    std::string uniqueName = name;
    while (!state.arrayNames.insert(uniqueName).second) {
      uniqueName = name + "_" + llvm::utostr(++id);
    }
    const Array *array = new Array(uniqueName, mo->size);
    ObjectState *os = bindObjectInState(state, 0, mo, false, array);
    os->isShared = shared;

    state.addSymbolic(mo, array);
    
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
      seedMap.find(&state);
    if (it!=seedMap.end()) { // In seed mode we need to add this as a
                             // binding.
      for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
             siie = it->second.end(); siit != siie; ++siit) {
        SeedInfo &si = *siit;
        KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

        if (!obj) {
          if (ZeroSeedExtension) {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values = std::vector<unsigned char>(mo->size, '\0');
          } else if (!AllowSeedExtension) {
            terminateStateOnError(state, 
                                  "ran out of inputs during seeding",
                                  "user.err");
            break;
          }
        } else {
          if (obj->numBytes != mo->size &&
              ((!(AllowSeedExtension || ZeroSeedExtension)
                && obj->numBytes < mo->size) ||
               (!AllowSeedTruncation && obj->numBytes > mo->size))) {
	    std::stringstream msg;
	    msg << "replace size mismatch: "
		<< mo->name << "[" << mo->size << "]"
		<< " vs " << obj->name << "[" << obj->numBytes << "]"
		<< " in test\n";

            terminateStateOnError(state,
                                  msg.str(),
                                  "user.err");
            break;
          } else {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values.insert(values.begin(), obj->bytes, 
                          obj->bytes + std::min(obj->numBytes, mo->size));
            if (ZeroSeedExtension) {
              for (unsigned i=obj->numBytes; i<mo->size; ++i)
                values.push_back('\0');
            }
          }
        }
      }
    }
  } else {
    ObjectState *os = bindObjectInState(state, 0, mo, false);
    if (replayPosition >= replayOut->numObjects) {
      terminateStateOnError(state, "replay count mismatch", "user.err");
    } else {
      KTestObject *obj = &replayOut->objects[replayPosition++];
      if (obj->numBytes != mo->size) {
        terminateStateOnError(state, "replay size mismatch", "user.err");
      } else {
        for (unsigned i=0; i<mo->size; i++)
          os->write8(i, obj->bytes[i], &state, solver);
      }
    }
  }
}

/***/

void Executor::runFunctionAsMain(Function *f,
				 int argc,
				 char **argv,
				 char **envp) {
	std::vector<ref<Expr> > arguments;

	// force deterministic initialization of memory objects
	srand(1);
	srandom(1);

        unsigned moduleId;
        KFunction *kf = getKFunction(f, moduleId);
	assert(kf);
	ExecutionState *state = new ExecutionState(kf, moduleId);

	MemoryObject *argvMO = 0;

	// In order to make uclibc happy and be closer to what the system is
	// doing we lay out the environments at the end of the argv array
	// (both are terminated by a null). There is also a final terminating
	// null that uclibc seems to expect, possibly the ELF header?

	int envc;
	for (envc = 0; envp[envc]; ++envc)
		;

	unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
	Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
	if (ai != ae) {
		arguments.push_back(ConstantExpr::alloc(argc, Expr::Int32));

		if (++ai != ae) {
			argvMO = memory->allocate(state, (argc + 1 + envc + 1 + 1) * NumPtrBytes,
					false, true, f->begin()->begin());

			arguments.push_back(argvMO->getBaseExpr());

			if (++ai != ae) {
				uint64_t envp_start = argvMO->address + (argc + 1)
						* NumPtrBytes;
				arguments.push_back(Expr::createPointer(envp_start));

				if (++ai != ae)
					klee_error("invalid main function (expect 0-3 arguments)");
			}
		}
	}

	if (pathWriter)
		state->pathOS = pathWriter->open();
	if (symPathWriter)
		state->symPathOS = symPathWriter->open();

	if (statsTracker)
		statsTracker->framePushed(*state, 0);

	assert(arguments.size() == f->arg_size() && "wrong number of arguments");
	for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
		bindArgument(kf, i, *state, arguments[i]);

	if (argvMO) {
		ObjectState *argvOS = bindObjectInState(*state, 0, argvMO, false);

		for (int i = 0; i < argc + 1 + envc + 1 + 1; i++) {
			MemoryObject *arg;

			if (i == argc || i >= argc + 1 + envc) {
				arg = 0;
			} else {
				char *s = i < argc ? argv[i] : envp[i - (argc + 1)];
				int j, len = strlen(s);

				arg = memory->allocate(state, len + 1, false, true, state->pc()->inst);
				ObjectState *os = bindObjectInState(*state, 0, arg, false);
				for (j = 0; j < len + 1; j++)
                                  os->write8(j, s[j], state, solver);
			}

			if (arg) {
				argvOS->write(i * NumPtrBytes, arg->getBaseExpr(), state, solver);
			} else {
				argvOS->write(i * NumPtrBytes, Expr::createPointer(0), state, solver);
			}
		}
	}
  
  initializeGlobals(*state);
  initializeExternals(*state);

  processTree = new PTree(state);
  state->ptreeNode = processTree->root;
  run(*state);
  delete processTree;
  processTree = 0;

  // hack to clear memory objects
  delete memory;
  memory = new MemoryManager();
  
  globalObjects.clear();
  globalAddresses.clear();

  if (statsTracker)
    statsTracker->done();
}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state,
                                std::string &res,
                                Interpreter::LogType logFormat) {

  std::ostringstream info;

  switch(logFormat)
  {
  case STP:
  {
	  Query query(state.constraints(), ConstantExpr::alloc(0, Expr::Bool));
	  char *log = solver->stpSolver->getConstraintLog(query);
	  res = std::string(log);
	  free(log);
  }
	  break;

  case KQUERY:
  {
	  std::ostringstream info;
	  ExprPPrinter::printConstraints(info, state.constraints());
	  res = info.str();
  }
	  break;

  case SMTLIB2:
  {
	  std::ostringstream info;
	  ExprSMTLIBPrinter* printer = createSMTLIBPrinter();
	  printer->setOutput(info);
	  Query query(state.constraints(), ConstantExpr::alloc(0, Expr::Bool));
	  printer->setQuery(query);
	  printer->generateOutput();
	  res = info.str();
	  delete printer;
  }
	  break;

  default:
	  klee_warning("Executor::getConstraintLog() : Log format not supported!");
  }

}

bool Executor::getSymbolicSolution(const ExecutionState &state,
                                   std::vector< 
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res) {
  solver->setTimeout(stpTimeout);

  ExecutionState tmp(state);
  if (!NoPreferCex) {
    for (unsigned i = 0; i != state.symbolics.size(); ++i) {
      const MemoryObject *mo = state.symbolics[i].first;
      std::vector< ref<Expr> >::const_iterator pi = 
        mo->cexPreferences.begin(), pie = mo->cexPreferences.end();
      for (; pi != pie; ++pi) {
        bool mustBeTrue;
        bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi), 
                                          mustBeTrue);
        if (!success) break;
        if (!mustBeTrue) tmp.addConstraint(*pi);
      }
      if (pi!=pie) break;
    }
  }

  std::vector< std::vector<unsigned char> > values;
  std::vector<const Array*> objects;
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    objects.push_back(state.symbolics[i].second);
  bool success = solver->getInitialValues(tmp, objects, values);
  solver->setTimeout(0);
  if (!success) {
    klee_warning("unable to compute initial values (invalid constraints?)!");
    ExprPPrinter::printQuery(std::cerr,
                             state.constraints(),
                             ConstantExpr::alloc(0, Expr::Bool));
    return false;
  }
  
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    res.push_back(std::make_pair(state.symbolics[i].first->name, values[i]));
  return true;
}

void Executor::getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state,
                                            ref<Expr> e,
                                            ref<ConstantExpr> value) {
  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; //re->updates.root->object;
      const ObjectState *os = state.addressSpace().findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly && 
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace().getWriteable(mo, os);
        wos->write(CE, it->second, &state, solver);
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(const KModule *kmodule,
                                          LLVM_TYPE_Q llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

///

Interpreter *Interpreter::create(const InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(opts, ih);
}

//}

