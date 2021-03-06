//===-- Memory.cpp --------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Common.h"

#include "Memory.h"

#include "Context.h"
#include "TimingSolver.h"
#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Solver.h"
#include "klee/util/BitArray.h"

#include "ObjectHolder.h"

#include <llvm/Function.h>
#include <llvm/Instruction.h>
#include <llvm/Value.h>
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <cassert>
#include <sstream>

using namespace llvm;
using namespace klee;

namespace {
  cl::opt<bool>
  UseConstantArrays("use-constant-arrays",
                    cl::init(true));
}

/***/

ObjectHolder::ObjectHolder(const ObjectHolder &b) : os(b.os) { 
  if (os) ++os->refCount; 
}

ObjectHolder::ObjectHolder(ObjectState *_os) : os(_os) { 
  if (os) ++os->refCount; 
}

ObjectHolder::~ObjectHolder() { 
  if (os && --os->refCount==0) delete os; 
}
  
ObjectHolder &ObjectHolder::operator=(const ObjectHolder &b) {
  if (b.os) ++b.os->refCount;
  if (os && --os->refCount==0) delete os;
  os = b.os;
  return *this;
}

/***/

int MemoryObject::counter = 0;

MemoryObject::~MemoryObject() {
}

void MemoryObject::getAllocInfo(std::string &result) const {
  llvm::raw_string_ostream info(result);

  info << "MO" << id << "[" << size << "]";

  if (allocSite) {
    info << " allocated at ";
    if (const Instruction *i = dyn_cast<Instruction>(allocSite)) {
      info << i->getParent()->getParent()->getName() << "():";
      info << *i;
    } else if (const GlobalValue *gv = dyn_cast<GlobalValue>(allocSite)) {
      info << "global:" << gv->getName();
    } else {
      info << "value:" << *allocSite;
    }
  } else {
    info << " (no allocation info)";
  }
  
  info.flush();
}

/***/

unsigned MemoryLog::logId = 0;

MemoryLog::MemoryLog(unsigned size) : size(size), updates(0) {}

MemoryLog::MemoryLog(const MemoryLog &that)
  : size(that.size),
    concreteEntries(that.concreteEntries),
    updates(that.updates ? new MemoryLogUpdates(*that.updates) : 0) {}

MemoryLog::~MemoryLog() {
  delete updates;
}

void MemoryLog::makeSymbolic() {
  if (isSymbolic())
    return;

  std::vector<ref<ConstantExpr> > threadId, wgid, read, write, manyRead,
                                  wgManyRead;
  for (std::vector<MemoryLogEntry>::iterator i = concreteEntries.begin(),
       e = concreteEntries.end(); i != e; ++i) {
    threadId.push_back(ConstantExpr::create(i->threadId, Expr::Int32));
    wgid.push_back(ConstantExpr::create(i->wgid, Expr::Int32));
    read.push_back(ConstantExpr::create(i->read, Expr::Bool));
    write.push_back(ConstantExpr::create(i->write, Expr::Bool));
    manyRead.push_back(ConstantExpr::create(i->manyRead, Expr::Bool));
    wgManyRead.push_back(ConstantExpr::create(i->wgManyRead, Expr::Bool));
  }

  if (size > concreteEntries.size()) {
    ref<ConstantExpr> zero1 = ConstantExpr::create(0, Expr::Bool),
                      zero32 = ConstantExpr::create(0, Expr::Int32);
    threadId.resize(size, zero32);
    wgid.resize(size, zero32);
    read.resize(size, zero1);
    write.resize(size, zero1);
    manyRead.resize(size, zero1);
    wgManyRead.resize(size, zero1);
  }

  std::string logIdStr;
  llvm::raw_string_ostream(logIdStr) << logId++;

  updates = new MemoryLogUpdates;
  updates->threadId.root =
    new Array("threadId_" + logIdStr, size,
              threadId.data(), threadId.data()+threadId.size(),
              Expr::Int32, Expr::Int32);
  updates->wgid.root =
    new Array("wgid_" + logIdStr, size,
              wgid.data(), wgid.data()+wgid.size(),
              Expr::Int32, Expr::Int32);
  updates->read.root =
    new Array("read_" + logIdStr, size,
              read.data(), read.data()+read.size(),
              Expr::Int32, Expr::Bool);
  updates->write.root =
    new Array("write_" + logIdStr, size,
              write.data(), write.data()+write.size(),
              Expr::Int32, Expr::Bool);
  updates->manyRead.root =
    new Array("manyRead_" + logIdStr, size,
              manyRead.data(), manyRead.data()+manyRead.size(),
              Expr::Int32, Expr::Bool);
  updates->wgManyRead.root =
    new Array("wgManyRead_" + logIdStr, size,
              wgManyRead.data(), wgManyRead.data()+wgManyRead.size(),
              Expr::Int32, Expr::Bool);
}

bool MemoryLog::logRead(ExecutionState *state, TimingSolver *solver, unsigned offset, MemoryRace &raceInfo) {
  if (!state)
    return false;

  thread_id_t threadId = state->crtThread().getTid();
  unsigned wgid = state->crtThread().getWorkgroupId();

  // FIXME: creating a thread should be handled specially
  // for now, just ignore thread 0
  if (threadId == 0)
    return false;

  if (isSymbolic())
    return logRead(state, solver, ConstantExpr::create(offset, Expr::Int32), raceInfo);

  if (concreteEntries.size() < offset+1)
    concreteEntries.resize(offset+1);
  MemoryLogEntry &entry = concreteEntries[offset];

  if (entry.write && !entry.matches(threadId, wgid)) {
    raceInfo.raceType = MemoryRace::RT_readwrite;
    raceInfo.op1ThreadId = threadId;
    raceInfo.op2ThreadId = entry.threadId;
    return true;
  }

  if (entry.read) {
    if (entry.threadId != 0 && entry.threadId != threadId)
      entry.manyRead = 1;
    if (entry.wgid != wgid)
      entry.wgManyRead = 1;
  }

  entry.threadId = threadId;
  entry.wgid = wgid;
  entry.read = 1;
  
  return false;
}

bool MemoryLog::logRead(ExecutionState *state, TimingSolver *solver, ref<Expr> offset, MemoryRace &raceInfo) {
  if (!state)
    return false;

  thread_id_t threadId = state->crtThread().getTid();
  unsigned wgid = state->crtThread().getWorkgroupId();

  // FIXME: creating a thread should be handled specially
  // for now, just ignore thread 0
  if (threadId == 0)
    return false;

  makeSymbolic();

  ref<Expr> oldWrite = ReadExpr::create(updates->write, offset);
  ref<Expr> oldThreadId = ReadExpr::create(updates->threadId, offset);
  ref<Expr> oldWgid = ReadExpr::create(updates->wgid, offset);

  ref<ConstantExpr> threadIdConst = ConstantExpr::create(threadId, Expr::Int32);
  ref<ConstantExpr> wgidConst = ConstantExpr::create(wgid, Expr::Int32);

  ref<Expr> threadIdMismatch = NeExpr::create(oldThreadId, threadIdConst);
  ref<Expr> wgidMismatch = NeExpr::create(oldWgid, wgidConst);

  ref<Expr> query = AndExpr::create(oldWrite,
                               AndExpr::create(threadIdMismatch, wgidMismatch));

  bool result;
  bool success = solver->mayBeTrue(*state, query, result);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;
  if (result) {
    raceInfo.raceType = MemoryRace::RT_readwrite;
    // TODO: get assignments from the solver for these
    raceInfo.op1ThreadId = 1;
    raceInfo.op2ThreadId = 2;
    return true;
  }

  ref<ConstantExpr> trueConst = ConstantExpr::create(1, Expr::Bool);

  ref<Expr> oldRead = ReadExpr::create(updates->read, offset);
  ref<Expr> oldManyRead = ReadExpr::create(updates->manyRead, offset);
  ref<Expr> oldWgManyRead = ReadExpr::create(updates->wgManyRead, offset);

  ref<Expr> threadIdNonZero =
    NeExpr::create(oldThreadId, ConstantExpr::create(0, Expr::Int32));

  ref<Expr> newManyRead =
    SelectExpr::create(AndExpr::create(oldRead,
                         AndExpr::create(threadIdNonZero, threadIdMismatch)),
                       trueConst, oldManyRead);
  ref<Expr> newWgManyRead =
    SelectExpr::create(AndExpr::create(oldRead, wgidMismatch),
                       trueConst, oldWgManyRead);

  ref<Expr> newThreadId = threadIdConst;
  ref<Expr> newWgid = wgidConst;
  ref<Expr> newRead = trueConst;

  updates->manyRead.extend(offset, newManyRead);
  updates->wgManyRead.extend(offset, newWgManyRead);
  updates->threadId.extend(offset, newThreadId);
  updates->wgid.extend(offset, newWgid);
  updates->read.extend(offset, newRead);

  return false;
}

bool MemoryLog::logWrite(ExecutionState *state, TimingSolver *solver, unsigned offset, MemoryRace &raceInfo) {
  if (!state)
    return false;

  thread_id_t threadId = state->crtThread().getTid();
  unsigned wgid = state->crtThread().getWorkgroupId();

  // FIXME: creating a thread should be handled specially
  // for now, just ignore thread 0
  if (threadId == 0)
    return false;

  if (isSymbolic())
    return logWrite(state, solver, ConstantExpr::create(offset, Expr::Int32), raceInfo);

  if (concreteEntries.size() < offset+1)
    concreteEntries.resize(offset+1);
  MemoryLogEntry &entry = concreteEntries[offset];

  if (entry.manyRead || entry.wgManyRead ||
      ((entry.read || entry.write) && !entry.matches(threadId, wgid))) {
    raceInfo.raceType = entry.read ? MemoryRace::RT_readwrite : MemoryRace::RT_writewrite;
    raceInfo.op1ThreadId = entry.threadId;
    raceInfo.op2ThreadId = threadId;
    return true;
  }

  entry.threadId = threadId;
  entry.wgid = wgid;
  entry.write = 1;

  return false;
}

bool MemoryLog::logWrite(ExecutionState *state, TimingSolver *solver, ref<Expr> offset, MemoryRace &raceInfo) {
  if (!state)
    return false;

  thread_id_t threadId = state->crtThread().getTid();
  unsigned wgid = state->crtThread().getWorkgroupId();

  // FIXME: creating a thread should be handled specially
  // for now, just ignore thread 0
  if (threadId == 0)
    return false;

  makeSymbolic();

  ref<Expr> oldThreadId = ReadExpr::create(updates->threadId, offset);
  ref<Expr> oldWgid = ReadExpr::create(updates->wgid, offset);
  ref<Expr> oldRead = ReadExpr::create(updates->read, offset);
  ref<Expr> oldWrite = ReadExpr::create(updates->write, offset);
  ref<Expr> oldManyRead = ReadExpr::create(updates->manyRead, offset);
  ref<Expr> oldWgManyRead = ReadExpr::create(updates->wgManyRead, offset);

  ref<ConstantExpr> threadIdConst = ConstantExpr::create(threadId, Expr::Int32);
  ref<ConstantExpr> wgidConst = ConstantExpr::create(wgid, Expr::Int32);

  ref<Expr> threadIdMismatch = NeExpr::create(oldThreadId, threadIdConst);
  ref<Expr> wgidMismatch = NeExpr::create(oldWgid, wgidConst);

  ref<Expr> query =
    OrExpr::create(OrExpr::create(oldManyRead, oldWgManyRead),
                   AndExpr::create(OrExpr::create(oldRead, oldWrite), 
                              AndExpr::create(threadIdMismatch, wgidMismatch)));

  bool result;
  bool success = solver->mayBeTrue(*state, query, result);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;
  if (result) {
    // TODO: use assignment to see if this is writewrite or readwrite?
    raceInfo.raceType = MemoryRace::RT_writewrite;
    // TODO: get assignments from the solver for these
    raceInfo.op1ThreadId = 1;
    raceInfo.op2ThreadId = 2;
    return true;
  }

  ref<ConstantExpr> trueConst = ConstantExpr::create(1, Expr::Bool);

  ref<Expr> newThreadId = threadIdConst;
  ref<Expr> newWgid = wgidConst;
  ref<Expr> newWrite = trueConst;

  updates->threadId.extend(offset, newThreadId);
  updates->wgid.extend(offset, newWgid);
  updates->write.extend(offset, newWrite);

  return false;
}

void MemoryLog::localReset(unsigned wgid) {
  if (isSymbolic()) {
    ref<ConstantExpr> zero1 = ConstantExpr::create(0, Expr::Bool),
                      zero32 = ConstantExpr::create(0, Expr::Int32),
                      wgidConst = ConstantExpr::create(wgid, Expr::Int32);

    for (unsigned ofs = 0; ofs != size; ++ofs) {
      ref<ConstantExpr> offset = ConstantExpr::create(ofs, Expr::Int32);

      ref<Expr> oldWgid = ReadExpr::create(updates->wgid, offset);
      ref<Expr> oldThreadId = ReadExpr::create(updates->threadId, offset);
      ref<Expr> oldManyRead = ReadExpr::create(updates->manyRead, offset);

      ref<Expr> match = EqExpr::create(oldWgid, wgidConst);

      ref<Expr> newThreadId = SelectExpr::create(match, zero32, oldThreadId);
      ref<Expr> newManyRead = SelectExpr::create(match, zero1, oldManyRead);

      updates->threadId.extend(offset, newThreadId);
      updates->manyRead.extend(offset, newManyRead);
    }
  } else {
    for (std::vector<MemoryLogEntry>::iterator i = concreteEntries.begin(),
         e = concreteEntries.end(); i != e; ++i) {
      if (i->wgid == wgid) {
        i->threadId = 0;
        i->manyRead = 0;
      }
    }
  }
}

void MemoryLog::globalReset() {
  concreteEntries.clear();
  delete updates;
  updates = 0;
}

/***/

ObjectState::ObjectState(const MemoryObject *mo)
  : copyOnWriteOwner(0),
    refCount(0),
    object(mo),
    concreteStore(new uint8_t[mo->size]),
    concreteMask(0),
    flushMask(0),
    knownSymbolics(0),
    updates(0, 0),
    memoryLog(mo->size),
    size(mo->size),
    readOnly(false),
    isShared(false) {
  if (!UseConstantArrays) {
    // FIXME: Leaked.
    static unsigned id = 0;
    const Array *array = new Array("tmp_arr" + llvm::utostr(++id), size);
    updates = UpdateList(array, 0);
  }
}


ObjectState::ObjectState(const MemoryObject *mo, const Array *array)
  : copyOnWriteOwner(0),
    refCount(0),
    object(mo),
    concreteStore(new uint8_t[mo->size]),
    concreteMask(0),
    flushMask(0),
    knownSymbolics(0),
    updates(array, 0),
    memoryLog(mo->size),
    size(mo->size),
    readOnly(false),
    isShared(false) {
  makeSymbolic();
}

ObjectState::ObjectState(const ObjectState &os) 
  : copyOnWriteOwner(0),
    refCount(0),
    object(os.object),
    concreteStore(new uint8_t[os.size]),
    concreteMask(os.concreteMask ? new BitArray(*os.concreteMask, os.size) : 0),
    flushMask(os.flushMask ? new BitArray(*os.flushMask, os.size) : 0),
    knownSymbolics(0),
    updates(os.updates),
    memoryLog(os.memoryLog),
    size(os.size),
    readOnly(false),
    isShared(os.isShared) {
  assert(!os.readOnly && "no need to copy read only object?");

  if (os.knownSymbolics) {
    knownSymbolics = new ref<Expr>[size];
    for (unsigned i=0; i<size; i++)
      knownSymbolics[i] = os.knownSymbolics[i];
  }

  memcpy(concreteStore, os.concreteStore, size*sizeof(*concreteStore));
}

ObjectState::~ObjectState() {
  if (concreteMask) delete concreteMask;
  if (flushMask) delete flushMask;
  if (knownSymbolics) delete[] knownSymbolics;
  delete[] concreteStore;
}

/***/

const UpdateList &ObjectState::getUpdates() const {
  // Constant arrays are created lazily.
  if (!updates.root) {
    // Collect the list of writes, with the oldest writes first.
    
    // FIXME: We should be able to do this more efficiently, we just need to be
    // careful to get the interaction with the cache right. In particular we
    // should avoid creating UpdateNode instances we never use.
    unsigned NumWrites = updates.head ? updates.head->getSize() : 0;
    std::vector< std::pair< ref<Expr>, ref<Expr> > > Writes(NumWrites);
    const UpdateNode *un = updates.head;
    for (unsigned i = NumWrites; i != 0; un = un->next) {
      --i;
      Writes[i] = std::make_pair(un->index, un->value);
    }

    std::vector< ref<ConstantExpr> > Contents(size);

    // Initialize to zeros.
    for (unsigned i = 0, e = size; i != e; ++i)
      Contents[i] = ConstantExpr::create(0, Expr::Int8);

    // Pull off as many concrete writes as we can.
    unsigned Begin = 0, End = Writes.size();
    for (; Begin != End; ++Begin) {
      // Push concrete writes into the constant array.
      ConstantExpr *Index = dyn_cast<ConstantExpr>(Writes[Begin].first);
      if (!Index)
        break;

      ConstantExpr *Value = dyn_cast<ConstantExpr>(Writes[Begin].second);
      if (!Value)
        break;

      Contents[Index->getZExtValue()] = Value;
    }

    // FIXME: We should unique these, there is no good reason to create multiple
    // ones.

    // Start a new update list.
    // FIXME: Leaked.
    static unsigned id = 0;
    const Array *array = new Array("const_arr" + llvm::utostr(++id), size,
                                   &Contents[0],
                                   &Contents[0] + Contents.size());
    updates = UpdateList(array, 0);

    // Apply the remaining (non-constant) writes.
    for (; Begin != End; ++Begin)
      updates.extend(Writes[Begin].first, Writes[Begin].second);
  }

  return updates;
}

void ObjectState::makeConcrete() {
  if (concreteMask) delete concreteMask;
  if (flushMask) delete flushMask;
  if (knownSymbolics) delete[] knownSymbolics;
  concreteMask = 0;
  flushMask = 0;
  knownSymbolics = 0;
}

void ObjectState::makeSymbolic() {
  assert(!updates.head &&
         "XXX makeSymbolic of objects with symbolic values is unsupported");

  // XXX simplify this, can just delete various arrays I guess
  for (unsigned i=0; i<size; i++) {
    markByteSymbolic(i);
    setKnownSymbolic(i, 0);
    markByteFlushed(i);
  }
}

void ObjectState::initializeToZero() {
  makeConcrete();
  memset(concreteStore, 0, size);
}

void ObjectState::initializeToRandom() {  
  makeConcrete();
  for (unsigned i=0; i<size; i++) {
    // randomly selected by 256 sided die
    concreteStore[i] = 0xAB;
  }
}

/*
Cache Invariants
--
isByteKnownSymbolic(i) => !isByteConcrete(i)
isByteConcrete(i) => !isByteKnownSymbolic(i)
!isByteFlushed(i) => (isByteConcrete(i) || isByteKnownSymbolic(i))
 */

void ObjectState::fastRangeCheckOffset(ref<Expr> offset,
                                       unsigned *base_r,
                                       unsigned *size_r) const {
  *base_r = 0;
  *size_r = size;
}

void ObjectState::flushRangeForRead(unsigned rangeBase, 
                                    unsigned rangeSize) const {
  if (!flushMask) flushMask = new BitArray(size, true);
 
  for (unsigned offset=rangeBase; offset<rangeBase+rangeSize; offset++) {
    if (!isByteFlushed(offset)) {
      if (isByteConcrete(offset)) {
        updates.extend(ConstantExpr::create(offset, Expr::Int32),
                       ConstantExpr::create(concreteStore[offset], Expr::Int8));
      } else {
        assert(isByteKnownSymbolic(offset) && "invalid bit set in flushMask");
        updates.extend(ConstantExpr::create(offset, Expr::Int32),
                       knownSymbolics[offset]);
      }

      flushMask->unset(offset);
    }
  } 
}

void ObjectState::flushRangeForWrite(unsigned rangeBase, 
                                     unsigned rangeSize) {
  if (!flushMask) flushMask = new BitArray(size, true);

  for (unsigned offset=rangeBase; offset<rangeBase+rangeSize; offset++) {
    if (!isByteFlushed(offset)) {
      if (isByteConcrete(offset)) {
        updates.extend(ConstantExpr::create(offset, Expr::Int32),
                       ConstantExpr::create(concreteStore[offset], Expr::Int8));
        markByteSymbolic(offset);
      } else {
        assert(isByteKnownSymbolic(offset) && "invalid bit set in flushMask");
        updates.extend(ConstantExpr::create(offset, Expr::Int32),
                       knownSymbolics[offset]);
        setKnownSymbolic(offset, 0);
      }

      flushMask->unset(offset);
    } else {
      // flushed bytes that are written over still need
      // to be marked out
      if (isByteConcrete(offset)) {
        markByteSymbolic(offset);
      } else if (isByteKnownSymbolic(offset)) {
        setKnownSymbolic(offset, 0);
      }
    }
  } 
}

bool ObjectState::isByteConcrete(unsigned offset) const {
  return !concreteMask || concreteMask->get(offset);
}

bool ObjectState::isByteFlushed(unsigned offset) const {
  return flushMask && !flushMask->get(offset);
}

bool ObjectState::isByteKnownSymbolic(unsigned offset) const {
  return knownSymbolics && knownSymbolics[offset].get();
}

void ObjectState::markByteConcrete(unsigned offset) {
  if (concreteMask)
    concreteMask->set(offset);
}

void ObjectState::markByteSymbolic(unsigned offset) {
  if (!concreteMask)
    concreteMask = new BitArray(size, true);
  concreteMask->unset(offset);
}

void ObjectState::markByteUnflushed(unsigned offset) {
  if (flushMask)
    flushMask->set(offset);
}

void ObjectState::markByteFlushed(unsigned offset) {
  if (!flushMask) {
    flushMask = new BitArray(size, false);
  } else {
    flushMask->unset(offset);
  }
}

void ObjectState::setKnownSymbolic(unsigned offset, 
                                   Expr *value /* can be null */) {
  if (knownSymbolics) {
    knownSymbolics[offset] = value;
  } else {
    if (value) {
      knownSymbolics = new ref<Expr>[size];
      knownSymbolics[offset] = value;
    }
  }
}

/***/

ref<Expr> ObjectState::read8(unsigned offset, ExecutionState *state, TimingSolver *solver) const {
  MemoryRace race;
  if (memoryLog.logRead(state, solver, offset, race)) {
    llvm::errs() << "memory read: race detected\n";
  }

  if (isByteConcrete(offset)) {
    return ConstantExpr::create(concreteStore[offset], Expr::Int8);
  } else if (isByteKnownSymbolic(offset)) {
    return knownSymbolics[offset];
  } else {
    assert(isByteFlushed(offset) && "unflushed byte without cache value");
    
    return ReadExpr::create(getUpdates(), 
                            ConstantExpr::create(offset, Expr::Int32));
  }    
}

ref<Expr> ObjectState::read8(ref<Expr> offset, ExecutionState *state, TimingSolver *solver) const {
  assert(!isa<ConstantExpr>(offset) && "constant offset passed to symbolic read8");
  unsigned base, size;
  fastRangeCheckOffset(offset, &base, &size);
  flushRangeForRead(base, size);

  MemoryRace race;
  if (memoryLog.logRead(state, solver, offset, race)) {
    llvm::errs() << "memory read: race detected\n";
  }

  if (size>4096) {
    std::string allocInfo;
    object->getAllocInfo(allocInfo);
    klee_warning_once(0, "flushing %d bytes on read, may be slow and/or crash: %s", 
                      size,
                      allocInfo.c_str());
  }
  
  return ReadExpr::create(getUpdates(), ZExtExpr::create(offset, Expr::Int32));
}

void ObjectState::write8(unsigned offset, uint8_t value, ExecutionState *state, TimingSolver *solver) {
  //assert(read_only == false && "writing to read-only object!");
  MemoryRace race;
  if (memoryLog.logWrite(state, solver, offset, race)) {
    llvm::errs() << "memory write: race detected\n";
  }

  concreteStore[offset] = value;
  setKnownSymbolic(offset, 0);

  markByteConcrete(offset);
  markByteUnflushed(offset);
}

void ObjectState::write8(unsigned offset, ref<Expr> value, ExecutionState *state, TimingSolver *solver) {
  // can happen when ExtractExpr special cases
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(value)) {
    write8(offset, (uint8_t) CE->getZExtValue(8), state, solver);
  } else {
    MemoryRace race;
    if (memoryLog.logWrite(state, solver, offset, race)) {
      llvm::errs() << "memory write: race detected\n";
    }

    setKnownSymbolic(offset, value.get());
      
    markByteSymbolic(offset);
    markByteUnflushed(offset);
  }
}

void ObjectState::write8(ref<Expr> offset, ref<Expr> value, ExecutionState *state, TimingSolver *solver) {
  assert(!isa<ConstantExpr>(offset) && "constant offset passed to symbolic write8");
  unsigned base, size;
  fastRangeCheckOffset(offset, &base, &size);
  flushRangeForWrite(base, size);

  MemoryRace race;
  if (memoryLog.logWrite(state, solver, offset, race)) {
    llvm::errs() << "memory write: race detected\n";
  }

  if (size>4096) {
    std::string allocInfo;
    object->getAllocInfo(allocInfo);
    klee_warning_once(0, "flushing %d bytes on read, may be slow and/or crash: %s", 
                      size,
                      allocInfo.c_str());
  }
  
  updates.extend(ZExtExpr::create(offset, Expr::Int32), value);
}

/***/

ref<Expr> ObjectState::read(ref<Expr> offset, Expr::Width width, ExecutionState *state, TimingSolver *solver) const {
  // Truncate offset to 32-bits.
  offset = ZExtExpr::create(offset, Expr::Int32);

  // Check for reads at constant offsets.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset))
    return read(CE->getZExtValue(32), width, state, solver);

  // Treat bool specially, it is the only non-byte sized write we allow.
  if (width == Expr::Bool)
    return ExtractExpr::create(read8(offset, state, solver), 0, Expr::Bool);

  // Otherwise, follow the slow general case.
  unsigned NumBytes = width / 8;
  assert(width == NumBytes * 8 && "Invalid write size!");
  ref<Expr> Res(0);
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    ref<Expr> Byte = read8(AddExpr::create(offset, 
                                           ConstantExpr::create(idx, 
                                                                Expr::Int32)), state, solver);
    Res = i ? ConcatExpr::create(Byte, Res) : Byte;
  }

  return Res;
}

ref<Expr> ObjectState::read(unsigned offset, Expr::Width width, ExecutionState *state, TimingSolver *solver) const {
  // Treat bool specially, it is the only non-byte sized write we allow.
  if (width == Expr::Bool)
    return ExtractExpr::create(read8(offset, state, solver), 0, Expr::Bool);

  // Otherwise, follow the slow general case.
  unsigned NumBytes = width / 8;
  assert(width == NumBytes * 8 && "Invalid write size!");
  ref<Expr> Res(0);
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    ref<Expr> Byte = read8(offset + idx, state, solver);
    Res = i ? ConcatExpr::create(Byte, Res) : Byte;
  }

  return Res;
}

void ObjectState::write(ref<Expr> offset, ref<Expr> value, ExecutionState *state, TimingSolver *solver) {
  // Truncate offset to 32-bits.
  offset = ZExtExpr::create(offset, Expr::Int32);

  // Check for writes at constant offsets.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset)) {
    write(CE->getZExtValue(32), value, state, solver);
    return;
  }

  // Treat bool specially, it is the only non-byte sized write we allow.
  Expr::Width w = value->getWidth();
  if (w == Expr::Bool) {
    write8(offset, ZExtExpr::create(value, Expr::Int8), state, solver);
    return;
  }

  // Otherwise, follow the slow general case.
  unsigned NumBytes = w / 8;
  assert(w == NumBytes * 8 && "Invalid write size!");
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(AddExpr::create(offset, ConstantExpr::create(idx, Expr::Int32)),
           ExtractExpr::create(value, 8 * i, Expr::Int8), state, solver);
  }
}

void ObjectState::write(unsigned offset, ref<Expr> value, ExecutionState *state, TimingSolver *solver) {
  // Check for writes of constant values.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(value)) {
    Expr::Width w = CE->getWidth();
    if (w <= 64) {
      uint64_t val = CE->getZExtValue();
      switch (w) {
      default: assert(0 && "Invalid write size!");
      case  Expr::Bool:
      case  Expr::Int8:  write8(offset, val, state, solver); return;
      case Expr::Int16: write16(offset, val, state, solver); return;
      case Expr::Int32: write32(offset, val, state, solver); return;
      case Expr::Int64: write64(offset, val, state, solver); return;
      }
    }
  }

  // Treat bool specially, it is the only non-byte sized write we allow.
  Expr::Width w = value->getWidth();
  if (w == Expr::Bool) {
    write8(offset, ZExtExpr::create(value, Expr::Int8), state, solver);
    return;
  }

  // Otherwise, follow the slow general case.
  unsigned NumBytes = w / 8;
  assert(w == NumBytes * 8 && "Invalid write size!");
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, ExtractExpr::create(value, 8 * i, Expr::Int8), state, solver);
  }
} 

void ObjectState::write16(unsigned offset, uint16_t value, ExecutionState *state, TimingSolver *solver) {
  unsigned NumBytes = 2;
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, (uint8_t) (value >> (8 * i)), state, solver);
  }
}

void ObjectState::write32(unsigned offset, uint32_t value, ExecutionState *state, TimingSolver *solver) {
  unsigned NumBytes = 4;
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, (uint8_t) (value >> (8 * i)), state, solver);
  }
}

void ObjectState::write64(unsigned offset, uint64_t value, ExecutionState *state, TimingSolver *solver) {
  unsigned NumBytes = 8;
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, (uint8_t) (value >> (8 * i)), state, solver);
  }
}

void ObjectState::print() {
  std::cerr << "-- ObjectState --\n";
  std::cerr << "\tMemoryObject ID: " << object->id << "\n";
  std::cerr << "\tRoot Object: " << updates.root << "\n";
  std::cerr << "\tSize: " << size << "\n";

  std::cerr << "\tBytes:\n";
  for (unsigned i=0; i<size; i++) {
    std::cerr << "\t\t["<<i<<"]"
               << " concrete? " << isByteConcrete(i)
               << " known-sym? " << isByteKnownSymbolic(i)
               << " flushed? " << isByteFlushed(i) << " = ";
    ref<Expr> e = read8(i);
    std::cerr << e << "\n";
  }

  std::cerr << "\tUpdates:\n";
  for (const UpdateNode *un=updates.head; un; un=un->next) {
    std::cerr << "\t\t[" << un->index << "] = " << un->value << "\n";
  }
}

void ObjectState::localResetMemoryLog(unsigned wgid) {
  memoryLog.localReset(wgid);
}

void ObjectState::globalResetMemoryLog() {
  memoryLog.globalReset();
}
