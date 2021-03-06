/*******************************************************************************
 * Copyright (C) 2010 Dependable Systems Laboratory, EPFL
 *
 * This file is part of the Cloud9-specific extensions to the KLEE symbolic
 * execution engine.
 *
 * Do NOT distribute this file to any third party; it is part of
 * unpublished work.
 *
 *******************************************************************************
 * Threading.cpp
 *
 *  Created on: Jul 22, 2010
 *  File owner: Stefan Bucur <stefan.bucur@epfl.ch>
 ******************************************************************************/

#include "klee/Threading.h"
#include "klee/ExecutionState.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Module/Cell.h"

#include "llvm/Function.h"

namespace klee {

/* StackFrame Methods */

StackFrame::StackFrame(KInstIterator _caller, KFunction *_kf, unsigned _moduleId)
  : caller(_caller), kf(_kf), moduleId(_moduleId), callPathNode(0),
    minDistToUncoveredOnReturn(0), varargs(0) {
  locals = new Cell[kf->numRegisters];
}

StackFrame::StackFrame(const StackFrame &s)
  : caller(s.caller),
    kf(s.kf),
    moduleId(s.moduleId),
    callPathNode(s.callPathNode),
    allocas(s.allocas),
    minDistToUncoveredOnReturn(s.minDistToUncoveredOnReturn),
    varargs(s.varargs) {
  locals = new Cell[s.kf->numRegisters];
  for (unsigned i=0; i<s.kf->numRegisters; i++)
    locals[i] = s.locals[i];
}

StackFrame& StackFrame::operator=(const StackFrame &s) {
  if (this != &s) {
    caller = s.caller;
    kf = s.kf;
    callPathNode = s.callPathNode;
    allocas = s.allocas;
    minDistToUncoveredOnReturn = s.minDistToUncoveredOnReturn;
    varargs = s.varargs;

    if (locals)
      delete []locals;

    locals = new Cell[s.kf->numRegisters];
    for (unsigned i=0; i<s.kf->numRegisters; i++)
        locals[i] = s.locals[i];
  }

  return *this;
}

StackFrame::~StackFrame() {
  delete[] locals;
}

/* Thread class methods */

Thread::Thread(thread_id_t tid, process_id_t pid, KFunction * kf, unsigned moduleId) :
  workgroupId(0), enabled(true), waitingList(0) {

  tuid = std::make_pair(tid, pid);

  if (kf) {
    stack.push_back(StackFrame(0, kf, moduleId));

    pc = kf->instructions;
    prevPC = pc;
  }

}

StackTrace Thread::getStackTrace() const {
  StackTrace result;

  const KInstruction *target = prevPC;

  for (ExecutionState::stack_ty::const_reverse_iterator
         it = stack.rbegin(), ie = stack.rend();
       it != ie; ++it) {

    const StackFrame &sf = *it;

    StackTrace::position_t position = std::make_pair(sf.kf, target);
    std::vector<ref<Expr> > arguments;

    Function *f = sf.kf->function;
    unsigned index = 0;
    for (Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
         ai != ae; ++ai) {

      ref<Expr> value = sf.locals[sf.kf->getArgRegister(index++)].value;
      arguments.push_back(value);
    }

    result.contents.push_back(std::make_pair(position, arguments));

    target = sf.caller;
  }

  return result;
}

void Thread::dumpStackTrace() const {
 getStackTrace().dump(std::cerr);
}

}
