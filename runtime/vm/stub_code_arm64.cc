// Copyright (c) 2014, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"
#if defined(TARGET_ARCH_ARM64) && !defined(DART_PRECOMPILED_RUNTIME)

#include "vm/compiler/assembler/assembler.h"
#include "vm/compiler/backend/flow_graph_compiler.h"
#include "vm/compiler/jit/compiler.h"
#include "vm/dart_entry.h"
#include "vm/heap/heap.h"
#include "vm/instructions.h"
#include "vm/object_store.h"
#include "vm/runtime_entry.h"
#include "vm/stack_frame.h"
#include "vm/stub_code.h"
#include "vm/tags.h"
#include "vm/type_testing_stubs.h"

#define __ assembler->

namespace dart {

DEFINE_FLAG(bool, inline_alloc, true, "Inline allocation of objects.");
DEFINE_FLAG(bool,
            use_slow_path,
            false,
            "Set to true for debugging & verifying the slow paths.");
DECLARE_FLAG(bool, trace_optimized_ic_calls);
DECLARE_FLAG(bool, enable_interpreter);

// Input parameters:
//   LR : return address.
//   SP : address of last argument in argument array.
//   SP + 8*R4 - 8 : address of first argument in argument array.
//   SP + 8*R4 : address of return value.
//   R5 : address of the runtime function to call.
//   R4 : number of arguments to the call.
void StubCode::GenerateCallToRuntimeStub(Assembler* assembler) {
  const intptr_t thread_offset = NativeArguments::thread_offset();
  const intptr_t argc_tag_offset = NativeArguments::argc_tag_offset();
  const intptr_t argv_offset = NativeArguments::argv_offset();
  const intptr_t retval_offset = NativeArguments::retval_offset();

  __ Comment("CallToRuntimeStub");
  __ ldr(CODE_REG, Address(THR, Thread::call_to_runtime_stub_offset()));
  __ SetPrologueOffset();
  __ EnterStubFrame();

  // Save exit frame information to enable stack walking as we are about
  // to transition to Dart VM C++ code.
  __ StoreToOffset(FP, THR, Thread::top_exit_frame_info_offset());

#if defined(DEBUG)
  {
    Label ok;
    // Check that we are always entering from Dart code.
    __ LoadFromOffset(R8, THR, Thread::vm_tag_offset());
    __ CompareImmediate(R8, VMTag::kDartTagId);
    __ b(&ok, EQ);
    __ Stop("Not coming from Dart code.");
    __ Bind(&ok);
  }
#endif

  // Mark that the thread is executing VM code.
  __ StoreToOffset(R5, THR, Thread::vm_tag_offset());

  // Reserve space for arguments and align frame before entering C++ world.
  // NativeArguments are passed in registers.
  __ Comment("align stack");
  // Reserve space for arguments.
  ASSERT(sizeof(NativeArguments) == 4 * kWordSize);
  __ ReserveAlignedFrameSpace(sizeof(NativeArguments));

  // Pass NativeArguments structure by value and call runtime.
  // Registers R0, R1, R2, and R3 are used.

  ASSERT(thread_offset == 0 * kWordSize);
  // Set thread in NativeArgs.
  __ mov(R0, THR);

  // There are no runtime calls to closures, so we do not need to set the tag
  // bits kClosureFunctionBit and kInstanceFunctionBit in argc_tag_.
  ASSERT(argc_tag_offset == 1 * kWordSize);
  __ mov(R1, R4);  // Set argc in NativeArguments.

  ASSERT(argv_offset == 2 * kWordSize);
  __ add(R2, ZR, Operand(R4, LSL, 3));
  __ add(R2, FP, Operand(R2));  // Compute argv.
  // Set argv in NativeArguments.
  __ AddImmediate(R2, kParamEndSlotFromFp * kWordSize);

  ASSERT(retval_offset == 3 * kWordSize);
  __ AddImmediate(R3, R2, kWordSize);

  __ StoreToOffset(R0, SP, thread_offset);
  __ StoreToOffset(R1, SP, argc_tag_offset);
  __ StoreToOffset(R2, SP, argv_offset);
  __ StoreToOffset(R3, SP, retval_offset);
  __ mov(R0, SP);  // Pass the pointer to the NativeArguments.

  // We are entering runtime code, so the C stack pointer must be restored from
  // the stack limit to the top of the stack. We cache the stack limit address
  // in a callee-saved register.
  __ mov(R25, CSP);
  __ mov(CSP, SP);

  __ blr(R5);
  __ Comment("CallToRuntimeStub return");

  // Restore SP and CSP.
  __ mov(SP, CSP);
  __ mov(CSP, R25);

  // Refresh write barrier mask.
  __ ldr(BARRIER_MASK, Address(THR, Thread::write_barrier_mask_offset()));

  // Retval is next to 1st argument.
  // Mark that the thread is executing Dart code.
  __ LoadImmediate(R2, VMTag::kDartTagId);
  __ StoreToOffset(R2, THR, Thread::vm_tag_offset());

  // Reset exit frame information in Isolate structure.
  __ StoreToOffset(ZR, THR, Thread::top_exit_frame_info_offset());

  __ LeaveStubFrame();

  // The following return can jump to a lazy-deopt stub, which assumes R0
  // contains a return value and will save it in a GC-visible way.  We therefore
  // have to ensure R0 does not contain any garbage value left from the C
  // function we called (which has return type "void").
  // (See GenerateDeoptimizationSequence::saved_result_slot_from_fp.)
  __ LoadImmediate(R0, 0);
  __ ret();
}

void StubCode::GenerateSharedStub(Assembler* assembler,
                                  bool save_fpu_registers,
                                  const RuntimeEntry* target,
                                  intptr_t self_code_stub_offset_from_thread,
                                  bool allow_return) {
  __ Push(LR);

  // We want the saved registers to appear like part of the caller's frame, so
  // we push them before calling EnterStubFrame.
  RegisterSet all_registers;
  all_registers.AddAllNonReservedRegisters(save_fpu_registers);
  __ PushRegisters(all_registers);

  const intptr_t kSavedCpuRegisterSlots =
      Utils::CountOneBitsWord(kDartAvailableCpuRegs);

  const intptr_t kSavedFpuRegisterSlots =
      save_fpu_registers ? kNumberOfFpuRegisters * kFpuRegisterSize / kWordSize
                         : 0;

  const intptr_t kAllSavedRegistersSlots =
      kSavedCpuRegisterSlots + kSavedFpuRegisterSlots;

  // Copy down the return address so the stack layout is correct.
  __ ldr(TMP, Address(SPREG, kAllSavedRegistersSlots * kWordSize));
  __ Push(TMP);

  __ ldr(CODE_REG, Address(THR, self_code_stub_offset_from_thread));

  __ EnterStubFrame();

  __ ldr(CODE_REG, Address(THR, Thread::call_to_runtime_stub_offset()));
  __ ldr(R5, Address(THR, Thread::OffsetFromThread(target)));
  __ LoadImmediate(R4, /*argument_count=*/0);
  __ ldr(TMP, Address(THR, Thread::call_to_runtime_entry_point_offset()));
  __ blr(TMP);

  if (!allow_return) {
    __ Breakpoint();
    return;
  }
  __ LeaveStubFrame();

  // Drop "official" return address -- we can just use the one stored above the
  // saved registers.
  __ Drop(1);

  __ PopRegisters(all_registers);

  __ Pop(LR);
  __ ret(LR);
}

// R1: The extracted method.
// R4: The type_arguments_field_offset (or 0)
void StubCode::GenerateBuildMethodExtractorStub(Assembler* assembler) {
  Thread* thread = Thread::Current();
  Zone* Z = thread->zone();
  ObjectStore* object_store = thread->isolate()->object_store();

  const auto& closure_class =
      Class::ZoneHandle(Z, object_store->closure_class());
  const auto& closure_allocation_stub =
      Code::ZoneHandle(Z, StubCode::GetAllocationStubForClass(closure_class));

  const intptr_t kReceiverOffset = compiler_frame_layout.param_end_from_fp + 1;

  const auto& context_allocation_stub =
      Code::ZoneHandle(StubCode::AllocateContext_entry()->code());

  __ EnterStubFrame();

  // Build type_arguments vector (or null)
  Label no_type_args;
  __ ldr(R3, Address(THR, Thread::object_null_offset()), kDoubleWord);
  __ cmp(R4, Operand(0));
  __ b(&no_type_args, EQ);
  __ ldr(R0, Address(FP, kReceiverOffset * kWordSize));
  __ ldr(R3, Address(R0, R4));
  __ Bind(&no_type_args);

  // Push type arguments & extracted method.
  __ PushPair(R3, R1);

  // Allocate context.
  {
    Label done, slow_path;
    __ TryAllocateArray(kContextCid, Context::InstanceSize(1), &slow_path,
                        R0,  // instance
                        R1,  // end address
                        R2, R3);
    __ ldr(R1, Address(THR, Thread::object_null_offset()));
    __ str(R1, FieldAddress(R0, Context::parent_offset()));
    __ LoadImmediate(R1, 1);
    __ str(R1, FieldAddress(R0, Context::num_variables_offset()));
    __ b(&done);

    __ Bind(&slow_path);

    __ LoadImmediate(/*num_vars=*/R1, 1);
    __ LoadObject(CODE_REG, context_allocation_stub);
    __ ldr(R0, FieldAddress(CODE_REG, Code::entry_point_offset()));
    __ blr(R0);

    __ Bind(&done);
  }

  // Store receiver in context
  __ ldr(R1, Address(FP, kWordSize * kReceiverOffset));
  __ StoreIntoObject(R0, FieldAddress(R0, Context::variable_offset(0)), R1);

  // Push context.
  __ Push(R0);

  // Allocate closure.
  __ LoadObject(CODE_REG, closure_allocation_stub);
  __ ldr(R1, FieldAddress(CODE_REG, Code::entry_point_offset(
                                        Code::EntryKind::kUnchecked)));
  __ blr(R1);

  // Populate closure object.
  __ Pop(R1);  // Pop context.
  __ StoreIntoObject(R0, FieldAddress(R0, Closure::context_offset()), R1);
  __ PopPair(R3, R1);  // Pop type arguments & extracted method.
  __ StoreIntoObjectNoBarrier(R0, FieldAddress(R0, Closure::function_offset()),
                              R1);
  __ StoreIntoObjectNoBarrier(
      R0, FieldAddress(R0, Closure::instantiator_type_arguments_offset()), R3);
  __ LoadObject(R1, Object::empty_type_arguments());
  __ StoreIntoObjectNoBarrier(
      R0, FieldAddress(R0, Closure::delayed_type_arguments_offset()), R1);

  __ LeaveStubFrame();
  __ Ret();
}

void StubCode::GenerateNullErrorSharedWithoutFPURegsStub(Assembler* assembler) {
  GenerateSharedStub(assembler, /*save_fpu_registers=*/false,
                     &kNullErrorRuntimeEntry,
                     Thread::null_error_shared_without_fpu_regs_stub_offset(),
                     /*allow_return=*/false);
}

void StubCode::GenerateNullErrorSharedWithFPURegsStub(Assembler* assembler) {
  GenerateSharedStub(assembler, /*save_fpu_registers=*/true,
                     &kNullErrorRuntimeEntry,
                     Thread::null_error_shared_with_fpu_regs_stub_offset(),
                     /*allow_return=*/false);
}

void StubCode::GenerateStackOverflowSharedWithoutFPURegsStub(
    Assembler* assembler) {
  GenerateSharedStub(
      assembler, /*save_fpu_registers=*/false, &kStackOverflowRuntimeEntry,
      Thread::stack_overflow_shared_without_fpu_regs_stub_offset(),
      /*allow_return=*/true);
}

void StubCode::GenerateStackOverflowSharedWithFPURegsStub(
    Assembler* assembler) {
  GenerateSharedStub(assembler, /*save_fpu_registers=*/true,
                     &kStackOverflowRuntimeEntry,
                     Thread::stack_overflow_shared_with_fpu_regs_stub_offset(),
                     /*allow_return=*/true);
}

void StubCode::GeneratePrintStopMessageStub(Assembler* assembler) {
  __ Stop("GeneratePrintStopMessageStub");
}

// Input parameters:
//   LR : return address.
//   SP : address of return value.
//   R5 : address of the native function to call.
//   R2 : address of first argument in argument array.
//   R1 : argc_tag including number of arguments and function kind.
static void GenerateCallNativeWithWrapperStub(Assembler* assembler,
                                              Address wrapper) {
  const intptr_t thread_offset = NativeArguments::thread_offset();
  const intptr_t argc_tag_offset = NativeArguments::argc_tag_offset();
  const intptr_t argv_offset = NativeArguments::argv_offset();
  const intptr_t retval_offset = NativeArguments::retval_offset();

  __ EnterStubFrame();

  // Save exit frame information to enable stack walking as we are about
  // to transition to native code.
  __ StoreToOffset(FP, THR, Thread::top_exit_frame_info_offset());

#if defined(DEBUG)
  {
    Label ok;
    // Check that we are always entering from Dart code.
    __ LoadFromOffset(R6, THR, Thread::vm_tag_offset());
    __ CompareImmediate(R6, VMTag::kDartTagId);
    __ b(&ok, EQ);
    __ Stop("Not coming from Dart code.");
    __ Bind(&ok);
  }
#endif

  // Mark that the thread is executing native code.
  __ StoreToOffset(R5, THR, Thread::vm_tag_offset());

  // Reserve space for the native arguments structure passed on the stack (the
  // outgoing pointer parameter to the native arguments structure is passed in
  // R0) and align frame before entering the C++ world.
  __ ReserveAlignedFrameSpace(sizeof(NativeArguments));

  // Initialize NativeArguments structure and call native function.
  // Registers R0, R1, R2, and R3 are used.

  ASSERT(thread_offset == 0 * kWordSize);
  // Set thread in NativeArgs.
  __ mov(R0, THR);

  // There are no native calls to closures, so we do not need to set the tag
  // bits kClosureFunctionBit and kInstanceFunctionBit in argc_tag_.
  ASSERT(argc_tag_offset == 1 * kWordSize);
  // Set argc in NativeArguments: R1 already contains argc.

  ASSERT(argv_offset == 2 * kWordSize);
  // Set argv in NativeArguments: R2 already contains argv.

  // Set retval in NativeArgs.
  ASSERT(retval_offset == 3 * kWordSize);
  __ AddImmediate(R3, FP, 2 * kWordSize);

  // Passing the structure by value as in runtime calls would require changing
  // Dart API for native functions.
  // For now, space is reserved on the stack and we pass a pointer to it.
  __ StoreToOffset(R0, SP, thread_offset);
  __ StoreToOffset(R1, SP, argc_tag_offset);
  __ StoreToOffset(R2, SP, argv_offset);
  __ StoreToOffset(R3, SP, retval_offset);
  __ mov(R0, SP);  // Pass the pointer to the NativeArguments.

  // We are entering runtime code, so the C stack pointer must be restored from
  // the stack limit to the top of the stack. We cache the stack limit address
  // in the Dart SP register, which is callee-saved in the C ABI.
  __ mov(R25, CSP);
  __ mov(CSP, SP);

  __ mov(R1, R5);  // Pass the function entrypoint to call.

  // Call native function invocation wrapper or redirection via simulator.
  __ ldr(LR, wrapper);
  __ blr(LR);

  // Restore SP and CSP.
  __ mov(SP, CSP);
  __ mov(CSP, R25);

  // Refresh write barrier mask.
  __ ldr(BARRIER_MASK, Address(THR, Thread::write_barrier_mask_offset()));

  // Mark that the thread is executing Dart code.
  __ LoadImmediate(R2, VMTag::kDartTagId);
  __ StoreToOffset(R2, THR, Thread::vm_tag_offset());

  // Reset exit frame information in Isolate structure.
  __ StoreToOffset(ZR, THR, Thread::top_exit_frame_info_offset());

  __ LeaveStubFrame();
  __ ret();
}

void StubCode::GenerateCallNoScopeNativeStub(Assembler* assembler) {
  GenerateCallNativeWithWrapperStub(
      assembler,
      Address(THR, Thread::no_scope_native_wrapper_entry_point_offset()));
}

void StubCode::GenerateCallAutoScopeNativeStub(Assembler* assembler) {
  GenerateCallNativeWithWrapperStub(
      assembler,
      Address(THR, Thread::auto_scope_native_wrapper_entry_point_offset()));
}

// Input parameters:
//   LR : return address.
//   SP : address of return value.
//   R5 : address of the native function to call.
//   R2 : address of first argument in argument array.
//   R1 : argc_tag including number of arguments and function kind.
void StubCode::GenerateCallBootstrapNativeStub(Assembler* assembler) {
  const intptr_t thread_offset = NativeArguments::thread_offset();
  const intptr_t argc_tag_offset = NativeArguments::argc_tag_offset();
  const intptr_t argv_offset = NativeArguments::argv_offset();
  const intptr_t retval_offset = NativeArguments::retval_offset();

  __ EnterStubFrame();

  // Save exit frame information to enable stack walking as we are about
  // to transition to native code.
  __ StoreToOffset(FP, THR, Thread::top_exit_frame_info_offset());

#if defined(DEBUG)
  {
    Label ok;
    // Check that we are always entering from Dart code.
    __ LoadFromOffset(R6, THR, Thread::vm_tag_offset());
    __ CompareImmediate(R6, VMTag::kDartTagId);
    __ b(&ok, EQ);
    __ Stop("Not coming from Dart code.");
    __ Bind(&ok);
  }
#endif

  // Mark that the thread is executing native code.
  __ StoreToOffset(R5, THR, Thread::vm_tag_offset());

  // Reserve space for the native arguments structure passed on the stack (the
  // outgoing pointer parameter to the native arguments structure is passed in
  // R0) and align frame before entering the C++ world.
  __ ReserveAlignedFrameSpace(sizeof(NativeArguments));

  // Initialize NativeArguments structure and call native function.
  // Registers R0, R1, R2, and R3 are used.

  ASSERT(thread_offset == 0 * kWordSize);
  // Set thread in NativeArgs.
  __ mov(R0, THR);

  // There are no native calls to closures, so we do not need to set the tag
  // bits kClosureFunctionBit and kInstanceFunctionBit in argc_tag_.
  ASSERT(argc_tag_offset == 1 * kWordSize);
  // Set argc in NativeArguments: R1 already contains argc.

  ASSERT(argv_offset == 2 * kWordSize);
  // Set argv in NativeArguments: R2 already contains argv.

  // Set retval in NativeArgs.
  ASSERT(retval_offset == 3 * kWordSize);
  __ AddImmediate(R3, FP, 2 * kWordSize);

  // Passing the structure by value as in runtime calls would require changing
  // Dart API for native functions.
  // For now, space is reserved on the stack and we pass a pointer to it.
  __ StoreToOffset(R0, SP, thread_offset);
  __ StoreToOffset(R1, SP, argc_tag_offset);
  __ StoreToOffset(R2, SP, argv_offset);
  __ StoreToOffset(R3, SP, retval_offset);
  __ mov(R0, SP);  // Pass the pointer to the NativeArguments.

  // We are entering runtime code, so the C stack pointer must be restored from
  // the stack limit to the top of the stack. We cache the stack limit address
  // in the Dart SP register, which is callee-saved in the C ABI.
  __ mov(R25, CSP);
  __ mov(CSP, SP);

  // Call native function or redirection via simulator.
  __ blr(R5);

  // Restore SP and CSP.
  __ mov(SP, CSP);
  __ mov(CSP, R25);

  // Refresh write barrier mask.
  __ ldr(BARRIER_MASK, Address(THR, Thread::write_barrier_mask_offset()));

  // Mark that the thread is executing Dart code.
  __ LoadImmediate(R2, VMTag::kDartTagId);
  __ StoreToOffset(R2, THR, Thread::vm_tag_offset());

  // Reset exit frame information in Isolate structure.
  __ StoreToOffset(ZR, THR, Thread::top_exit_frame_info_offset());

  __ LeaveStubFrame();
  __ ret();
}

// Input parameters:
//   R4: arguments descriptor array.
void StubCode::GenerateCallStaticFunctionStub(Assembler* assembler) {
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  // Setup space on stack for return value and preserve arguments descriptor.
  __ Push(R4);
  __ Push(ZR);
  __ CallRuntime(kPatchStaticCallRuntimeEntry, 0);
  // Get Code object result and restore arguments descriptor array.
  __ Pop(CODE_REG);
  __ Pop(R4);
  // Remove the stub frame.
  __ LeaveStubFrame();
  // Jump to the dart function.
  __ LoadFieldFromOffset(R0, CODE_REG, Code::entry_point_offset());
  __ br(R0);
}

// Called from a static call only when an invalid code has been entered
// (invalid because its function was optimized or deoptimized).
// R4: arguments descriptor array.
void StubCode::GenerateFixCallersTargetStub(Assembler* assembler) {
  // Load code pointer to this stub from the thread:
  // The one that is passed in, is not correct - it points to the code object
  // that needs to be replaced.
  __ ldr(CODE_REG, Address(THR, Thread::fix_callers_target_code_offset()));
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  // Setup space on stack for return value and preserve arguments descriptor.
  __ Push(R4);
  __ Push(ZR);
  __ CallRuntime(kFixCallersTargetRuntimeEntry, 0);
  // Get Code object result and restore arguments descriptor array.
  __ Pop(CODE_REG);
  __ Pop(R4);
  // Remove the stub frame.
  __ LeaveStubFrame();
  // Jump to the dart function.
  __ LoadFieldFromOffset(R0, CODE_REG, Code::entry_point_offset());
  __ br(R0);
}

// Called from object allocate instruction when the allocation stub has been
// disabled.
void StubCode::GenerateFixAllocationStubTargetStub(Assembler* assembler) {
  // Load code pointer to this stub from the thread:
  // The one that is passed in, is not correct - it points to the code object
  // that needs to be replaced.
  __ ldr(CODE_REG, Address(THR, Thread::fix_allocation_stub_code_offset()));
  __ EnterStubFrame();
  // Setup space on stack for return value.
  __ Push(ZR);
  __ CallRuntime(kFixAllocationStubTargetRuntimeEntry, 0);
  // Get Code object result.
  __ Pop(CODE_REG);
  // Remove the stub frame.
  __ LeaveStubFrame();
  // Jump to the dart function.
  __ LoadFieldFromOffset(R0, CODE_REG, Code::entry_point_offset());
  __ br(R0);
}

// Input parameters:
//   R2: smi-tagged argument count, may be zero.
//   FP[kParamEndSlotFromFp + 1]: last argument.
static void PushArrayOfArguments(Assembler* assembler) {
  // Allocate array to store arguments of caller.
  __ LoadObject(R1, Object::null_object());
  // R1: null element type for raw Array.
  // R2: smi-tagged argument count, may be zero.
  __ BranchLink(*StubCode::AllocateArray_entry());
  // R0: newly allocated array.
  // R2: smi-tagged argument count, may be zero (was preserved by the stub).
  __ Push(R0);  // Array is in R0 and on top of stack.
  __ add(R1, FP, Operand(R2, LSL, 2));
  __ AddImmediate(R1, kParamEndSlotFromFp * kWordSize);
  __ AddImmediate(R3, R0, Array::data_offset() - kHeapObjectTag);
  // R1: address of first argument on stack.
  // R3: address of first argument in array.

  Label loop, loop_exit;
  __ CompareRegisters(R2, ZR);
  __ b(&loop_exit, LE);
  __ Bind(&loop);
  __ ldr(R7, Address(R1));
  __ AddImmediate(R1, -kWordSize);
  __ AddImmediate(R3, kWordSize);
  __ AddImmediateSetFlags(R2, R2, -Smi::RawValue(1));
  __ str(R7, Address(R3, -kWordSize));
  __ b(&loop, GE);
  __ Bind(&loop_exit);
}

// Used by eager and lazy deoptimization. Preserve result in RAX if necessary.
// This stub translates optimized frame into unoptimized frame. The optimized
// frame can contain values in registers and on stack, the unoptimized
// frame contains all values on stack.
// Deoptimization occurs in following steps:
// - Push all registers that can contain values.
// - Call C routine to copy the stack and saved registers into temporary buffer.
// - Adjust caller's frame to correct unoptimized frame size.
// - Fill the unoptimized frame.
// - Materialize objects that require allocation (e.g. Double instances).
// GC can occur only after frame is fully rewritten.
// Stack after TagAndPushPP() below:
//   +------------------+
//   | Saved PP         | <- PP
//   +------------------+
//   | PC marker        | <- TOS
//   +------------------+
//   | Saved FP         | <- FP of stub
//   +------------------+
//   | return-address   |  (deoptimization point)
//   +------------------+
//   | Saved CODE_REG   |
//   +------------------+
//   | ...              | <- SP of optimized frame
//
// Parts of the code cannot GC, part of the code can GC.
static void GenerateDeoptimizationSequence(Assembler* assembler,
                                           DeoptStubKind kind) {
  // DeoptimizeCopyFrame expects a Dart frame, i.e. EnterDartFrame(0), but there
  // is no need to set the correct PC marker or load PP, since they get patched.
  __ EnterStubFrame();

  // The code in this frame may not cause GC. kDeoptimizeCopyFrameRuntimeEntry
  // and kDeoptimizeFillFrameRuntimeEntry are leaf runtime calls.
  const intptr_t saved_result_slot_from_fp =
      compiler_frame_layout.first_local_from_fp + 1 -
      (kNumberOfCpuRegisters - R0);
  const intptr_t saved_exception_slot_from_fp =
      compiler_frame_layout.first_local_from_fp + 1 -
      (kNumberOfCpuRegisters - R0);
  const intptr_t saved_stacktrace_slot_from_fp =
      compiler_frame_layout.first_local_from_fp + 1 -
      (kNumberOfCpuRegisters - R1);
  // Result in R0 is preserved as part of pushing all registers below.

  // Push registers in their enumeration order: lowest register number at
  // lowest address.
  for (intptr_t i = kNumberOfCpuRegisters - 1; i >= 0; i--) {
    const Register r = static_cast<Register>(i);
    if (r == CODE_REG) {
      // Save the original value of CODE_REG pushed before invoking this stub
      // instead of the value used to call this stub.
      COMPILE_ASSERT(R25 > CODE_REG);
      __ ldr(R25, Address(FP, 2 * kWordSize));
      __ str(R25, Address(SP, -1 * kWordSize, Address::PreIndex));
    } else {
      __ str(r, Address(SP, -1 * kWordSize, Address::PreIndex));
    }
  }

  for (intptr_t reg_idx = kNumberOfVRegisters - 1; reg_idx >= 0; reg_idx--) {
    VRegister vreg = static_cast<VRegister>(reg_idx);
    __ PushQuad(vreg);
  }

  __ mov(R0, SP);  // Pass address of saved registers block.
  bool is_lazy =
      (kind == kLazyDeoptFromReturn) || (kind == kLazyDeoptFromThrow);
  __ LoadImmediate(R1, is_lazy ? 1 : 0);
  __ ReserveAlignedFrameSpace(0);
  __ CallRuntime(kDeoptimizeCopyFrameRuntimeEntry, 2);
  // Result (R0) is stack-size (FP - SP) in bytes.

  if (kind == kLazyDeoptFromReturn) {
    // Restore result into R1 temporarily.
    __ LoadFromOffset(R1, FP, saved_result_slot_from_fp * kWordSize);
  } else if (kind == kLazyDeoptFromThrow) {
    // Restore result into R1 temporarily.
    __ LoadFromOffset(R1, FP, saved_exception_slot_from_fp * kWordSize);
    __ LoadFromOffset(R2, FP, saved_stacktrace_slot_from_fp * kWordSize);
  }

  // There is a Dart Frame on the stack. We must restore PP and leave frame.
  __ RestoreCodePointer();
  __ LeaveStubFrame();
  __ sub(SP, FP, Operand(R0));

  // DeoptimizeFillFrame expects a Dart frame, i.e. EnterDartFrame(0), but there
  // is no need to set the correct PC marker or load PP, since they get patched.
  __ EnterStubFrame();

  if (kind == kLazyDeoptFromReturn) {
    __ Push(R1);  // Preserve result as first local.
  } else if (kind == kLazyDeoptFromThrow) {
    __ Push(R1);  // Preserve exception as first local.
    __ Push(R2);  // Preserve stacktrace as second local.
  }
  __ ReserveAlignedFrameSpace(0);
  __ mov(R0, FP);  // Pass last FP as parameter in R0.
  __ CallRuntime(kDeoptimizeFillFrameRuntimeEntry, 1);
  if (kind == kLazyDeoptFromReturn) {
    // Restore result into R1.
    __ LoadFromOffset(R1, FP,
                      compiler_frame_layout.first_local_from_fp * kWordSize);
  } else if (kind == kLazyDeoptFromThrow) {
    // Restore result into R1.
    __ LoadFromOffset(R1, FP,
                      compiler_frame_layout.first_local_from_fp * kWordSize);
    __ LoadFromOffset(
        R2, FP, (compiler_frame_layout.first_local_from_fp - 1) * kWordSize);
  }
  // Code above cannot cause GC.
  // There is a Dart Frame on the stack. We must restore PP and leave frame.
  __ RestoreCodePointer();
  __ LeaveStubFrame();

  // Frame is fully rewritten at this point and it is safe to perform a GC.
  // Materialize any objects that were deferred by FillFrame because they
  // require allocation.
  // Enter stub frame with loading PP. The caller's PP is not materialized yet.
  __ EnterStubFrame();
  if (kind == kLazyDeoptFromReturn) {
    __ Push(R1);  // Preserve result, it will be GC-d here.
  } else if (kind == kLazyDeoptFromThrow) {
    __ Push(R1);  // Preserve exception, it will be GC-d here.
    __ Push(R2);  // Preserve stacktrace, it will be GC-d here.
  }

  __ Push(ZR);  // Space for the result.
  __ CallRuntime(kDeoptimizeMaterializeRuntimeEntry, 0);
  // Result tells stub how many bytes to remove from the expression stack
  // of the bottom-most frame. They were used as materialization arguments.
  __ Pop(R2);
  __ SmiUntag(R2);
  if (kind == kLazyDeoptFromReturn) {
    __ Pop(R0);  // Restore result.
  } else if (kind == kLazyDeoptFromThrow) {
    __ Pop(R1);  // Restore stacktrace.
    __ Pop(R0);  // Restore exception.
  }
  __ LeaveStubFrame();
  // Remove materialization arguments.
  __ add(SP, SP, Operand(R2));
  // The caller is responsible for emitting the return instruction.
}

// R0: result, must be preserved
void StubCode::GenerateDeoptimizeLazyFromReturnStub(Assembler* assembler) {
  // Push zap value instead of CODE_REG for lazy deopt.
  __ LoadImmediate(TMP, kZapCodeReg);
  __ Push(TMP);
  // Return address for "call" to deopt stub.
  __ LoadImmediate(LR, kZapReturnAddress);
  __ ldr(CODE_REG, Address(THR, Thread::lazy_deopt_from_return_stub_offset()));
  GenerateDeoptimizationSequence(assembler, kLazyDeoptFromReturn);
  __ ret();
}

// R0: exception, must be preserved
// R1: stacktrace, must be preserved
void StubCode::GenerateDeoptimizeLazyFromThrowStub(Assembler* assembler) {
  // Push zap value instead of CODE_REG for lazy deopt.
  __ LoadImmediate(TMP, kZapCodeReg);
  __ Push(TMP);
  // Return address for "call" to deopt stub.
  __ LoadImmediate(LR, kZapReturnAddress);
  __ ldr(CODE_REG, Address(THR, Thread::lazy_deopt_from_throw_stub_offset()));
  GenerateDeoptimizationSequence(assembler, kLazyDeoptFromThrow);
  __ ret();
}

void StubCode::GenerateDeoptimizeStub(Assembler* assembler) {
  __ Push(CODE_REG);
  __ ldr(CODE_REG, Address(THR, Thread::deoptimize_stub_offset()));
  GenerateDeoptimizationSequence(assembler, kEagerDeopt);
  __ ret();
}

static void GenerateDispatcherCode(Assembler* assembler,
                                   Label* call_target_function) {
  __ Comment("NoSuchMethodDispatch");
  // When lazily generated invocation dispatchers are disabled, the
  // miss-handler may return null.
  __ CompareObject(R0, Object::null_object());
  __ b(call_target_function, NE);
  __ EnterStubFrame();

  // Load the receiver.
  __ LoadFieldFromOffset(R2, R4, ArgumentsDescriptor::count_offset());
  __ add(TMP, FP, Operand(R2, LSL, 2));  // R2 is Smi.
  __ LoadFromOffset(R6, TMP, kParamEndSlotFromFp * kWordSize);
  __ Push(ZR);  // Result slot.
  __ Push(R6);  // Receiver.
  __ Push(R5);  // ICData/MegamorphicCache.
  __ Push(R4);  // Arguments descriptor.

  // Adjust arguments count.
  __ LoadFieldFromOffset(R3, R4, ArgumentsDescriptor::type_args_len_offset());
  __ AddImmediate(TMP, R2, 1);  // Include the type arguments.
  __ cmp(R3, Operand(0));
  __ csinc(R2, R2, TMP, EQ);  // R2 <- (R3 == 0) ? R2 : TMP + 1 (R2 : R2 + 2).

  // R2: Smi-tagged arguments array length.
  PushArrayOfArguments(assembler);
  const intptr_t kNumArgs = 4;
  __ CallRuntime(kInvokeNoSuchMethodDispatcherRuntimeEntry, kNumArgs);
  __ Drop(4);
  __ Pop(R0);  // Return value.
  __ LeaveStubFrame();
  __ ret();
}

void StubCode::GenerateMegamorphicMissStub(Assembler* assembler) {
  __ EnterStubFrame();

  // Load the receiver.
  __ LoadFieldFromOffset(R2, R4, ArgumentsDescriptor::count_offset());
  __ add(TMP, FP, Operand(R2, LSL, 2));  // R2 is Smi.
  __ LoadFromOffset(R6, TMP,
                    compiler_frame_layout.param_end_from_fp * kWordSize);

  // Preserve IC data and arguments descriptor.
  __ Push(R5);
  __ Push(R4);

  // Push space for the return value.
  // Push the receiver.
  // Push IC data object.
  // Push arguments descriptor array.
  __ Push(ZR);
  __ Push(R6);
  __ Push(R5);
  __ Push(R4);
  __ CallRuntime(kMegamorphicCacheMissHandlerRuntimeEntry, 3);
  // Remove arguments.
  __ Drop(3);
  __ Pop(R0);  // Get result into R0 (target function).

  // Restore IC data and arguments descriptor.
  __ Pop(R4);
  __ Pop(R5);

  __ RestoreCodePointer();
  __ LeaveStubFrame();

  if (!FLAG_lazy_dispatchers) {
    Label call_target_function;
    GenerateDispatcherCode(assembler, &call_target_function);
    __ Bind(&call_target_function);
  }

  // Tail-call to target function.
  __ LoadFieldFromOffset(CODE_REG, R0, Function::code_offset());
  __ LoadFieldFromOffset(R2, R0, Function::entry_point_offset());
  __ br(R2);
}

// Called for inline allocation of arrays.
// Input parameters:
//   LR: return address.
//   R2: array length as Smi.
//   R1: array element type (either NULL or an instantiated type).
// NOTE: R2 cannot be clobbered here as the caller relies on it being saved.
// The newly allocated object is returned in R0.
void StubCode::GenerateAllocateArrayStub(Assembler* assembler) {
  Label slow_case;
  // Compute the size to be allocated, it is based on the array length
  // and is computed as:
  // RoundedAllocationSize((array_length * kwordSize) + sizeof(RawArray)).
  // Assert that length is a Smi.
  __ tsti(R2, Immediate(kSmiTagMask));
  if (FLAG_use_slow_path) {
    __ b(&slow_case);
  } else {
    __ b(&slow_case, NE);
  }
  __ cmp(R2, Operand(0));
  __ b(&slow_case, LT);

  // Check for maximum allowed length.
  const intptr_t max_len =
      reinterpret_cast<intptr_t>(Smi::New(Array::kMaxNewSpaceElements));
  __ CompareImmediate(R2, max_len);
  __ b(&slow_case, GT);

  const intptr_t cid = kArrayCid;
  NOT_IN_PRODUCT(__ MaybeTraceAllocation(kArrayCid, R4, &slow_case));

  NOT_IN_PRODUCT(Heap::Space space = Heap::kNew);

  // Calculate and align allocation size.
  // Load new object start and calculate next object start.
  // R1: array element type.
  // R2: array length as Smi.
  __ ldr(R0, Address(THR, Thread::top_offset()));
  intptr_t fixed_size_plus_alignment_padding =
      sizeof(RawArray) + kObjectAlignment - 1;
  __ LoadImmediate(R3, fixed_size_plus_alignment_padding);
  __ add(R3, R3, Operand(R2, LSL, 2));  // R2 is Smi.
  ASSERT(kSmiTagShift == 1);
  __ andi(R3, R3, Immediate(~(kObjectAlignment - 1)));
  // R0: potential new object start.
  // R3: object size in bytes.
  __ adds(R7, R3, Operand(R0));
  __ b(&slow_case, CS);  // Branch if unsigned overflow.

  // Check if the allocation fits into the remaining space.
  // R0: potential new object start.
  // R1: array element type.
  // R2: array length as Smi.
  // R3: array size.
  // R7: potential next object start.
  __ LoadFromOffset(TMP, THR, Thread::end_offset());
  __ CompareRegisters(R7, TMP);
  __ b(&slow_case, CS);  // Branch if unsigned higher or equal.

  // Successfully allocated the object(s), now update top to point to
  // next object start and initialize the object.
  // R0: potential new object start.
  // R3: array size.
  // R7: potential next object start.
  __ str(R7, Address(THR, Thread::top_offset()));
  __ add(R0, R0, Operand(kHeapObjectTag));
  NOT_IN_PRODUCT(__ UpdateAllocationStatsWithSize(cid, R3, space));

  // R0: new object start as a tagged pointer.
  // R1: array element type.
  // R2: array length as Smi.
  // R3: array size.
  // R7: new object end address.

  // Store the type argument field.
  __ StoreIntoObjectOffsetNoBarrier(R0, Array::type_arguments_offset(), R1);

  // Set the length field.
  __ StoreIntoObjectOffsetNoBarrier(R0, Array::length_offset(), R2);

  // Calculate the size tag.
  // R0: new object start as a tagged pointer.
  // R2: array length as Smi.
  // R3: array size.
  // R7: new object end address.
  const intptr_t shift = RawObject::kSizeTagPos - kObjectAlignmentLog2;
  __ CompareImmediate(R3, RawObject::SizeTag::kMaxSizeTag);
  // If no size tag overflow, shift R1 left, else set R1 to zero.
  __ LslImmediate(TMP, R3, shift);
  __ csel(R1, TMP, R1, LS);
  __ csel(R1, ZR, R1, HI);

  // Get the class index and insert it into the tags.
  uint32_t tags = 0;
  tags = RawObject::ClassIdTag::update(cid, tags);
  tags = RawObject::NewBit::update(true, tags);
  __ LoadImmediate(TMP, tags);
  __ orr(R1, R1, Operand(TMP));
  __ StoreFieldToOffset(R1, R0, Array::tags_offset());

  // Initialize all array elements to raw_null.
  // R0: new object start as a tagged pointer.
  // R7: new object end address.
  // R2: array length as Smi.
  __ AddImmediate(R1, R0, Array::data_offset() - kHeapObjectTag);
  // R1: iterator which initially points to the start of the variable
  // data area to be initialized.
  __ LoadObject(TMP, Object::null_object());
  Label loop, done;
  __ Bind(&loop);
  // TODO(cshapiro): StoreIntoObjectNoBarrier
  __ CompareRegisters(R1, R7);
  __ b(&done, CS);
  __ str(TMP, Address(R1));  // Store if unsigned lower.
  __ AddImmediate(R1, kWordSize);
  __ b(&loop);  // Loop until R1 == R7.
  __ Bind(&done);

  // Done allocating and initializing the array.
  // R0: new object.
  // R2: array length as Smi (preserved for the caller.)
  __ ret();

  // Unable to allocate the array using the fast inline code, just call
  // into the runtime.
  __ Bind(&slow_case);
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  // Setup space on stack for return value.
  // Push array length as Smi and element type.
  __ Push(ZR);
  __ Push(R2);
  __ Push(R1);
  __ CallRuntime(kAllocateArrayRuntimeEntry, 2);
  // Pop arguments; result is popped in IP.
  __ Pop(R1);
  __ Pop(R2);
  __ Pop(R0);
  __ LeaveStubFrame();
  __ ret();
}

// Called when invoking Dart code from C++ (VM code).
// Input parameters:
//   LR : points to return address.
//   R0 : code object of the Dart function to call.
//   R1 : arguments descriptor array.
//   R2 : arguments array.
//   R3 : current thread.
void StubCode::GenerateInvokeDartCodeStub(Assembler* assembler) {
  __ Comment("InvokeDartCodeStub");

  // Copy the C stack pointer (R31) into the stack pointer we'll actually use
  // to access the stack.
  __ SetupDartSP();
  __ EnterFrame(0);

  // Push code object to PC marker slot.
  __ ldr(TMP, Address(R3, Thread::invoke_dart_code_stub_offset()));
  __ Push(TMP);

  // Save the callee-saved registers.
  for (int i = kAbiFirstPreservedCpuReg; i <= kAbiLastPreservedCpuReg; i++) {
    const Register r = static_cast<Register>(i);
    // We use str instead of the Push macro because we will be pushing the PP
    // register when it is not holding a pool-pointer since we are coming from
    // C++ code.
    __ str(r, Address(SP, -1 * kWordSize, Address::PreIndex));
  }

  // Save the bottom 64-bits of callee-saved V registers.
  for (int i = kAbiFirstPreservedFpuReg; i <= kAbiLastPreservedFpuReg; i++) {
    const VRegister r = static_cast<VRegister>(i);
    __ PushDouble(r);
  }

  // Set up THR, which caches the current thread in Dart code.
  if (THR != R3) {
    __ mov(THR, R3);
  }
  // Refresh write barrier mask.
  __ ldr(BARRIER_MASK, Address(THR, Thread::write_barrier_mask_offset()));

  // Save the current VMTag on the stack.
  __ LoadFromOffset(R4, THR, Thread::vm_tag_offset());
  __ Push(R4);

  // Mark that the thread is executing Dart code.
  __ LoadImmediate(R6, VMTag::kDartTagId);
  __ StoreToOffset(R6, THR, Thread::vm_tag_offset());

  // Save top resource and top exit frame info. Use R6 as a temporary register.
  // StackFrameIterator reads the top exit frame info saved in this frame.
  __ LoadFromOffset(R6, THR, Thread::top_resource_offset());
  __ StoreToOffset(ZR, THR, Thread::top_resource_offset());
  __ Push(R6);
  __ LoadFromOffset(R6, THR, Thread::top_exit_frame_info_offset());
  __ StoreToOffset(ZR, THR, Thread::top_exit_frame_info_offset());
  // kExitLinkSlotFromEntryFp must be kept in sync with the code below.
  ASSERT(kExitLinkSlotFromEntryFp == -22);
  __ Push(R6);

  // Load arguments descriptor array into R4, which is passed to Dart code.
  __ LoadFromOffset(R4, R1, VMHandles::kOffsetOfRawPtrInHandle);

  // Load number of arguments into R5 and adjust count for type arguments.
  __ LoadFieldFromOffset(R5, R4, ArgumentsDescriptor::count_offset());
  __ LoadFieldFromOffset(R3, R4, ArgumentsDescriptor::type_args_len_offset());
  __ AddImmediate(TMP, R5, 1);  // Include the type arguments.
  __ cmp(R3, Operand(0));
  __ csinc(R5, R5, TMP, EQ);  // R5 <- (R3 == 0) ? R5 : TMP + 1 (R5 : R5 + 2).
  __ SmiUntag(R5);

  // Compute address of 'arguments array' data area into R2.
  __ LoadFromOffset(R2, R2, VMHandles::kOffsetOfRawPtrInHandle);
  __ AddImmediate(R2, Array::data_offset() - kHeapObjectTag);

  // Set up arguments for the Dart call.
  Label push_arguments;
  Label done_push_arguments;
  __ cmp(R5, Operand(0));
  __ b(&done_push_arguments, EQ);  // check if there are arguments.
  __ LoadImmediate(R1, 0);
  __ Bind(&push_arguments);
  __ ldr(R3, Address(R2));
  __ Push(R3);
  __ add(R1, R1, Operand(1));
  __ add(R2, R2, Operand(kWordSize));
  __ cmp(R1, Operand(R5));
  __ b(&push_arguments, LT);
  __ Bind(&done_push_arguments);

  // We now load the pool pointer(PP) with a GC safe value as we are about to
  // invoke dart code. We don't need a real object pool here.
  // Smi zero does not work because ARM64 assumes PP to be untagged.
  __ LoadObject(PP, Object::null_object());

  // Call the Dart code entrypoint.
  __ ldr(CODE_REG, Address(R0, VMHandles::kOffsetOfRawPtrInHandle));
  __ ldr(R0, FieldAddress(CODE_REG, Code::entry_point_offset()));
  __ blr(R0);  // R4 is the arguments descriptor array.
  __ Comment("InvokeDartCodeStub return");

  // Get rid of arguments pushed on the stack.
  __ AddImmediate(SP, FP, kExitLinkSlotFromEntryFp * kWordSize);

  // Restore the saved top exit frame info and top resource back into the
  // Isolate structure. Uses R6 as a temporary register for this.
  __ Pop(R6);
  __ StoreToOffset(R6, THR, Thread::top_exit_frame_info_offset());
  __ Pop(R6);
  __ StoreToOffset(R6, THR, Thread::top_resource_offset());

  // Restore the current VMTag from the stack.
  __ Pop(R4);
  __ StoreToOffset(R4, THR, Thread::vm_tag_offset());

  // Restore the bottom 64-bits of callee-saved V registers.
  for (int i = kAbiLastPreservedFpuReg; i >= kAbiFirstPreservedFpuReg; i--) {
    const VRegister r = static_cast<VRegister>(i);
    __ PopDouble(r);
  }

  // Restore C++ ABI callee-saved registers.
  for (int i = kAbiLastPreservedCpuReg; i >= kAbiFirstPreservedCpuReg; i--) {
    Register r = static_cast<Register>(i);
    // We use ldr instead of the Pop macro because we will be popping the PP
    // register when it is not holding a pool-pointer since we are returning to
    // C++ code. We also skip the dart stack pointer SP, since we are still
    // using it as the stack pointer.
    __ ldr(r, Address(SP, 1 * kWordSize, Address::PostIndex));
  }

  // Restore the frame pointer and C stack pointer and return.
  __ LeaveFrame();
  __ RestoreCSP();
  __ ret();
}

// Called when invoking compiled Dart code from interpreted Dart code.
// Input parameters:
//   LR : points to return address.
//   R0 : raw code object of the Dart function to call.
//   R1 : arguments raw descriptor array.
//   R2 : address of first argument.
//   R3 : current thread.
void StubCode::GenerateInvokeDartCodeFromBytecodeStub(Assembler* assembler) {
#if defined(DART_PRECOMPILED_RUNTIME)
  __ Stop("Not using interpreter");
#else
  // Copy the C stack pointer (R31) into the stack pointer we'll actually use
  // to access the stack.
  __ SetupDartSP();
  __ EnterFrame(0);

  // Push code object to PC marker slot.
  __ ldr(TMP,
         Address(R3, Thread::invoke_dart_code_from_bytecode_stub_offset()));
  __ Push(TMP);

  // Save the callee-saved registers.
  for (int i = kAbiFirstPreservedCpuReg; i <= kAbiLastPreservedCpuReg; i++) {
    const Register r = static_cast<Register>(i);
    // We use str instead of the Push macro because we will be pushing the PP
    // register when it is not holding a pool-pointer since we are coming from
    // C++ code.
    __ str(r, Address(SP, -1 * kWordSize, Address::PreIndex));
  }

  // Save the bottom 64-bits of callee-saved V registers.
  for (int i = kAbiFirstPreservedFpuReg; i <= kAbiLastPreservedFpuReg; i++) {
    const VRegister r = static_cast<VRegister>(i);
    __ PushDouble(r);
  }

  // Set up THR, which caches the current thread in Dart code.
  if (THR != R3) {
    __ mov(THR, R3);
  }
  // Refresh write barrier mask.
  __ ldr(BARRIER_MASK, Address(THR, Thread::write_barrier_mask_offset()));

  // Save the current VMTag on the stack.
  __ LoadFromOffset(R4, THR, Thread::vm_tag_offset());
  __ Push(R4);

  // Mark that the thread is executing Dart code.
  __ LoadImmediate(R6, VMTag::kDartTagId);
  __ StoreToOffset(R6, THR, Thread::vm_tag_offset());

  // Save top resource and top exit frame info. Use R6 as a temporary register.
  // StackFrameIterator reads the top exit frame info saved in this frame.
  __ LoadFromOffset(R6, THR, Thread::top_resource_offset());
  __ StoreToOffset(ZR, THR, Thread::top_resource_offset());
  __ Push(R6);
  __ LoadFromOffset(R6, THR, Thread::top_exit_frame_info_offset());
  __ StoreToOffset(ZR, THR, Thread::top_exit_frame_info_offset());
  // kExitLinkSlotFromEntryFp must be kept in sync with the code below.
  ASSERT(kExitLinkSlotFromEntryFp == -22);
  __ Push(R6);

  // Load arguments descriptor array into R4, which is passed to Dart code.
  __ mov(R4, R1);

  // Load number of arguments into R5 and adjust count for type arguments.
  __ LoadFieldFromOffset(R5, R4, ArgumentsDescriptor::count_offset());
  __ LoadFieldFromOffset(R3, R4, ArgumentsDescriptor::type_args_len_offset());
  __ AddImmediate(TMP, R5, 1);  // Include the type arguments.
  __ cmp(R3, Operand(0));
  __ csinc(R5, R5, TMP, EQ);  // R5 <- (R3 == 0) ? R5 : TMP + 1 (R5 : R5 + 2).
  __ SmiUntag(R5);

  // R2 points to first argument.
  // Set up arguments for the Dart call.
  Label push_arguments;
  Label done_push_arguments;
  __ cmp(R5, Operand(0));
  __ b(&done_push_arguments, EQ);  // check if there are arguments.
  __ LoadImmediate(R1, 0);
  __ Bind(&push_arguments);
  __ ldr(R3, Address(R2));
  __ Push(R3);
  __ add(R1, R1, Operand(1));
  __ add(R2, R2, Operand(kWordSize));
  __ cmp(R1, Operand(R5));
  __ b(&push_arguments, LT);
  __ Bind(&done_push_arguments);

  // We now load the pool pointer(PP) with a GC safe value as we are about to
  // invoke dart code. We don't need a real object pool here.
  // Smi zero does not work because ARM64 assumes PP to be untagged.
  __ LoadObject(PP, Object::null_object());

  // Call the Dart code entrypoint.
  __ mov(CODE_REG, R0);
  __ ldr(R0, FieldAddress(CODE_REG, Code::entry_point_offset()));
  __ blr(R0);  // R4 is the arguments descriptor array.

  // Get rid of arguments pushed on the stack.
  __ AddImmediate(SP, FP, kExitLinkSlotFromEntryFp * kWordSize);

  // Restore the saved top exit frame info and top resource back into the
  // Isolate structure. Uses R6 as a temporary register for this.
  __ Pop(R6);
  __ StoreToOffset(R6, THR, Thread::top_exit_frame_info_offset());
  __ Pop(R6);
  __ StoreToOffset(R6, THR, Thread::top_resource_offset());

  // Restore the current VMTag from the stack.
  __ Pop(R4);
  __ StoreToOffset(R4, THR, Thread::vm_tag_offset());

  // Restore the bottom 64-bits of callee-saved V registers.
  for (int i = kAbiLastPreservedFpuReg; i >= kAbiFirstPreservedFpuReg; i--) {
    const VRegister r = static_cast<VRegister>(i);
    __ PopDouble(r);
  }

  // Restore C++ ABI callee-saved registers.
  for (int i = kAbiLastPreservedCpuReg; i >= kAbiFirstPreservedCpuReg; i--) {
    Register r = static_cast<Register>(i);
    // We use ldr instead of the Pop macro because we will be popping the PP
    // register when it is not holding a pool-pointer since we are returning to
    // C++ code. We also skip the dart stack pointer SP, since we are still
    // using it as the stack pointer.
    __ ldr(r, Address(SP, 1 * kWordSize, Address::PostIndex));
  }

  // Restore the frame pointer and C stack pointer and return.
  __ LeaveFrame();
  __ RestoreCSP();
  __ ret();
#endif  // defined(DART_PRECOMPILED_RUNTIME)
}

// Called for inline allocation of contexts.
// Input:
//   R1: number of context variables.
// Output:
//   R0: new allocated RawContext object.
void StubCode::GenerateAllocateContextStub(Assembler* assembler) {
  if (FLAG_inline_alloc) {
    Label slow_case;
    // First compute the rounded instance size.
    // R1: number of context variables.
    intptr_t fixed_size_plus_alignment_padding =
        sizeof(RawContext) + kObjectAlignment - 1;
    __ LoadImmediate(R2, fixed_size_plus_alignment_padding);
    __ add(R2, R2, Operand(R1, LSL, 3));
    ASSERT(kSmiTagShift == 1);
    __ andi(R2, R2, Immediate(~(kObjectAlignment - 1)));

    NOT_IN_PRODUCT(__ MaybeTraceAllocation(kContextCid, R4, &slow_case));
    // Now allocate the object.
    // R1: number of context variables.
    // R2: object size.
    const intptr_t cid = kContextCid;
    NOT_IN_PRODUCT(Heap::Space space = Heap::kNew);
    __ ldr(R0, Address(THR, Thread::top_offset()));
    __ add(R3, R2, Operand(R0));
    // Check if the allocation fits into the remaining space.
    // R0: potential new object.
    // R1: number of context variables.
    // R2: object size.
    // R3: potential next object start.
    __ ldr(TMP, Address(THR, Thread::end_offset()));
    __ CompareRegisters(R3, TMP);
    if (FLAG_use_slow_path) {
      __ b(&slow_case);
    } else {
      __ b(&slow_case, CS);  // Branch if unsigned higher or equal.
    }

    // Successfully allocated the object, now update top to point to
    // next object start and initialize the object.
    // R0: new object.
    // R1: number of context variables.
    // R2: object size.
    // R3: next object start.
    __ str(R3, Address(THR, Thread::top_offset()));
    __ add(R0, R0, Operand(kHeapObjectTag));
    NOT_IN_PRODUCT(__ UpdateAllocationStatsWithSize(cid, R2, space));

    // Calculate the size tag.
    // R0: new object.
    // R1: number of context variables.
    // R2: object size.
    const intptr_t shift = RawObject::kSizeTagPos - kObjectAlignmentLog2;
    __ CompareImmediate(R2, RawObject::SizeTag::kMaxSizeTag);
    // If no size tag overflow, shift R2 left, else set R2 to zero.
    __ LslImmediate(TMP, R2, shift);
    __ csel(R2, TMP, R2, LS);
    __ csel(R2, ZR, R2, HI);

    // Get the class index and insert it into the tags.
    // R2: size and bit tags.
    uint32_t tags = 0;
    tags = RawObject::ClassIdTag::update(cid, tags);
    tags = RawObject::NewBit::update(true, tags);
    __ LoadImmediate(TMP, tags);
    __ orr(R2, R2, Operand(TMP));
    __ StoreFieldToOffset(R2, R0, Context::tags_offset());

    // Setup up number of context variables field.
    // R0: new object.
    // R1: number of context variables as integer value (not object).
    __ StoreFieldToOffset(R1, R0, Context::num_variables_offset());

    // Setup the parent field.
    // R0: new object.
    // R1: number of context variables.
    __ LoadObject(R2, Object::null_object());
    __ StoreFieldToOffset(R2, R0, Context::parent_offset());

    // Initialize the context variables.
    // R0: new object.
    // R1: number of context variables.
    // R2: raw null.
    Label loop, done;
    __ AddImmediate(R3, R0, Context::variable_offset(0) - kHeapObjectTag);
    __ Bind(&loop);
    __ subs(R1, R1, Operand(1));
    __ b(&done, MI);
    __ str(R2, Address(R3, R1, UXTX, Address::Scaled));
    __ b(&loop, NE);  // Loop if R1 not zero.
    __ Bind(&done);

    // Done allocating and initializing the context.
    // R0: new object.
    __ ret();

    __ Bind(&slow_case);
  }
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  // Setup space on stack for return value.
  __ SmiTag(R1);
  __ PushObject(Object::null_object());
  __ Push(R1);
  __ CallRuntime(kAllocateContextRuntimeEntry, 1);  // Allocate context.
  __ Drop(1);  // Pop number of context variables argument.
  __ Pop(R0);  // Pop the new context object.
  // R0: new object
  // Restore the frame pointer.
  __ LeaveStubFrame();
  __ ret();
}

void StubCode::GenerateWriteBarrierWrappersStub(Assembler* assembler) {
  for (intptr_t i = 0; i < kNumberOfCpuRegisters; ++i) {
    if ((kDartAvailableCpuRegs & (1 << i)) == 0) continue;

    Register reg = static_cast<Register>(i);
    intptr_t start = __ CodeSize();
    __ Push(LR);
    __ Push(kWriteBarrierObjectReg);
    __ mov(kWriteBarrierObjectReg, reg);
    __ ldr(LR, Address(THR, Thread::write_barrier_entry_point_offset()));
    __ blr(LR);
    __ Pop(kWriteBarrierObjectReg);
    __ Pop(LR);
    __ ret(LR);
    intptr_t end = __ CodeSize();

    RELEASE_ASSERT(end - start == kStoreBufferWrapperSize);
  }
}

// Helper stub to implement Assembler::StoreIntoObject.
// Input parameters:
//   R1: Object (old)
//   R0: Value (old or new)
// If R0 is new, add R1 to the store buffer. Otherwise R0 is old, mark R0
// and add it to the mark list.
COMPILE_ASSERT(kWriteBarrierObjectReg == R1);
COMPILE_ASSERT(kWriteBarrierValueReg == R0);
void StubCode::GenerateWriteBarrierStub(Assembler* assembler) {
#if defined(CONCURRENT_MARKING)
  Label add_to_mark_stack;
  __ tbz(&add_to_mark_stack, R0, kNewObjectBitPosition);
#else
  Label add_to_buffer;
  // Check whether this object has already been remembered. Skip adding to the
  // store buffer if the object is in the store buffer already.
  __ LoadFieldFromOffset(TMP, R1, Object::tags_offset(), kWord);
  __ tbnz(&add_to_buffer, TMP, RawObject::kOldAndNotRememberedBit);
  __ ret();

  __ Bind(&add_to_buffer);
#endif

  // Save values being destroyed.
  __ Push(R2);
  __ Push(R3);
  __ Push(R4);

  // Atomically set the remembered bit of the object header.
  ASSERT(Object::tags_offset() == 0);
  __ sub(R3, R1, Operand(kHeapObjectTag));
  // R3: Untagged address of header word (ldxr/stxr do not support offsets).
  // Note that we use 32 bit operations here to match the size of the
  // background sweeper which is also manipulating this 32 bit word.
  Label retry;
  __ Bind(&retry);
  __ ldxr(R2, R3, kWord);
  __ AndImmediate(R2, R2, ~(1 << RawObject::kOldAndNotRememberedBit));
  __ stxr(R4, R2, R3, kWord);
  __ cbnz(&retry, R4);

  // Load the StoreBuffer block out of the thread. Then load top_ out of the
  // StoreBufferBlock and add the address to the pointers_.
  __ LoadFromOffset(R4, THR, Thread::store_buffer_block_offset());
  __ LoadFromOffset(R2, R4, StoreBufferBlock::top_offset(), kUnsignedWord);
  __ add(R3, R4, Operand(R2, LSL, kWordSizeLog2));
  __ StoreToOffset(R1, R3, StoreBufferBlock::pointers_offset());

  // Increment top_ and check for overflow.
  // R2: top_.
  // R4: StoreBufferBlock.
  Label overflow;
  __ add(R2, R2, Operand(1));
  __ StoreToOffset(R2, R4, StoreBufferBlock::top_offset(), kUnsignedWord);
  __ CompareImmediate(R2, StoreBufferBlock::kSize);
  // Restore values.
  __ Pop(R4);
  __ Pop(R3);
  __ Pop(R2);
  __ b(&overflow, EQ);
  __ ret();

  // Handle overflow: Call the runtime leaf function.
  __ Bind(&overflow);
  // Setup frame, push callee-saved registers.

  __ Push(CODE_REG);
  __ ldr(CODE_REG, Address(THR, Thread::write_barrier_code_offset()));
  __ EnterCallRuntimeFrame(0 * kWordSize);
  __ mov(R0, THR);
  __ CallRuntime(kStoreBufferBlockProcessRuntimeEntry, 1);
  // Restore callee-saved registers, tear down frame.
  __ LeaveCallRuntimeFrame();
  __ Pop(CODE_REG);
  __ ret();

#if defined(CONCURRENT_MARKING)
  __ Bind(&add_to_mark_stack);
  __ Push(R2);  // Spill.
  __ Push(R3);  // Spill.
  __ Push(R4);  // Spill.

  // Atomically clear kOldAndNotMarkedBit.
  // Note that we use 32 bit operations here to match the size of the
  // background sweeper which is also manipulating this 32 bit word.
  Label marking_retry, lost_race, marking_overflow;
  ASSERT(Object::tags_offset() == 0);
  __ sub(R3, R0, Operand(kHeapObjectTag));
  // R3: Untagged address of header word (ldxr/stxr do not support offsets).
  __ Bind(&marking_retry);
  __ ldxr(R2, R3, kWord);
  __ tbz(&lost_race, R2, RawObject::kOldAndNotMarkedBit);
  __ AndImmediate(R2, R2, ~(1 << RawObject::kOldAndNotMarkedBit));
  __ stxr(R4, R2, R3, kWord);
  __ cbnz(&marking_retry, R4);

  __ LoadFromOffset(R4, THR, Thread::marking_stack_block_offset());
  __ LoadFromOffset(R2, R4, MarkingStackBlock::top_offset(), kUnsignedWord);
  __ add(R3, R4, Operand(R2, LSL, kWordSizeLog2));
  __ StoreToOffset(R0, R3, MarkingStackBlock::pointers_offset());
  __ add(R2, R2, Operand(1));
  __ StoreToOffset(R2, R4, MarkingStackBlock::top_offset(), kUnsignedWord);
  __ CompareImmediate(R2, MarkingStackBlock::kSize);
  __ Pop(R4);  // Unspill.
  __ Pop(R3);  // Unspill.
  __ Pop(R2);  // Unspill.
  __ b(&marking_overflow, EQ);
  __ ret();

  __ Bind(&marking_overflow);
  __ Push(CODE_REG);
  __ ldr(CODE_REG, Address(THR, Thread::write_barrier_code_offset()));
  __ EnterCallRuntimeFrame(0 * kWordSize);
  __ mov(R0, THR);
  __ CallRuntime(kMarkingStackBlockProcessRuntimeEntry, 1);
  __ LeaveCallRuntimeFrame();
  __ Pop(CODE_REG);
  __ ret();

  __ Bind(&lost_race);
  __ Pop(R4);  // Unspill.
  __ Pop(R3);  // Unspill.
  __ Pop(R2);  // Unspill.
  __ ret();
#endif
}

// Called for inline allocation of objects.
// Input parameters:
//   LR : return address.
//   SP + 0 : type arguments object (only if class is parameterized).
void StubCode::GenerateAllocationStubForClass(Assembler* assembler,
                                              const Class& cls) {
  // The generated code is different if the class is parameterized.
  const bool is_cls_parameterized = cls.NumTypeArguments() > 0;
  ASSERT(!is_cls_parameterized ||
         (cls.type_arguments_field_offset() != Class::kNoTypeArguments));

  const Register kTypeArgumentsReg = R1;
  const Register kInstanceReg = R0;
  const Register kNullReg = R3;
  const Register kTempReg = R4;
  const Register kTopReg = R5;

  // kInlineInstanceSize is a constant used as a threshold for determining
  // when the object initialization should be done as a loop or as
  // straight line code.
  const int kInlineInstanceSize = 12;
  const intptr_t instance_size = cls.instance_size();
  ASSERT(instance_size > 0);
  if (is_cls_parameterized) {
    __ ldr(kTypeArgumentsReg, Address(SP));
  }
  Isolate* isolate = Isolate::Current();

  __ LoadObject(kNullReg, Object::null_object());
  if (FLAG_inline_alloc && Heap::IsAllocatableInNewSpace(instance_size) &&
      !cls.TraceAllocation(isolate)) {
    Label slow_case;
    // Allocate the object & initialize header word.
    __ TryAllocate(cls, &slow_case, kInstanceReg, kTopReg,
                   /*tag_result=*/false);

    // Initialize the remaining words of the object.
    if (instance_size < (kInlineInstanceSize * kWordSize)) {
      intptr_t current_offset = Instance::NextFieldOffset();
      while ((current_offset + kWordSize) < instance_size) {
        __ stp(kNullReg, kNullReg,
               Address(kInstanceReg, current_offset, Address::PairOffset));
        current_offset += 2 * kWordSize;
      }
      while (current_offset < instance_size) {
        __ str(kNullReg, Address(kInstanceReg, current_offset));
        current_offset += kWordSize;
      }
    } else {
      __ AddImmediate(kTempReg, kInstanceReg, Instance::NextFieldOffset());
      Label done, init_loop;
      __ Bind(&init_loop);
      __ CompareRegisters(kTempReg, kTopReg);
      __ b(&done, CS);
      __ str(kNullReg, Address(kTempReg, kWordSize, Address::PostIndex));
      __ b(&init_loop);

      __ Bind(&done);
    }
    if (is_cls_parameterized) {
      __ StoreToOffset(kTypeArgumentsReg, kInstanceReg,
                       cls.type_arguments_field_offset());
    }
    __ add(kInstanceReg, kInstanceReg, Operand(kHeapObjectTag));
    __ ret();

    __ Bind(&slow_case);
  }

  // If is_cls_parameterized:
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();  // Uses pool pointer to pass cls to runtime.
  __ LoadObject(R0, cls);
  __ PushPair(R0, kNullReg);  // Pushes cls, result slot.
  __ Push(is_cls_parameterized ? kTypeArgumentsReg : kNullReg);
  __ CallRuntime(kAllocateObjectRuntimeEntry, 2);  // Allocate object.
  __ ldr(kInstanceReg,
         Address(SP, 2 * kWordSize));  // Pop result (newly allocated object).
  __ LeaveStubFrame();                 // Restores correct SP.
  __ ret();
}

// Called for invoking "dynamic noSuchMethod(Invocation invocation)" function
// from the entry code of a dart function after an error in passed argument
// name or number is detected.
// Input parameters:
//  LR : return address.
//  SP : address of last argument.
//  R4: arguments descriptor array.
void StubCode::GenerateCallClosureNoSuchMethodStub(Assembler* assembler) {
  __ EnterStubFrame();

  // Load the receiver.
  __ LoadFieldFromOffset(R2, R4, ArgumentsDescriptor::count_offset());
  __ add(TMP, FP, Operand(R2, LSL, 2));  // R2 is Smi.
  __ LoadFromOffset(R6, TMP, kParamEndSlotFromFp * kWordSize);

  // Push space for the return value.
  // Push the receiver.
  // Push arguments descriptor array.
  __ Push(ZR);
  __ Push(R6);
  __ Push(R4);

  // Adjust arguments count.
  __ LoadFieldFromOffset(R3, R4, ArgumentsDescriptor::type_args_len_offset());
  __ AddImmediate(TMP, R2, 1);  // Include the type arguments.
  __ cmp(R3, Operand(0));
  __ csinc(R2, R2, TMP, EQ);  // R2 <- (R3 == 0) ? R2 : TMP + 1 (R2 : R2 + 2).

  // R2: Smi-tagged arguments array length.
  PushArrayOfArguments(assembler);

  const intptr_t kNumArgs = 3;
  __ CallRuntime(kInvokeClosureNoSuchMethodRuntimeEntry, kNumArgs);
  // noSuchMethod on closures always throws an error, so it will never return.
  __ brk(0);
}

//  R6: function object.
//  R5: inline cache data object.
// Cannot use function object from ICData as it may be the inlined
// function and not the top-scope function.
void StubCode::GenerateOptimizedUsageCounterIncrement(Assembler* assembler) {
  Register ic_reg = R5;
  Register func_reg = R6;
  if (FLAG_trace_optimized_ic_calls) {
    __ EnterStubFrame();
    __ Push(R6);        // Preserve.
    __ Push(R5);        // Preserve.
    __ Push(ic_reg);    // Argument.
    __ Push(func_reg);  // Argument.
    __ CallRuntime(kTraceICCallRuntimeEntry, 2);
    __ Drop(2);  // Discard argument;
    __ Pop(R5);  // Restore.
    __ Pop(R6);  // Restore.
    __ LeaveStubFrame();
  }
  __ LoadFieldFromOffset(R7, func_reg, Function::usage_counter_offset(), kWord);
  __ add(R7, R7, Operand(1));
  __ StoreFieldToOffset(R7, func_reg, Function::usage_counter_offset(), kWord);
}

// Loads function into 'temp_reg'.
void StubCode::GenerateUsageCounterIncrement(Assembler* assembler,
                                             Register temp_reg) {
  if (FLAG_optimization_counter_threshold >= 0) {
    Register ic_reg = R5;
    Register func_reg = temp_reg;
    ASSERT(temp_reg == R6);
    __ Comment("Increment function counter");
    __ LoadFieldFromOffset(func_reg, ic_reg, ICData::owner_offset());
    __ LoadFieldFromOffset(R7, func_reg, Function::usage_counter_offset(),
                           kWord);
    __ AddImmediate(R7, 1);
    __ StoreFieldToOffset(R7, func_reg, Function::usage_counter_offset(),
                          kWord);
  }
}

// Note: R5 must be preserved.
// Attempt a quick Smi operation for known operations ('kind'). The ICData
// must have been primed with a Smi/Smi check that will be used for counting
// the invocations.
static void EmitFastSmiOp(Assembler* assembler,
                          Token::Kind kind,
                          intptr_t num_args,
                          Label* not_smi_or_overflow) {
  __ Comment("Fast Smi op");
  __ ldr(R0, Address(SP, +0 * kWordSize));  // Right.
  __ ldr(R1, Address(SP, +1 * kWordSize));  // Left.
  __ orr(TMP, R0, Operand(R1));
  __ BranchIfNotSmi(TMP, not_smi_or_overflow);
  switch (kind) {
    case Token::kADD: {
      __ adds(R0, R1, Operand(R0));   // Adds.
      __ b(not_smi_or_overflow, VS);  // Branch if overflow.
      break;
    }
    case Token::kSUB: {
      __ subs(R0, R1, Operand(R0));   // Subtract.
      __ b(not_smi_or_overflow, VS);  // Branch if overflow.
      break;
    }
    case Token::kEQ: {
      __ CompareRegisters(R0, R1);
      __ LoadObject(R0, Bool::True());
      __ LoadObject(R1, Bool::False());
      __ csel(R0, R1, R0, NE);
      break;
    }
    default:
      UNIMPLEMENTED();
  }

  // R5: IC data object (preserved).
  __ LoadFieldFromOffset(R6, R5, ICData::ic_data_offset());
  // R6: ic_data_array with check entries: classes and target functions.
  __ AddImmediate(R6, Array::data_offset() - kHeapObjectTag);
// R6: points directly to the first ic data array element.
#if defined(DEBUG)
  // Check that first entry is for Smi/Smi.
  Label error, ok;
  const intptr_t imm_smi_cid = reinterpret_cast<intptr_t>(Smi::New(kSmiCid));
  __ ldr(R1, Address(R6, 0));
  __ CompareImmediate(R1, imm_smi_cid);
  __ b(&error, NE);
  __ ldr(R1, Address(R6, kWordSize));
  __ CompareImmediate(R1, imm_smi_cid);
  __ b(&ok, EQ);
  __ Bind(&error);
  __ Stop("Incorrect IC data");
  __ Bind(&ok);
#endif
  if (FLAG_optimization_counter_threshold >= 0) {
    const intptr_t count_offset = ICData::CountIndexFor(num_args) * kWordSize;
    // Update counter, ignore overflow.
    __ LoadFromOffset(R1, R6, count_offset);
    __ adds(R1, R1, Operand(Smi::RawValue(1)));
    __ StoreToOffset(R1, R6, count_offset);
  }

  __ ret();
}

// Generate inline cache check for 'num_args'.
//  LR: return address.
//  R5: inline cache data object.
// Control flow:
// - If receiver is null -> jump to IC miss.
// - If receiver is Smi -> load Smi class.
// - If receiver is not-Smi -> load receiver's class.
// - Check if 'num_args' (including receiver) match any IC data group.
// - Match found -> jump to target.
// - Match not found -> jump to IC miss.
void StubCode::GenerateNArgsCheckInlineCacheStub(
    Assembler* assembler,
    intptr_t num_args,
    const RuntimeEntry& handle_ic_miss,
    Token::Kind kind,
    bool optimized,
    bool exactness_check /* = false */) {
  ASSERT(!exactness_check);
  ASSERT(num_args == 1 || num_args == 2);
#if defined(DEBUG)
  {
    Label ok;
    // Check that the IC data array has NumArgsTested() == num_args.
    // 'NumArgsTested' is stored in the least significant bits of 'state_bits'.
    __ LoadFromOffset(R6, R5, ICData::state_bits_offset() - kHeapObjectTag,
                      kUnsignedWord);
    ASSERT(ICData::NumArgsTestedShift() == 0);  // No shift needed.
    __ andi(R6, R6, Immediate(ICData::NumArgsTestedMask()));
    __ CompareImmediate(R6, num_args);
    __ b(&ok, EQ);
    __ Stop("Incorrect stub for IC data");
    __ Bind(&ok);
  }
#endif  // DEBUG

#if !defined(PRODUCT)
  Label stepping, done_stepping;
  if (!optimized) {
    __ Comment("Check single stepping");
    __ LoadIsolate(R6);
    __ LoadFromOffset(R6, R6, Isolate::single_step_offset(), kUnsignedByte);
    __ CompareRegisters(R6, ZR);
    __ b(&stepping, NE);
    __ Bind(&done_stepping);
  }
#endif

  Label not_smi_or_overflow;
  if (kind != Token::kILLEGAL) {
    EmitFastSmiOp(assembler, kind, num_args, &not_smi_or_overflow);
  }
  __ Bind(&not_smi_or_overflow);

  __ Comment("Extract ICData initial values and receiver cid");
  // Load arguments descriptor into R4.
  __ LoadFieldFromOffset(R4, R5, ICData::arguments_descriptor_offset());
  // Loop that checks if there is an IC data match.
  Label loop, found, miss;
  // R5: IC data object (preserved).
  __ LoadFieldFromOffset(R6, R5, ICData::ic_data_offset());
  // R6: ic_data_array with check entries: classes and target functions.
  __ AddImmediate(R6, Array::data_offset() - kHeapObjectTag);
  // R6: points directly to the first ic data array element.

  // Get the receiver's class ID (first read number of arguments from
  // arguments descriptor array and then access the receiver from the stack).
  __ LoadFieldFromOffset(R7, R4, ArgumentsDescriptor::count_offset());
  __ SmiUntag(R7);  // Untag so we can use the LSL 3 addressing mode.
  __ sub(R7, R7, Operand(1));

  // R0 <- [SP + (R7 << 3)]
  __ ldr(R0, Address(SP, R7, UXTX, Address::Scaled));
  __ LoadTaggedClassIdMayBeSmi(R0, R0);

  if (num_args == 2) {
    __ AddImmediate(R1, R7, -1);
    // R1 <- [SP + (R1 << 3)]
    __ ldr(R1, Address(SP, R1, UXTX, Address::Scaled));
    __ LoadTaggedClassIdMayBeSmi(R1, R1);
  }

  // We unroll the generic one that is generated once more than the others.
  const bool optimize = kind == Token::kILLEGAL;

  __ Comment("ICData loop");
  __ Bind(&loop);
  for (int unroll = optimize ? 4 : 2; unroll >= 0; unroll--) {
    Label update;

    __ LoadFromOffset(R2, R6, 0);
    __ CompareRegisters(R0, R2);  // Class id match?
    if (num_args == 2) {
      __ b(&update, NE);  // Continue.
      __ LoadFromOffset(R2, R6, kWordSize);
      __ CompareRegisters(R1, R2);  // Class id match?
    }
    __ b(&found, EQ);  // Break.

    __ Bind(&update);

    const intptr_t entry_size =
        ICData::TestEntryLengthFor(num_args, exactness_check) * kWordSize;
    __ AddImmediate(R6, entry_size);  // Next entry.

    __ CompareImmediate(R2, Smi::RawValue(kIllegalCid));  // Done?
    if (unroll == 0) {
      __ b(&loop, NE);
    } else {
      __ b(&miss, EQ);
    }
  }

  __ Bind(&miss);
  __ Comment("IC miss");
  // Compute address of arguments.
  // R7: argument_count - 1 (untagged).
  // R7 <- SP + (R7 << 3)
  __ add(R7, SP, Operand(R7, UXTX, 3));  // R7 is Untagged.
  // R7: address of receiver.
  // Create a stub frame as we are pushing some objects on the stack before
  // calling into the runtime.
  __ EnterStubFrame();
  // Preserve IC data object and arguments descriptor array and
  // setup space on stack for result (target code object).
  __ Push(R4);  // Preserve arguments descriptor array.
  __ Push(R5);  // Preserve IC Data.
  // Setup space on stack for the result (target code object).
  __ Push(ZR);
  // Push call arguments.
  for (intptr_t i = 0; i < num_args; i++) {
    __ LoadFromOffset(TMP, R7, -i * kWordSize);
    __ Push(TMP);
  }
  // Pass IC data object.
  __ Push(R5);
  __ CallRuntime(handle_ic_miss, num_args + 1);
  // Remove the call arguments pushed earlier, including the IC data object.
  __ Drop(num_args + 1);
  // Pop returned function object into R0.
  // Restore arguments descriptor array and IC data array.
  __ Pop(R0);  // Pop returned function object into R0.
  __ Pop(R5);  // Restore IC Data.
  __ Pop(R4);  // Restore arguments descriptor array.
  __ RestoreCodePointer();
  __ LeaveStubFrame();
  Label call_target_function;
  if (!FLAG_lazy_dispatchers) {
    GenerateDispatcherCode(assembler, &call_target_function);
  } else {
    __ b(&call_target_function);
  }

  __ Bind(&found);
  __ Comment("Update caller's counter");
  // R6: pointer to an IC data check group.
  const intptr_t target_offset = ICData::TargetIndexFor(num_args) * kWordSize;
  const intptr_t count_offset = ICData::CountIndexFor(num_args) * kWordSize;
  __ LoadFromOffset(R0, R6, target_offset);

  if (FLAG_optimization_counter_threshold >= 0) {
    // Update counter, ignore overflow.
    __ LoadFromOffset(R1, R6, count_offset);
    __ adds(R1, R1, Operand(Smi::RawValue(1)));
    __ StoreToOffset(R1, R6, count_offset);
  }

  __ Comment("Call target");
  __ Bind(&call_target_function);
  // R0: target function.
  __ LoadFieldFromOffset(CODE_REG, R0, Function::code_offset());
  __ LoadFieldFromOffset(R2, R0, Function::entry_point_offset());
  __ br(R2);

#if !defined(PRODUCT)
  if (!optimized) {
    __ Bind(&stepping);
    __ EnterStubFrame();
    __ Push(R5);  // Preserve IC data.
    __ CallRuntime(kSingleStepHandlerRuntimeEntry, 0);
    __ Pop(R5);
    __ RestoreCodePointer();
    __ LeaveStubFrame();
    __ b(&done_stepping);
  }
#endif
}

// Use inline cache data array to invoke the target or continue in inline
// cache miss handler. Stub for 1-argument check (receiver class).
//  LR: return address.
//  R5: inline cache data object.
// Inline cache data object structure:
// 0: function-name
// 1: N, number of arguments checked.
// 2 .. (length - 1): group of checks, each check containing:
//   - N classes.
//   - 1 target function.
void StubCode::GenerateOneArgCheckInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
  GenerateNArgsCheckInlineCacheStub(
      assembler, 1, kInlineCacheMissHandlerOneArgRuntimeEntry, Token::kILLEGAL);
}

void StubCode::GenerateOneArgCheckInlineCacheWithExactnessCheckStub(
    Assembler* assembler) {
  __ Stop("Unimplemented");
}

void StubCode::GenerateTwoArgsCheckInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
  GenerateNArgsCheckInlineCacheStub(assembler, 2,
                                    kInlineCacheMissHandlerTwoArgsRuntimeEntry,
                                    Token::kILLEGAL);
}

void StubCode::GenerateSmiAddInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
  GenerateNArgsCheckInlineCacheStub(
      assembler, 2, kInlineCacheMissHandlerTwoArgsRuntimeEntry, Token::kADD);
}

void StubCode::GenerateSmiSubInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
  GenerateNArgsCheckInlineCacheStub(
      assembler, 2, kInlineCacheMissHandlerTwoArgsRuntimeEntry, Token::kSUB);
}

void StubCode::GenerateSmiEqualInlineCacheStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
  GenerateNArgsCheckInlineCacheStub(
      assembler, 2, kInlineCacheMissHandlerTwoArgsRuntimeEntry, Token::kEQ);
}

void StubCode::GenerateOneArgOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 1,
                                    kInlineCacheMissHandlerOneArgRuntimeEntry,
                                    Token::kILLEGAL, true /* optimized */);
}

void StubCode::GenerateOneArgOptimizedCheckInlineCacheWithExactnessCheckStub(
    Assembler* assembler) {
  __ Stop("Unimplemented");
}

void StubCode::GenerateTwoArgsOptimizedCheckInlineCacheStub(
    Assembler* assembler) {
  GenerateOptimizedUsageCounterIncrement(assembler);
  GenerateNArgsCheckInlineCacheStub(assembler, 2,
                                    kInlineCacheMissHandlerTwoArgsRuntimeEntry,
                                    Token::kILLEGAL, true /* optimized */);
}

void StubCode::GenerateZeroArgsUnoptimizedStaticCallStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
#if defined(DEBUG)
  {
    Label ok;
    // Check that the IC data array has NumArgsTested() == 0.
    // 'NumArgsTested' is stored in the least significant bits of 'state_bits'.
    __ LoadFromOffset(R6, R5, ICData::state_bits_offset() - kHeapObjectTag,
                      kUnsignedWord);
    ASSERT(ICData::NumArgsTestedShift() == 0);  // No shift needed.
    __ andi(R6, R6, Immediate(ICData::NumArgsTestedMask()));
    __ CompareImmediate(R6, 0);
    __ b(&ok, EQ);
    __ Stop("Incorrect IC data for unoptimized static call");
    __ Bind(&ok);
  }
#endif  // DEBUG

  // Check single stepping.
#if !defined(PRODUCT)
  Label stepping, done_stepping;
  __ LoadIsolate(R6);
  __ LoadFromOffset(R6, R6, Isolate::single_step_offset(), kUnsignedByte);
  __ CompareImmediate(R6, 0);
  __ b(&stepping, NE);
  __ Bind(&done_stepping);
#endif

  // R5: IC data object (preserved).
  __ LoadFieldFromOffset(R6, R5, ICData::ic_data_offset());
  // R6: ic_data_array with entries: target functions and count.
  __ AddImmediate(R6, Array::data_offset() - kHeapObjectTag);
  // R6: points directly to the first ic data array element.
  const intptr_t target_offset = ICData::TargetIndexFor(0) * kWordSize;
  const intptr_t count_offset = ICData::CountIndexFor(0) * kWordSize;

  if (FLAG_optimization_counter_threshold >= 0) {
    // Increment count for this call, ignore overflow.
    __ LoadFromOffset(R1, R6, count_offset);
    __ adds(R1, R1, Operand(Smi::RawValue(1)));
    __ StoreToOffset(R1, R6, count_offset);
  }

  // Load arguments descriptor into R4.
  __ LoadFieldFromOffset(R4, R5, ICData::arguments_descriptor_offset());

  // Get function and call it, if possible.
  __ LoadFromOffset(R0, R6, target_offset);
  __ LoadFieldFromOffset(CODE_REG, R0, Function::code_offset());
  __ LoadFieldFromOffset(R2, R0, Function::entry_point_offset());
  __ br(R2);

#if !defined(PRODUCT)
  __ Bind(&stepping);
  __ EnterStubFrame();
  __ Push(R5);  // Preserve IC data.
  __ CallRuntime(kSingleStepHandlerRuntimeEntry, 0);
  __ Pop(R5);
  __ RestoreCodePointer();
  __ LeaveStubFrame();
  __ b(&done_stepping);
#endif
}

void StubCode::GenerateOneArgUnoptimizedStaticCallStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
  GenerateNArgsCheckInlineCacheStub(
      assembler, 1, kStaticCallMissHandlerOneArgRuntimeEntry, Token::kILLEGAL);
}

void StubCode::GenerateTwoArgsUnoptimizedStaticCallStub(Assembler* assembler) {
  GenerateUsageCounterIncrement(assembler, R6);
  GenerateNArgsCheckInlineCacheStub(
      assembler, 2, kStaticCallMissHandlerTwoArgsRuntimeEntry, Token::kILLEGAL);
}

// Stub for compiling a function and jumping to the compiled code.
// R4: Arguments descriptor.
// R0: Function.
void StubCode::GenerateLazyCompileStub(Assembler* assembler) {
  // Preserve arg desc.
  __ EnterStubFrame();
  __ Push(R4);  // Save arg. desc.
  __ Push(R0);  // Pass function.
  __ CallRuntime(kCompileFunctionRuntimeEntry, 1);
  __ Pop(R0);  // Restore argument.
  __ Pop(R4);  // Restore arg desc.
  __ LeaveStubFrame();

  // When using the interpreter, the function's code may now point to the
  // InterpretCall stub. Make sure R0, R4, and R5 are preserved.
  __ LoadFieldFromOffset(CODE_REG, R0, Function::code_offset());
  __ LoadFieldFromOffset(R2, R0, Function::entry_point_offset());
  __ br(R2);
}

// Stub for interpreting a function call.
// R4: Arguments descriptor.
// R0: Function.
void StubCode::GenerateInterpretCallStub(Assembler* assembler) {
#if defined(DART_PRECOMPILED_RUNTIME)
  __ Stop("Not using interpreter")
#else
  __ SetPrologueOffset();
  __ EnterStubFrame();

#if defined(DEBUG)
  {
    Label ok;
    // Check that we are always entering from Dart code.
    __ LoadFromOffset(R8, THR, Thread::vm_tag_offset());
    __ CompareImmediate(R8, VMTag::kDartTagId);
    __ b(&ok, EQ);
    __ Stop("Not coming from Dart code.");
    __ Bind(&ok);
  }
#endif

  // Adjust arguments count for type arguments vector.
  __ LoadFieldFromOffset(R2, R4, ArgumentsDescriptor::count_offset());
  __ SmiUntag(R2);
  __ LoadFieldFromOffset(R1, R4, ArgumentsDescriptor::type_args_len_offset());
  __ cmp(R1, Operand(0));
  __ csinc(R2, R2, R2, EQ);  // R2 <- (R1 == 0) ? R2 : R2 + 1.

  // Compute argv.
  __ add(R3, ZR, Operand(R2, LSL, 3));
  __ add(R3, FP, Operand(R3));
  __ AddImmediate(R3, kParamEndSlotFromFp * kWordSize);

  // Indicate decreasing memory addresses of arguments with negative argc.
  __ neg(R2, R2);

  // Align frame before entering C++ world. No shadow stack space required.
  __ ReserveAlignedFrameSpace(0 * kWordSize);

  // Pass arguments in registers.
  // R0: Function.
  __ mov(R1, R4);  // Arguments descriptor.
  // R2: Negative argc.
  // R3: Argv.
  __ mov(R4, THR);  // Thread.

  // Save exit frame information to enable stack walking as we are about
  // to transition to Dart VM C++ code.
  __ StoreToOffset(FP, THR, Thread::top_exit_frame_info_offset());

  // Mark that the thread is executing VM code.
  __ LoadFromOffset(R5, THR, Thread::interpret_call_entry_point_offset());
  __ StoreToOffset(R5, THR, Thread::vm_tag_offset());

  // We are entering runtime code, so the C stack pointer must be restored from
  // the stack limit to the top of the stack. We cache the stack limit address
  // in a callee-saved register.
  __ mov(R25, CSP);
  __ mov(CSP, SP);

  __ blr(R5);

  // Restore SP and CSP.
  __ mov(SP, CSP);
  __ mov(CSP, R25);

  // Refresh write barrier mask.
  __ ldr(BARRIER_MASK, Address(THR, Thread::write_barrier_mask_offset()));

  // Mark that the thread is executing Dart code.
  __ LoadImmediate(R2, VMTag::kDartTagId);
  __ StoreToOffset(R2, THR, Thread::vm_tag_offset());

  // Reset exit frame information in Isolate structure.
  __ StoreToOffset(ZR, THR, Thread::top_exit_frame_info_offset());

  __ LeaveStubFrame();
  __ ret();
#endif  // defined(DART_PRECOMPILED_RUNTIME)
}

// R5: Contains an ICData.
void StubCode::GenerateICCallBreakpointStub(Assembler* assembler) {
  __ EnterStubFrame();
  __ Push(R5);
  __ Push(ZR);  // Space for result.
  __ CallRuntime(kBreakpointRuntimeHandlerRuntimeEntry, 0);
  __ Pop(CODE_REG);
  __ Pop(R5);
  __ LeaveStubFrame();
  __ LoadFieldFromOffset(R0, CODE_REG, Code::entry_point_offset());
  __ br(R0);
}

void StubCode::GenerateRuntimeCallBreakpointStub(Assembler* assembler) {
  __ EnterStubFrame();
  __ Push(ZR);  // Space for result.
  __ CallRuntime(kBreakpointRuntimeHandlerRuntimeEntry, 0);
  __ Pop(CODE_REG);
  __ LeaveStubFrame();
  __ LoadFieldFromOffset(R0, CODE_REG, Code::entry_point_offset());
  __ br(R0);
}

// Called only from unoptimized code. All relevant registers have been saved.
void StubCode::GenerateDebugStepCheckStub(Assembler* assembler) {
  // Check single stepping.
  Label stepping, done_stepping;
  __ LoadIsolate(R1);
  __ LoadFromOffset(R1, R1, Isolate::single_step_offset(), kUnsignedByte);
  __ CompareImmediate(R1, 0);
  __ b(&stepping, NE);
  __ Bind(&done_stepping);

  __ ret();

  __ Bind(&stepping);
  __ EnterStubFrame();
  __ CallRuntime(kSingleStepHandlerRuntimeEntry, 0);
  __ LeaveStubFrame();
  __ b(&done_stepping);
}

// Used to check class and type arguments. Arguments passed in registers:
// LR: return address.
// R0: instance (must be preserved).
// R1: instantiator type arguments (only if n == 4, can be raw_null).
// R2: function type arguments (only if n == 4, can be raw_null).
// R3: SubtypeTestCache.
//
// Preserves R0/R2/R8.
//
// Result in R1: null -> not found, otherwise result (true or false).
static void GenerateSubtypeNTestCacheStub(Assembler* assembler, int n) {
  ASSERT(n == 1 || n == 2 || n == 4 || n == 6);

  const Register kCacheReg = R3;
  const Register kInstanceReg = R0;
  const Register kInstantiatorTypeArgumentsReg = R1;
  const Register kFunctionTypeArgumentsReg = R2;

  const Register kInstanceCidOrFunction = R6;
  const Register kInstanceInstantiatorTypeArgumentsReg = R4;
  const Register kInstanceParentFunctionTypeArgumentsReg = R9;
  const Register kInstanceDelayedFunctionTypeArgumentsReg = R10;

  const Register kNullReg = R7;

  __ LoadObject(kNullReg, Object::null_object());

  // Loop initialization (moved up here to avoid having all dependent loads
  // after each other).
  __ ldr(kCacheReg, FieldAddress(kCacheReg, SubtypeTestCache::cache_offset()));
  __ AddImmediate(kCacheReg, Array::data_offset() - kHeapObjectTag);

  Label loop, not_closure;
  if (n >= 4) {
    __ LoadClassIdMayBeSmi(kInstanceCidOrFunction, kInstanceReg);
  } else {
    __ LoadClassId(kInstanceCidOrFunction, kInstanceReg);
  }
  __ CompareImmediate(kInstanceCidOrFunction, kClosureCid);
  __ b(&not_closure, NE);

  // Closure handling.
  {
    __ ldr(kInstanceCidOrFunction,
           FieldAddress(kInstanceReg, Closure::function_offset()));
    if (n >= 2) {
      __ ldr(kInstanceInstantiatorTypeArgumentsReg,
             FieldAddress(kInstanceReg,
                          Closure::instantiator_type_arguments_offset()));
      if (n >= 6) {
        ASSERT(n == 6);
        __ ldr(kInstanceParentFunctionTypeArgumentsReg,
               FieldAddress(kInstanceReg,
                            Closure::function_type_arguments_offset()));
        __ ldr(kInstanceDelayedFunctionTypeArgumentsReg,
               FieldAddress(kInstanceReg,
                            Closure::delayed_type_arguments_offset()));
      }
    }
    __ b(&loop);
  }

  // Non-Closure handling.
  {
    __ Bind(&not_closure);
    if (n == 1) {
      __ SmiTag(kInstanceCidOrFunction);
    } else {
      ASSERT(n >= 2);
      Label has_no_type_arguments;
      // [LoadClassById] also tags [kInstanceCidOrFunction] as a side-effect.
      __ LoadClassById(R5, kInstanceCidOrFunction);
      __ mov(kInstanceInstantiatorTypeArgumentsReg, kNullReg);
      __ LoadFieldFromOffset(
          R5, R5, Class::type_arguments_field_offset_in_words_offset(), kWord);
      __ CompareImmediate(R5, Class::kNoTypeArguments);
      __ b(&has_no_type_arguments, EQ);
      __ add(R5, kInstanceReg, Operand(R5, LSL, 3));
      __ ldr(kInstanceInstantiatorTypeArgumentsReg, FieldAddress(R5, 0));
      __ Bind(&has_no_type_arguments);

      if (n >= 6) {
        __ mov(kInstanceParentFunctionTypeArgumentsReg, kNullReg);
        __ mov(kInstanceDelayedFunctionTypeArgumentsReg, kNullReg);
      }
    }
  }

  Label found, not_found, next_iteration;

  // Loop header
  __ Bind(&loop);
  __ ldr(R5, Address(kCacheReg,
                     kWordSize * SubtypeTestCache::kInstanceClassIdOrFunction));
  __ cmp(R5, Operand(kNullReg));
  __ b(&not_found, EQ);
  __ cmp(R5, Operand(kInstanceCidOrFunction));
  if (n == 1) {
    __ b(&found, EQ);
  } else {
    __ b(&next_iteration, NE);
    __ ldr(R5, Address(kCacheReg,
                       kWordSize * SubtypeTestCache::kInstanceTypeArguments));
    __ cmp(R5, Operand(kInstanceInstantiatorTypeArgumentsReg));
    if (n == 2) {
      __ b(&found, EQ);
    } else {
      __ b(&next_iteration, NE);
      __ ldr(R5,
             Address(kCacheReg,
                     kWordSize * SubtypeTestCache::kInstantiatorTypeArguments));
      __ cmp(R5, Operand(kInstantiatorTypeArgumentsReg));
      __ b(&next_iteration, NE);
      __ ldr(R5, Address(kCacheReg,
                         kWordSize * SubtypeTestCache::kFunctionTypeArguments));
      __ cmp(R5, Operand(kFunctionTypeArgumentsReg));
      if (n == 4) {
        __ b(&found, EQ);
      } else {
        ASSERT(n == 6);
        __ b(&next_iteration, NE);

        __ ldr(R5,
               Address(
                   kCacheReg,
                   kWordSize *
                       SubtypeTestCache::kInstanceParentFunctionTypeArguments));
        __ cmp(R5, Operand(kInstanceParentFunctionTypeArgumentsReg));
        __ b(&next_iteration, NE);

        __ ldr(
            R5,
            Address(
                kCacheReg,
                kWordSize *
                    SubtypeTestCache::kInstanceDelayedFunctionTypeArguments));
        __ cmp(R5, Operand(kInstanceDelayedFunctionTypeArgumentsReg));
        __ b(&found, EQ);
      }
    }
  }
  __ Bind(&next_iteration);
  __ AddImmediate(kCacheReg, kWordSize * SubtypeTestCache::kTestEntryLength);
  __ b(&loop);

  __ Bind(&found);
  __ ldr(R1, Address(kCacheReg, kWordSize * SubtypeTestCache::kTestResult));
  __ ret();

  __ Bind(&not_found);
  __ mov(R1, kNullReg);
  __ ret();
}

// See comment on [GenerateSubtypeNTestCacheStub].
void StubCode::GenerateSubtype1TestCacheStub(Assembler* assembler) {
  GenerateSubtypeNTestCacheStub(assembler, 1);
}

// See comment on [GenerateSubtypeNTestCacheStub].
void StubCode::GenerateSubtype2TestCacheStub(Assembler* assembler) {
  GenerateSubtypeNTestCacheStub(assembler, 2);
}

// See comment on [GenerateSubtypeNTestCacheStub].
void StubCode::GenerateSubtype4TestCacheStub(Assembler* assembler) {
  GenerateSubtypeNTestCacheStub(assembler, 4);
}

// See comment on [GenerateSubtypeNTestCacheStub].
void StubCode::GenerateSubtype6TestCacheStub(Assembler* assembler) {
  GenerateSubtypeNTestCacheStub(assembler, 6);
}

// Used to test whether a given value is of a given type (different variants,
// all have the same calling convention).
//
// Inputs:
//   - R0 : instance to test against.
//   - R2 : instantiator type arguments (if needed).
//   - R1 : function type arguments (if needed).
//
//   - R3 : subtype test cache.
//
//   - R8 : type to test against.
//   - R4 : name of destination variable.
//
// Preserves R0/R2.
//
// Note of warning: The caller will not populate CODE_REG and we have therefore
// no access to the pool.
void StubCode::GenerateDefaultTypeTestStub(Assembler* assembler) {
  Label done;

  const Register kInstanceReg = R0;
  const Register kDstTypeReg = R8;

  // Fast case for 'null'.
  __ CompareObject(kInstanceReg, Object::null_object());
  __ BranchIf(EQUAL, &done);

  // Fast case for 'int'.
  Label not_smi;
  __ BranchIfNotSmi(kInstanceReg, &not_smi);
  __ CompareObject(kDstTypeReg, Object::ZoneHandle(Type::IntType()));
  __ BranchIf(EQUAL, &done);
  __ Bind(&not_smi);

  // Tail call the [SubtypeTestCache]-based implementation.
  __ ldr(CODE_REG, Address(THR, Thread::slow_type_test_stub_offset()));
  __ ldr(R9, FieldAddress(CODE_REG, Code::entry_point_offset()));
  __ br(R9);

  __ Bind(&done);
  __ Ret();
}

void StubCode::GenerateTopTypeTypeTestStub(Assembler* assembler) {
  __ Ret();
}

void StubCode::GenerateTypeRefTypeTestStub(Assembler* assembler) {
  const Register kTypeRefReg = R8;

  // We dereference the TypeRef and tail-call to it's type testing stub.
  __ ldr(kTypeRefReg, FieldAddress(kTypeRefReg, TypeRef::type_offset()));
  __ ldr(R9, FieldAddress(kTypeRefReg,
                          AbstractType::type_test_stub_entry_point_offset()));
  __ br(R9);
}

void StubCode::GenerateUnreachableTypeTestStub(Assembler* assembler) {
  __ Breakpoint();
}

void TypeTestingStubGenerator::BuildOptimizedTypeTestStub(
    Assembler* assembler,
    HierarchyInfo* hi,
    const Type& type,
    const Class& type_class) {
  const Register kInstanceReg = R0;
  const Register kClassIdReg = R9;

  BuildOptimizedTypeTestStubFastCases(assembler, hi, type, type_class,
                                      kInstanceReg, kClassIdReg);

  __ ldr(CODE_REG, Address(THR, Thread::slow_type_test_stub_offset()));
  __ ldr(R9, FieldAddress(CODE_REG, Code::entry_point_offset()));
  __ br(R9);
}

void TypeTestingStubGenerator::
    BuildOptimizedSubclassRangeCheckWithTypeArguments(Assembler* assembler,
                                                      HierarchyInfo* hi,
                                                      const Class& type_class,
                                                      const TypeArguments& tp,
                                                      const TypeArguments& ta) {
  const Register kInstanceReg = R0;
  const Register kInstanceTypeArguments = R7;
  const Register kClassIdReg = R9;

  BuildOptimizedSubclassRangeCheckWithTypeArguments(
      assembler, hi, type_class, tp, ta, kClassIdReg, kInstanceReg,
      kInstanceTypeArguments);
}

void TypeTestingStubGenerator::BuildOptimizedTypeArgumentValueCheck(
    Assembler* assembler,
    HierarchyInfo* hi,
    const AbstractType& type_arg,
    intptr_t type_param_value_offset_i,
    Label* check_failed) {
  const Register kInstantiatorTypeArgumentsReg = R1;
  const Register kFunctionTypeArgumentsReg = R2;
  const Register kInstanceTypeArguments = R7;

  const Register kClassIdReg = R9;
  const Register kOwnTypeArgumentValue = TMP;

  BuildOptimizedTypeArgumentValueCheck(
      assembler, hi, type_arg, type_param_value_offset_i, kClassIdReg,
      kInstanceTypeArguments, kInstantiatorTypeArgumentsReg,
      kFunctionTypeArgumentsReg, kOwnTypeArgumentValue, check_failed);
}

static void InvokeTypeCheckFromTypeTestStub(Assembler* assembler,
                                            TypeCheckMode mode) {
  const Register kInstanceReg = R0;
  const Register kInstantiatorTypeArgumentsReg = R1;
  const Register kFunctionTypeArgumentsReg = R2;

  const Register kSubtypeTestCacheReg = R3;
  const Register kDstTypeReg = R8;

  __ PushObject(Object::null_object());  // Make room for result.
  __ Push(kInstanceReg);
  __ Push(kDstTypeReg);
  __ Push(kInstantiatorTypeArgumentsReg);
  __ Push(kFunctionTypeArgumentsReg);
  __ PushObject(Object::null_object());
  __ Push(kSubtypeTestCacheReg);
  __ PushObject(Smi::ZoneHandle(Smi::New(mode)));
  __ CallRuntime(kTypeCheckRuntimeEntry, 7);
  __ Drop(1);  // mode
  __ Pop(kSubtypeTestCacheReg);
  __ Drop(1);  // dst_name
  __ Pop(kFunctionTypeArgumentsReg);
  __ Pop(kInstantiatorTypeArgumentsReg);
  __ Pop(kDstTypeReg);
  __ Pop(kInstanceReg);
  __ Drop(1);  // Discard return value.
}

void StubCode::GenerateLazySpecializeTypeTestStub(Assembler* assembler) {
  const Register kInstanceReg = R0;
  Label done;

  __ CompareObject(kInstanceReg, Object::null_object());
  __ BranchIf(EQUAL, &done);

  __ ldr(CODE_REG,
         Address(THR, Thread::lazy_specialize_type_test_stub_offset()));
  __ EnterStubFrame();
  InvokeTypeCheckFromTypeTestStub(assembler, kTypeCheckFromLazySpecializeStub);
  __ LeaveStubFrame();

  __ Bind(&done);
  __ Ret();
}

void StubCode::GenerateSlowTypeTestStub(Assembler* assembler) {
  Label done, call_runtime;

  const Register kInstanceReg = R0;
  const Register kInstantiatorTypeArgumentsReg = R1;
  const Register kSubtypeTestCacheReg = R3;
  const Register kDstTypeReg = R8;

  __ EnterStubFrame();

#ifdef DEBUG
  // Guaranteed by caller.
  Label no_error;
  __ CompareObject(kInstanceReg, Object::null_object());
  __ BranchIf(NOT_EQUAL, &no_error);
  __ Breakpoint();
  __ Bind(&no_error);
#endif

  // If the subtype-cache is null, it needs to be lazily-created by the runtime.
  __ CompareObject(kSubtypeTestCacheReg, Object::null_object());
  __ BranchIf(EQUAL, &call_runtime);

  const Register kTmp = R9;

  // If this is not a [Type] object, we'll go to the runtime.
  Label is_simple_case, is_complex_case;
  __ LoadClassId(kTmp, kDstTypeReg);
  __ cmp(kTmp, Operand(kTypeCid));
  __ BranchIf(NOT_EQUAL, &is_complex_case);

  // Check whether this [Type] is instantiated/uninstantiated.
  __ ldr(kTmp, FieldAddress(kDstTypeReg, Type::type_state_offset()), kByte);
  __ cmp(kTmp, Operand(RawType::kFinalizedInstantiated));
  __ BranchIf(NOT_EQUAL, &is_complex_case);

  // Check whether this [Type] is a function type.
  __ ldr(kTmp, FieldAddress(kDstTypeReg, Type::signature_offset()));
  __ CompareObject(kTmp, Object::null_object());
  __ BranchIf(NOT_EQUAL, &is_complex_case);

  // This [Type] could be a FutureOr. Subtype2TestCache does not support Smi.
  __ BranchIfSmi(kInstanceReg, &is_complex_case);

  // Fall through to &is_simple_case

  __ Bind(&is_simple_case);
  {
    __ PushPair(kInstantiatorTypeArgumentsReg, kSubtypeTestCacheReg);
    __ BranchLink(*StubCode::Subtype2TestCache_entry());
    __ CompareObject(R1, Bool::True());
    __ PopPair(kInstantiatorTypeArgumentsReg, kSubtypeTestCacheReg);
    __ BranchIf(EQUAL, &done);  // Cache said: yes.
    __ Jump(&call_runtime);
  }

  __ Bind(&is_complex_case);
  {
    __ PushPair(kInstantiatorTypeArgumentsReg, kSubtypeTestCacheReg);
    __ BranchLink(*StubCode::Subtype6TestCache_entry());
    __ CompareObject(R1, Bool::True());
    __ PopPair(kInstantiatorTypeArgumentsReg, kSubtypeTestCacheReg);
    __ BranchIf(EQUAL, &done);  // Cache said: yes.
    // Fall through to runtime_call
  }

  __ Bind(&call_runtime);

  // We cannot really ensure here that dynamic/Object/void never occur here
  // (though it is guaranteed at dart_precompiled_runtime time).  This is
  // because we do constant evaluation with default stubs and only install
  // optimized versions before writing out the AOT snapshot.
  // So dynamic/Object/void will run with default stub in constant evaluation.
  __ CompareObject(kDstTypeReg, Type::dynamic_type());
  __ BranchIf(EQUAL, &done);
  __ CompareObject(kDstTypeReg, Type::Handle(Type::ObjectType()));
  __ BranchIf(EQUAL, &done);
  __ CompareObject(kDstTypeReg, Type::void_type());
  __ BranchIf(EQUAL, &done);

  InvokeTypeCheckFromTypeTestStub(assembler, kTypeCheckFromSlowStub);

  __ Bind(&done);
  __ LeaveStubFrame();
  __ Ret();
}

void StubCode::GenerateGetCStackPointerStub(Assembler* assembler) {
  __ mov(R0, CSP);
  __ ret();
}

// Jump to a frame on the call stack.
// LR: return address.
// R0: program_counter.
// R1: stack_pointer.
// R2: frame_pointer.
// R3: thread.
// Does not return.
void StubCode::GenerateJumpToFrameStub(Assembler* assembler) {
  ASSERT(kExceptionObjectReg == R0);
  ASSERT(kStackTraceObjectReg == R1);
  __ mov(LR, R0);  // Program counter.
  __ mov(SP, R1);  // Stack pointer.
  __ mov(FP, R2);  // Frame_pointer.
  __ mov(THR, R3);
  __ ldr(BARRIER_MASK, Address(THR, Thread::write_barrier_mask_offset()));
  // Set the tag.
  __ LoadImmediate(R2, VMTag::kDartTagId);
  __ StoreToOffset(R2, THR, Thread::vm_tag_offset());
  // Clear top exit frame.
  __ StoreToOffset(ZR, THR, Thread::top_exit_frame_info_offset());
  // Restore the pool pointer.
  __ RestoreCodePointer();
  __ LoadPoolPointer();
  __ ret();  // Jump to continuation point.
}

// Run an exception handler.  Execution comes from JumpToFrame
// stub or from the simulator.
//
// The arguments are stored in the Thread object.
// Does not return.
void StubCode::GenerateRunExceptionHandlerStub(Assembler* assembler) {
  __ LoadFromOffset(LR, THR, Thread::resume_pc_offset());

  ASSERT(Thread::CanLoadFromThread(Object::null_object()));
  __ LoadFromOffset(R2, THR, Thread::OffsetFromThread(Object::null_object()));

  // Exception object.
  __ LoadFromOffset(R0, THR, Thread::active_exception_offset());
  __ StoreToOffset(R2, THR, Thread::active_exception_offset());

  // StackTrace object.
  __ LoadFromOffset(R1, THR, Thread::active_stacktrace_offset());
  __ StoreToOffset(R2, THR, Thread::active_stacktrace_offset());

  __ ret();  // Jump to the exception handler code.
}

// Deoptimize a frame on the call stack before rewinding.
// The arguments are stored in the Thread object.
// No result.
void StubCode::GenerateDeoptForRewindStub(Assembler* assembler) {
  // Push zap value instead of CODE_REG.
  __ LoadImmediate(TMP, kZapCodeReg);
  __ Push(TMP);

  // Load the deopt pc into LR.
  __ LoadFromOffset(LR, THR, Thread::resume_pc_offset());
  GenerateDeoptimizationSequence(assembler, kEagerDeopt);

  // After we have deoptimized, jump to the correct frame.
  __ EnterStubFrame();
  __ CallRuntime(kRewindPostDeoptRuntimeEntry, 0);
  __ LeaveStubFrame();
  __ brk(0);
}

// Calls to the runtime to optimize the given function.
// R6: function to be re-optimized.
// R4: argument descriptor (preserved).
void StubCode::GenerateOptimizeFunctionStub(Assembler* assembler) {
  __ EnterStubFrame();
  __ Push(R4);
  // Setup space on stack for the return value.
  __ Push(ZR);
  __ Push(R6);
  __ CallRuntime(kOptimizeInvokedFunctionRuntimeEntry, 1);
  __ Pop(R0);  // Discard argument.
  __ Pop(R0);  // Get Function object
  __ Pop(R4);  // Restore argument descriptor.
  __ LoadFieldFromOffset(CODE_REG, R0, Function::code_offset());
  __ LoadFieldFromOffset(R1, R0, Function::entry_point_offset());
  __ LeaveStubFrame();
  __ br(R1);
  __ brk(0);
}

// Does identical check (object references are equal or not equal) with special
// checks for boxed numbers.
// Left and right are pushed on stack.
// Return Zero condition flag set if equal.
// Note: A Mint cannot contain a value that would fit in Smi.
static void GenerateIdenticalWithNumberCheckStub(Assembler* assembler,
                                                 const Register left,
                                                 const Register right) {
  Label reference_compare, done, check_mint;
  // If any of the arguments is Smi do reference compare.
  __ BranchIfSmi(left, &reference_compare);
  __ BranchIfSmi(right, &reference_compare);

  // Value compare for two doubles.
  __ CompareClassId(left, kDoubleCid);
  __ b(&check_mint, NE);
  __ CompareClassId(right, kDoubleCid);
  __ b(&done, NE);

  // Double values bitwise compare.
  __ LoadFieldFromOffset(left, left, Double::value_offset());
  __ LoadFieldFromOffset(right, right, Double::value_offset());
  __ b(&reference_compare);

  __ Bind(&check_mint);
  __ CompareClassId(left, kMintCid);
  __ b(&reference_compare, NE);
  __ CompareClassId(right, kMintCid);
  __ b(&done, NE);
  __ LoadFieldFromOffset(left, left, Mint::value_offset());
  __ LoadFieldFromOffset(right, right, Mint::value_offset());

  __ Bind(&reference_compare);
  __ CompareRegisters(left, right);
  __ Bind(&done);
}

// Called only from unoptimized code. All relevant registers have been saved.
// LR: return address.
// SP + 4: left operand.
// SP + 0: right operand.
// Return Zero condition flag set if equal.
void StubCode::GenerateUnoptimizedIdenticalWithNumberCheckStub(
    Assembler* assembler) {
#if !defined(PRODUCT)
  // Check single stepping.
  Label stepping, done_stepping;
  __ LoadIsolate(R1);
  __ LoadFromOffset(R1, R1, Isolate::single_step_offset(), kUnsignedByte);
  __ CompareImmediate(R1, 0);
  __ b(&stepping, NE);
  __ Bind(&done_stepping);
#endif

  const Register left = R1;
  const Register right = R0;
  __ LoadFromOffset(left, SP, 1 * kWordSize);
  __ LoadFromOffset(right, SP, 0 * kWordSize);
  GenerateIdenticalWithNumberCheckStub(assembler, left, right);
  __ ret();

#if !defined(PRODUCT)
  __ Bind(&stepping);
  __ EnterStubFrame();
  __ CallRuntime(kSingleStepHandlerRuntimeEntry, 0);
  __ RestoreCodePointer();
  __ LeaveStubFrame();
  __ b(&done_stepping);
#endif
}

// Called from optimized code only.
// LR: return address.
// SP + 4: left operand.
// SP + 0: right operand.
// Return Zero condition flag set if equal.
void StubCode::GenerateOptimizedIdenticalWithNumberCheckStub(
    Assembler* assembler) {
  const Register left = R1;
  const Register right = R0;
  __ LoadFromOffset(left, SP, 1 * kWordSize);
  __ LoadFromOffset(right, SP, 0 * kWordSize);
  GenerateIdenticalWithNumberCheckStub(assembler, left, right);
  __ ret();
}

// Called from megamorphic calls.
//  R0: receiver
//  R5: MegamorphicCache (preserved)
// Passed to target:
//  CODE_REG: target Code
//  R4: arguments descriptor
void StubCode::GenerateMegamorphicCallStub(Assembler* assembler) {
  // Jump if receiver is a smi.
  Label smi_case;
  __ BranchIfSmi(R0, &smi_case);

  // Loads the cid of the object.
  __ LoadClassId(R0, R0);

  Label cid_loaded;
  __ Bind(&cid_loaded);
  __ ldr(R2, FieldAddress(R5, MegamorphicCache::buckets_offset()));
  __ ldr(R1, FieldAddress(R5, MegamorphicCache::mask_offset()));
  // R2: cache buckets array.
  // R1: mask as a smi.

  // Make the cid into a smi.
  __ SmiTag(R0);
  // R0: class ID of the receiver (smi).

  // Compute the table index.
  ASSERT(MegamorphicCache::kSpreadFactor == 7);
  // Use lsl and sub to multiply with 7 == 8 - 1.
  __ LslImmediate(R3, R0, 3);
  __ sub(R3, R3, Operand(R0));
  // R3: probe.
  Label loop;
  __ Bind(&loop);
  __ and_(R3, R3, Operand(R1));

  const intptr_t base = Array::data_offset();
  // R3 is smi tagged, but table entries are 16 bytes, so LSL 3.
  __ add(TMP, R2, Operand(R3, LSL, 3));
  __ ldr(R6, FieldAddress(TMP, base));
  Label probe_failed;
  __ CompareRegisters(R6, R0);
  __ b(&probe_failed, NE);

  Label load_target;
  __ Bind(&load_target);
  // Call the target found in the cache.  For a class id match, this is a
  // proper target for the given name and arguments descriptor.  If the
  // illegal class id was found, the target is a cache miss handler that can
  // be invoked as a normal Dart function.
  __ ldr(R0, FieldAddress(TMP, base + kWordSize));
  __ ldr(R4, FieldAddress(R5, MegamorphicCache::arguments_descriptor_offset()));
  __ ldr(R1, FieldAddress(R0, Function::entry_point_offset()));
  __ ldr(CODE_REG, FieldAddress(R0, Function::code_offset()));
  __ br(R1);

  // Probe failed, check if it is a miss.
  __ Bind(&probe_failed);
  ASSERT(kIllegalCid == 0);
  __ tst(R6, Operand(R6));
  __ b(&load_target, EQ);  // branch if miss.

  // Try next extry in the table.
  __ AddImmediate(R3, Smi::RawValue(1));
  __ b(&loop);

  // Load cid for the Smi case.
  __ Bind(&smi_case);
  __ LoadImmediate(R0, kSmiCid);
  __ b(&cid_loaded);
}

// Called from switchable IC calls.
//  R0: receiver
//  R5: ICData (preserved)
// Passed to target:
//  CODE_REG: target Code object
//  R4: arguments descriptor
void StubCode::GenerateICCallThroughFunctionStub(Assembler* assembler) {
  Label loop, found, miss;
  __ ldr(R4, FieldAddress(R5, ICData::arguments_descriptor_offset()));
  __ ldr(R8, FieldAddress(R5, ICData::ic_data_offset()));
  __ AddImmediate(R8, Array::data_offset() - kHeapObjectTag);
  // R8: first IC entry
  __ LoadTaggedClassIdMayBeSmi(R1, R0);
  // R1: receiver cid as Smi

  __ Bind(&loop);
  __ ldr(R2, Address(R8, 0));
  __ cmp(R1, Operand(R2));
  __ b(&found, EQ);
  __ CompareImmediate(R2, Smi::RawValue(kIllegalCid));
  __ b(&miss, EQ);

  const intptr_t entry_length =
      ICData::TestEntryLengthFor(1, /*tracking_exactness=*/false) * kWordSize;
  __ AddImmediate(R8, entry_length);  // Next entry.
  __ b(&loop);

  __ Bind(&found);
  const intptr_t target_offset = ICData::TargetIndexFor(1) * kWordSize;
  __ ldr(R0, Address(R8, target_offset));
  __ ldr(R1, FieldAddress(R0, Function::entry_point_offset()));
  __ ldr(CODE_REG, FieldAddress(R0, Function::code_offset()));
  __ br(R1);

  __ Bind(&miss);
  __ LoadIsolate(R2);
  __ ldr(CODE_REG, Address(R2, Isolate::ic_miss_code_offset()));
  __ ldr(R1, FieldAddress(CODE_REG, Code::entry_point_offset()));
  __ br(R1);
}

void StubCode::GenerateICCallThroughCodeStub(Assembler* assembler) {
  Label loop, found, miss;
  __ ldr(R4, FieldAddress(R5, ICData::arguments_descriptor_offset()));
  __ ldr(R8, FieldAddress(R5, ICData::ic_data_offset()));
  __ AddImmediate(R8, Array::data_offset() - kHeapObjectTag);
  // R8: first IC entry
  __ LoadTaggedClassIdMayBeSmi(R1, R0);
  // R1: receiver cid as Smi

  __ Bind(&loop);
  __ ldr(R2, Address(R8, 0));
  __ cmp(R1, Operand(R2));
  __ b(&found, EQ);
  __ CompareImmediate(R2, Smi::RawValue(kIllegalCid));
  __ b(&miss, EQ);

  const intptr_t entry_length =
      ICData::TestEntryLengthFor(1, /*tracking_exactness=*/false) * kWordSize;
  __ AddImmediate(R8, entry_length);  // Next entry.
  __ b(&loop);

  __ Bind(&found);
  const intptr_t code_offset = ICData::CodeIndexFor(1) * kWordSize;
  const intptr_t entry_offset = ICData::EntryPointIndexFor(1) * kWordSize;
  __ ldr(R1, Address(R8, entry_offset));
  __ ldr(CODE_REG, Address(R8, code_offset));
  __ br(R1);

  __ Bind(&miss);
  __ LoadIsolate(R2);
  __ ldr(CODE_REG, Address(R2, Isolate::ic_miss_code_offset()));
  __ ldr(R1, FieldAddress(CODE_REG, Code::entry_point_offset()));
  __ br(R1);
}

// Called from switchable IC calls.
//  R0: receiver
//  R5: SingleTargetCache
void StubCode::GenerateUnlinkedCallStub(Assembler* assembler) {
  __ EnterStubFrame();
  __ Push(R0);  // Preserve receiver.

  __ Push(ZR);  // Result slot.
  __ Push(R0);  // Arg0: Receiver
  __ Push(R5);  // Arg1: UnlinkedCall
  __ CallRuntime(kUnlinkedCallRuntimeEntry, 2);
  __ Drop(2);
  __ Pop(R5);  // result = IC

  __ Pop(R0);  // Restore receiver.
  __ LeaveStubFrame();

  __ ldr(CODE_REG, Address(THR, Thread::ic_lookup_through_code_stub_offset()));
  __ ldr(R1, FieldAddress(CODE_REG, Code::entry_point_offset(
                                        Code::EntryKind::kMonomorphic)));
  __ br(R1);
}

// Called from switchable IC calls.
//  R0: receiver
//  R5: SingleTargetCache
// Passed to target:
//  CODE_REG: target Code object
void StubCode::GenerateSingleTargetCallStub(Assembler* assembler) {
  Label miss;
  __ LoadClassIdMayBeSmi(R1, R0);
  __ ldr(R2, FieldAddress(R5, SingleTargetCache::lower_limit_offset()),
         kUnsignedHalfword);
  __ ldr(R3, FieldAddress(R5, SingleTargetCache::upper_limit_offset()),
         kUnsignedHalfword);

  __ cmp(R1, Operand(R2));
  __ b(&miss, LT);
  __ cmp(R1, Operand(R3));
  __ b(&miss, GT);

  __ ldr(R1, FieldAddress(R5, SingleTargetCache::entry_point_offset()));
  __ ldr(CODE_REG, FieldAddress(R5, SingleTargetCache::target_offset()));
  __ br(R1);

  __ Bind(&miss);
  __ EnterStubFrame();
  __ Push(R0);  // Preserve receiver.

  __ Push(ZR);  // Result slot.
  __ Push(R0);  // Arg0: Receiver
  __ CallRuntime(kSingleTargetMissRuntimeEntry, 1);
  __ Drop(1);
  __ Pop(R5);  // result = IC

  __ Pop(R0);  // Restore receiver.
  __ LeaveStubFrame();

  __ ldr(CODE_REG, Address(THR, Thread::ic_lookup_through_code_stub_offset()));
  __ ldr(R1, FieldAddress(CODE_REG, Code::entry_point_offset(
                                        Code::EntryKind::kMonomorphic)));
  __ br(R1);
}

// Called from the monomorphic checked entry.
//  R0: receiver
void StubCode::GenerateMonomorphicMissStub(Assembler* assembler) {
  __ ldr(CODE_REG, Address(THR, Thread::monomorphic_miss_stub_offset()));
  __ EnterStubFrame();
  __ Push(R0);  // Preserve receiver.

  __ Push(ZR);  // Result slot.
  __ Push(R0);  // Arg0: Receiver
  __ CallRuntime(kMonomorphicMissRuntimeEntry, 1);
  __ Drop(1);
  __ Pop(R5);  // result = IC

  __ Pop(R0);  // Restore receiver.
  __ LeaveStubFrame();

  __ ldr(CODE_REG, Address(THR, Thread::ic_lookup_through_code_stub_offset()));
  __ ldr(R1, FieldAddress(CODE_REG, Code::entry_point_offset(
                                        Code::EntryKind::kMonomorphic)));
  __ br(R1);
}

void StubCode::GenerateFrameAwaitingMaterializationStub(Assembler* assembler) {
  __ brk(0);
}

void StubCode::GenerateAsynchronousGapMarkerStub(Assembler* assembler) {
  __ brk(0);
}

}  // namespace dart

#endif  // defined(TARGET_ARCH_ARM64) && !defined(DART_PRECOMPILED_RUNTIME)
