// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"  // Needed here to get TARGET_ARCH_IA32.
#if defined(TARGET_ARCH_IA32)

#include "vm/intermediate_language.h"

#include "lib/error.h"
#include "vm/dart_entry.h"
#include "vm/flow_graph_compiler.h"
#include "vm/locations.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"

#define __ compiler->assembler()->

namespace dart {

DECLARE_FLAG(int, optimization_counter_threshold);
DECLARE_FLAG(bool, propagate_ic_data);

// Generic summary for call instructions that have all arguments pushed
// on the stack and return the result in a fixed register EAX.
LocationSummary* Instruction::MakeCallSummary() {
  LocationSummary* result = new LocationSummary(0, 0, LocationSummary::kCall);
  result->set_out(Location::RegisterLocation(EAX));
  return result;
}


LocationSummary* PushArgumentInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps= 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::AnyOrConstant(value()));
  return locs;
}


void PushArgumentInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // In SSA mode, we need an explicit push. Nothing to do in non-SSA mode
  // where PushArgument is handled by BindInstr::EmitNativeCode.
  if (compiler->is_optimizing()) {
    Location value = locs()->in(0);
    if (value.IsRegister()) {
      __ pushl(value.reg());
    } else if (value.IsConstant()) {
      __ PushObject(value.constant());
    } else {
      ASSERT(value.IsStackSlot());
      __ pushl(value.ToStackSlotAddress());
    }
  }
}


LocationSummary* ReturnInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(EAX));
  return locs;
}


// Attempt optimized compilation at return instruction instead of at the entry.
// The entry needs to be patchable, no inlined objects are allowed in the area
// that will be overwritten by the patch instruction: a jump).
void ReturnInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->in(0).reg();
  ASSERT(result == EAX);
#if defined(DEBUG)
  // TODO(srdjan): Fix for functions with finally clause.
  // A finally clause may leave a previously pushed return value if it
  // has its own return instruction. Method that have finally are currently
  // not optimized.
  if (!compiler->HasFinally()) {
    __ Comment("Stack Check");
    Label done;
    __ movl(EDI, EBP);
    __ subl(EDI, ESP);
    // + 1 for Pc marker.
    __ cmpl(EDI, Immediate((compiler->StackSize() + 1) * kWordSize));
    __ j(EQUAL, &done, Assembler::kNearJump);
    __ int3();
    __ Bind(&done);
  }
#endif
  __ LeaveFrame();
  __ ret();

  // Generate 1 byte NOP so that the debugger can patch the
  // return pattern with a call to the debug stub.
  __ nop(1);
  compiler->AddCurrentDescriptor(PcDescriptors::kReturn,
                                 Isolate::kNoDeoptId,
                                 token_pos());
}


LocationSummary* ClosureCallInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 1;
  LocationSummary* result =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  result->set_out(Location::RegisterLocation(EAX));
  result->set_temp(0, Location::RegisterLocation(EDX));  // Arg. descriptor.
  return result;
}


LocationSummary* LoadLocalInstr::MakeLocationSummary() const {
  return LocationSummary::Make(0,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void LoadLocalInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out().reg();
  __ movl(result, Address(EBP, local().index() * kWordSize));
}


LocationSummary* StoreLocalInstr::MakeLocationSummary() const {
  return LocationSummary::Make(1,
                               Location::SameAsFirstInput(),
                               LocationSummary::kNoCall);
}


void StoreLocalInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out().reg();
  ASSERT(result == value);  // Assert that register assignment is correct.
  __ movl(Address(EBP, local().index() * kWordSize), value);
}


LocationSummary* ConstantInstr::MakeLocationSummary() const {
  return LocationSummary::Make(0,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void ConstantInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // The register allocator drops constant definitions that have no uses.
  if (!locs()->out().IsInvalid()) {
    Register result = locs()->out().reg();
    __ LoadObject(result, value());
  }
}


LocationSummary* AssertAssignableInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 3;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  summary->set_in(0, Location::RegisterLocation(EAX));  // Value.
  summary->set_in(1, Location::RegisterLocation(ECX));  // Instantiator.
  summary->set_in(2, Location::RegisterLocation(EDX));  // Type arguments.
  summary->set_out(Location::RegisterLocation(EAX));
  return summary;
}


LocationSummary* AssertBooleanInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(EAX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


static void EmitAssertBoolean(Register reg,
                              intptr_t token_pos,
                              LocationSummary* locs,
                              FlowGraphCompiler* compiler) {
  // Check that the type of the value is allowed in conditional context.
  // Call the runtime if the object is not bool::true or bool::false.
  ASSERT(locs->always_calls());
  Label done;
  __ CompareObject(reg, Bool::True());
  __ j(EQUAL, &done, Assembler::kNearJump);
  __ CompareObject(reg, Bool::False());
  __ j(EQUAL, &done, Assembler::kNearJump);

  __ pushl(reg);  // Push the source object.
  compiler->GenerateCallRuntime(token_pos,
                                kConditionTypeErrorRuntimeEntry,
                                locs);
  // We should never return here.
  __ int3();
  __ Bind(&done);
}


void AssertBooleanInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register obj = locs()->in(0).reg();
  Register result = locs()->out().reg();

  if (!is_eliminated()) {
    EmitAssertBoolean(obj, token_pos(), locs(), compiler);
  }
  ASSERT(obj == result);
}


LocationSummary* ArgumentDefinitionTestInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(EAX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void ArgumentDefinitionTestInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register saved_args_desc = locs()->in(0).reg();
  Register result = locs()->out().reg();

  // Push the result place holder initialized to NULL.
  __ PushObject(Object::ZoneHandle());
  __ pushl(Immediate(Smi::RawValue(formal_parameter_index())));
  __ PushObject(formal_parameter_name());
  __ pushl(saved_args_desc);
  compiler->GenerateCallRuntime(token_pos(),
                                kArgumentDefinitionTestRuntimeEntry,
                                locs());
  __ Drop(3);
  __ popl(result);  // Pop bool result.
}


static Condition TokenKindToSmiCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ: return EQUAL;
    case Token::kNE: return NOT_EQUAL;
    case Token::kLT: return LESS;
    case Token::kGT: return GREATER;
    case Token::kLTE: return LESS_EQUAL;
    case Token::kGTE: return  GREATER_EQUAL;
    default:
      UNREACHABLE();
      return OVERFLOW;
  }
}


LocationSummary* EqualityCompareInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const bool is_checked_strict_equal =
      HasICData() && ic_data()->AllTargetsHaveSameOwner(kInstanceCid);
  if (receiver_class_id() == kMintCid) {
    const intptr_t kNumTemps = 1;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    locs->set_in(0, Location::RequiresFpuRegister());
    locs->set_in(1, Location::RequiresFpuRegister());
    locs->set_temp(0, Location::RequiresRegister());
    locs->set_out(Location::RequiresRegister());
    return locs;
  }
  if (receiver_class_id() == kDoubleCid) {
    const intptr_t kNumTemps = 0;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    locs->set_in(0, Location::RequiresFpuRegister());
    locs->set_in(1, Location::RequiresFpuRegister());
    locs->set_out(Location::RequiresRegister());
    return locs;
  }
  if (receiver_class_id() == kSmiCid) {
    const intptr_t kNumTemps = 0;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    locs->set_in(0, Location::RegisterOrConstant(left()));
    // Only one input can be a constant operand. The case of two constant
    // operands should be handled by constant propagation.
    locs->set_in(1, locs->in(0).IsConstant()
                        ? Location::RequiresRegister()
                        : Location::RegisterOrConstant(right()));
    locs->set_out(Location::RequiresRegister());
    return locs;
  }
  if (is_checked_strict_equal) {
    const intptr_t kNumTemps = 1;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    locs->set_in(0, Location::RequiresRegister());
    locs->set_in(1, Location::RequiresRegister());
    locs->set_temp(0, Location::RequiresRegister());
    locs->set_out(Location::RequiresRegister());
    return locs;
  }
  if (IsPolymorphic()) {
    const intptr_t kNumTemps = 1;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
    locs->set_in(0, Location::RegisterLocation(ECX));
    locs->set_in(1, Location::RegisterLocation(EDX));
    locs->set_temp(0, Location::RegisterLocation(EBX));
    locs->set_out(Location::RegisterLocation(EAX));
    return locs;
  }
  const intptr_t kNumTemps = 1;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(EBX));
  locs->set_in(1, Location::RegisterLocation(EDX));
  locs->set_temp(0, Location::RegisterLocation(ECX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


static void EmitEqualityAsInstanceCall(FlowGraphCompiler* compiler,
                                       intptr_t deopt_id,
                                       intptr_t token_pos,
                                       Token::Kind kind,
                                       LocationSummary* locs,
                                       const ICData& original_ic_data) {
  if (!compiler->is_optimizing()) {
    compiler->AddCurrentDescriptor(PcDescriptors::kDeoptBefore,
                                   deopt_id,
                                   token_pos);
  }
  const int kNumberOfArguments = 2;
  const Array& kNoArgumentNames = Array::Handle();
  const int kNumArgumentsChecked = 2;

  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  Label check_identity;
  __ cmpl(Address(ESP, 0 * kWordSize), raw_null);
  __ j(EQUAL, &check_identity);
  __ cmpl(Address(ESP, 1 * kWordSize), raw_null);
  __ j(EQUAL, &check_identity);

  ICData& equality_ic_data = ICData::ZoneHandle();
  if (compiler->is_optimizing() && FLAG_propagate_ic_data) {
    ASSERT(!original_ic_data.IsNull());
    if (original_ic_data.NumberOfChecks() == 0) {
      // IC call for reoptimization populates original ICData.
      equality_ic_data = original_ic_data.raw();
    } else {
      // Megamorphic call.
      equality_ic_data = original_ic_data.AsUnaryClassChecks();
    }
  } else {
    equality_ic_data = ICData::New(compiler->parsed_function().function(),
                                   Symbols::EqualOperator(),
                                   deopt_id,
                                   kNumArgumentsChecked);
  }
  compiler->GenerateInstanceCall(deopt_id,
                                 token_pos,
                                 kNumberOfArguments,
                                 kNoArgumentNames,
                                 locs,
                                 equality_ic_data);
  Label check_ne;
  __ jmp(&check_ne);

  __ Bind(&check_identity);
  Label equality_done;
  if (compiler->is_optimizing()) {
    // No need to update IC data.
    Label is_true;
    __ popl(EAX);
    __ popl(EDX);
    __ cmpl(EAX, EDX);
    __ j(EQUAL, &is_true);
    __ LoadObject(EAX, (kind == Token::kEQ) ? Bool::False() : Bool::True());
    __ jmp(&equality_done);
    __ Bind(&is_true);
    __ LoadObject(EAX, (kind == Token::kEQ) ? Bool::True() : Bool::False());
    if (kind == Token::kNE) {
      // Skip not-equal result conversion.
      __ jmp(&equality_done);
    }
  } else {
    // Call stub, load IC data in register. The stub will update ICData if
    // necessary.
    Register ic_data_reg = locs->temp(0).reg();
    ASSERT(ic_data_reg == ECX);  // Stub depends on it.
    __ LoadObject(ic_data_reg, equality_ic_data);
    compiler->GenerateCall(token_pos,
                           &StubCode::EqualityWithNullArgLabel(),
                           PcDescriptors::kOther,
                           locs);
    __ Drop(2);
  }
  __ Bind(&check_ne);
  if (kind == Token::kNE) {
    Label false_label, true_label, done;
    // Negate the condition: true label returns false and vice versa.
    __ CompareObject(EAX, Bool::True());
    __ j(EQUAL, &true_label, Assembler::kNearJump);
    __ Bind(&false_label);
    __ LoadObject(EAX, Bool::True());
    __ jmp(&done, Assembler::kNearJump);
    __ Bind(&true_label);
    __ LoadObject(EAX, Bool::False());
    __ Bind(&done);
  }
  __ Bind(&equality_done);
}


static void EmitEqualityAsPolymorphicCall(FlowGraphCompiler* compiler,
                                          const ICData& orig_ic_data,
                                          LocationSummary* locs,
                                          BranchInstr* branch,
                                          Token::Kind kind,
                                          intptr_t deopt_id,
                                          intptr_t token_pos) {
  ASSERT((kind == Token::kEQ) || (kind == Token::kNE));
  const ICData& ic_data = ICData::Handle(orig_ic_data.AsUnaryClassChecks());
  ASSERT(ic_data.NumberOfChecks() > 0);
  ASSERT(ic_data.num_args_tested() == 1);
  Label* deopt = compiler->AddDeoptStub(deopt_id, kDeoptEquality);
  Register left = locs->in(0).reg();
  Register right = locs->in(1).reg();
  __ testl(left, Immediate(kSmiTagMask));
  Register temp = locs->temp(0).reg();
  if (ic_data.GetReceiverClassIdAt(0) == kSmiCid) {
    Label done, load_class_id;
    __ j(NOT_ZERO, &load_class_id, Assembler::kNearJump);
    __ movl(temp, Immediate(kSmiCid));
    __ jmp(&done, Assembler::kNearJump);
    __ Bind(&load_class_id);
    __ LoadClassId(temp, left);
    __ Bind(&done);
  } else {
    __ j(ZERO, deopt);  // Smi deopts.
    __ LoadClassId(temp, left);
  }
  // 'temp' contains class-id of the left argument.
  ObjectStore* object_store = Isolate::Current()->object_store();
  Condition cond = TokenKindToSmiCondition(kind);
  Label done;
  const intptr_t len = ic_data.NumberOfChecks();
  for (intptr_t i = 0; i < len; i++) {
    // Assert that the Smi is at position 0, if at all.
    ASSERT((ic_data.GetReceiverClassIdAt(i) != kSmiCid) || (i == 0));
    Label next_test;
    __ cmpl(temp, Immediate(ic_data.GetReceiverClassIdAt(i)));
    __ j(NOT_EQUAL, &next_test);
    const Function& target = Function::ZoneHandle(ic_data.GetTargetAt(i));
    if (target.Owner() == object_store->object_class()) {
      // Object.== is same as ===.
      __ Drop(2);
      __ cmpl(left, right);
      if (branch != NULL) {
        branch->EmitBranchOnCondition(compiler, cond);
      } else {
        Register result = locs->out().reg();
        Label load_true;
        __ j(cond, &load_true, Assembler::kNearJump);
        __ LoadObject(result, Bool::False());
        __ jmp(&done);
        __ Bind(&load_true);
        __ LoadObject(result, Bool::True());
      }
    } else {
      const int kNumberOfArguments = 2;
      const Array& kNoArgumentNames = Array::Handle();
      compiler->GenerateStaticCall(deopt_id,
                                   token_pos,
                                   target,
                                   kNumberOfArguments,
                                   kNoArgumentNames,
                                   locs);
      if (branch == NULL) {
        if (kind == Token::kNE) {
          Label false_label;
          __ CompareObject(EAX, Bool::True());
          __ j(EQUAL, &false_label, Assembler::kNearJump);
          __ LoadObject(EAX, Bool::True());
          __ jmp(&done);
          __ Bind(&false_label);
          __ LoadObject(EAX, Bool::False());
          __ jmp(&done);
        }
      } else {
        if (branch->is_checked()) {
          EmitAssertBoolean(EAX, token_pos, locs, compiler);
        }
        __ CompareObject(EAX, Bool::True());
        branch->EmitBranchOnCondition(compiler, cond);
      }
    }
    __ jmp(&done);
    __ Bind(&next_test);
  }
  // Fall through leads to deoptimization
  __ jmp(deopt);
  __ Bind(&done);
}


// Emit code when ICData's targets are all Object == (which is ===).
static void EmitCheckedStrictEqual(FlowGraphCompiler* compiler,
                                   const ICData& ic_data,
                                   const LocationSummary& locs,
                                   Token::Kind kind,
                                   BranchInstr* branch,
                                   intptr_t deopt_id) {
  ASSERT((kind == Token::kEQ) || (kind == Token::kNE));
  Register left = locs.in(0).reg();
  Register right = locs.in(1).reg();
  Register temp = locs.temp(0).reg();
  Label* deopt = compiler->AddDeoptStub(deopt_id, kDeoptEquality);
  __ testl(left, Immediate(kSmiTagMask));
  __ j(ZERO, deopt);
  // 'left' is not Smi.
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  Label identity_compare;
  __ cmpl(right, raw_null);
  __ j(EQUAL, &identity_compare);
  __ cmpl(left, raw_null);
  __ j(EQUAL, &identity_compare);

  __ LoadClassId(temp, left);
  const intptr_t len = ic_data.NumberOfChecks();
  for (intptr_t i = 0; i < len; i++) {
    __ cmpl(temp, Immediate(ic_data.GetReceiverClassIdAt(i)));
    if (i == (len - 1)) {
      __ j(NOT_EQUAL, deopt);
    } else {
      __ j(EQUAL, &identity_compare);
    }
  }
  __ Bind(&identity_compare);
  __ cmpl(left, right);
  if (branch == NULL) {
    Label done, is_equal;
    Register result = locs.out().reg();
    __ j(EQUAL, &is_equal, Assembler::kNearJump);
    // Not equal.
    __ LoadObject(result, (kind == Token::kEQ) ? Bool::False() : Bool::True());
    __ jmp(&done, Assembler::kNearJump);
    __ Bind(&is_equal);
    __ LoadObject(result, (kind == Token::kEQ) ? Bool::True() : Bool::False());
    __ Bind(&done);
  } else {
    Condition cond = TokenKindToSmiCondition(kind);
    branch->EmitBranchOnCondition(compiler, cond);
  }
}


// First test if receiver is NULL, in which case === is applied.
// If type feedback was provided (lists of <class-id, target>), do a
// type by type check (either === or static call to the operator.
static void EmitGenericEqualityCompare(FlowGraphCompiler* compiler,
                                       LocationSummary* locs,
                                       Token::Kind kind,
                                       BranchInstr* branch,
                                       const ICData& ic_data,
                                       intptr_t deopt_id,
                                       intptr_t token_pos) {
  ASSERT((kind == Token::kEQ) || (kind == Token::kNE));
  ASSERT(!ic_data.IsNull() && (ic_data.NumberOfChecks() > 0));
  Register left = locs->in(0).reg();
  Register right = locs->in(1).reg();
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  Label done, identity_compare, non_null_compare;
  __ cmpl(right, raw_null);
  __ j(EQUAL, &identity_compare, Assembler::kNearJump);
  __ cmpl(left, raw_null);
  __ j(NOT_EQUAL, &non_null_compare, Assembler::kNearJump);
  // Comparison with NULL is "===".
  __ Bind(&identity_compare);
  __ cmpl(left, right);
  Condition cond = TokenKindToSmiCondition(kind);
  if (branch != NULL) {
    branch->EmitBranchOnCondition(compiler, cond);
  } else {
    Register result = locs->out().reg();
    Label load_true;
    __ j(cond, &load_true, Assembler::kNearJump);
    __ LoadObject(result, Bool::False());
    __ jmp(&done);
    __ Bind(&load_true);
    __ LoadObject(result, Bool::True());
  }
  __ jmp(&done);
  __ Bind(&non_null_compare);  // Receiver is not null.
  __ pushl(left);
  __ pushl(right);
  EmitEqualityAsPolymorphicCall(compiler, ic_data, locs, branch, kind,
                                deopt_id, token_pos);
  __ Bind(&done);
}


static void EmitSmiComparisonOp(FlowGraphCompiler* compiler,
                                const LocationSummary& locs,
                                Token::Kind kind,
                                BranchInstr* branch) {
  Location left = locs.in(0);
  Location right = locs.in(1);
  ASSERT(!left.IsConstant() || !right.IsConstant());

  Condition true_condition = TokenKindToSmiCondition(kind);

  if (left.IsConstant()) {
    __ CompareObject(right.reg(), left.constant());
    true_condition = FlowGraphCompiler::FlipCondition(true_condition);
  } else if (right.IsConstant()) {
    __ CompareObject(left.reg(), right.constant());
  } else {
    __ cmpl(left.reg(), right.reg());
  }

  if (branch != NULL) {
    branch->EmitBranchOnCondition(compiler, true_condition);
  } else {
    Register result = locs.out().reg();
    Label done, is_true;
    __ j(true_condition, &is_true);
    __ LoadObject(result, Bool::False());
    __ jmp(&done);
    __ Bind(&is_true);
    __ LoadObject(result, Bool::True());
    __ Bind(&done);
  }
}


static Condition TokenKindToMintCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ: return EQUAL;
    case Token::kNE: return NOT_EQUAL;
    case Token::kLT: return LESS;
    case Token::kGT: return GREATER;
    case Token::kLTE: return LESS_EQUAL;
    case Token::kGTE: return GREATER_EQUAL;
    default:
      UNREACHABLE();
      return OVERFLOW;
  }
}


static void EmitUnboxedMintEqualityOp(FlowGraphCompiler* compiler,
                                      const LocationSummary& locs,
                                      Token::Kind kind,
                                      BranchInstr* branch) {
  ASSERT(Token::IsEqualityOperator(kind));
  XmmRegister left = locs.in(0).fpu_reg();
  XmmRegister right = locs.in(1).fpu_reg();
  Register temp = locs.temp(0).reg();
  __ movaps(XMM0, left);
  __ pcmpeqq(XMM0, right);
  __ movd(temp, XMM0);

  Condition true_condition = TokenKindToMintCondition(kind);
  __ cmpl(temp, Immediate(-1));

  if (branch != NULL) {
    branch->EmitBranchOnCondition(compiler, true_condition);
  } else {
    Register result = locs.out().reg();
    Label done, is_true;
    __ j(true_condition, &is_true);
    __ LoadObject(result, Bool::False());
    __ jmp(&done);
    __ Bind(&is_true);
    __ LoadObject(result, Bool::True());
    __ Bind(&done);
  }
}


static void EmitUnboxedMintComparisonOp(FlowGraphCompiler* compiler,
                                        const LocationSummary& locs,
                                        Token::Kind kind,
                                        BranchInstr* branch) {
  XmmRegister left = locs.in(0).fpu_reg();
  XmmRegister right = locs.in(1).fpu_reg();
  Register left_tmp = locs.temp(0).reg();
  Register right_tmp = locs.temp(1).reg();
  Register result = branch == NULL ? locs.out().reg() : kNoRegister;

  Condition hi_cond = OVERFLOW, lo_cond = OVERFLOW;
  switch (kind) {
    case Token::kLT:
      hi_cond = LESS;
      lo_cond = BELOW;
      break;
    case Token::kGT:
      hi_cond = GREATER;
      lo_cond = ABOVE;
      break;
    case Token::kLTE:
      hi_cond = LESS;
      lo_cond = BELOW_EQUAL;
      break;
    case Token::kGTE:
      hi_cond = GREATER;
      lo_cond = ABOVE_EQUAL;
      break;
    default:
      break;
  }
  ASSERT(hi_cond != OVERFLOW && lo_cond != OVERFLOW);
  Label is_true, is_false;
  // Compare upper halves first.
  __ pextrd(left_tmp, left, Immediate(1));
  __ pextrd(right_tmp, right, Immediate(1));
  __ cmpl(left_tmp, right_tmp);
  if (branch != NULL) {
    __ j(hi_cond, compiler->GetBlockLabel(branch->true_successor()));
    __ j(FlowGraphCompiler::FlipCondition(hi_cond),
         compiler->GetBlockLabel(branch->false_successor()));
  } else {
    __ j(hi_cond, &is_true);
    __ j(FlowGraphCompiler::FlipCondition(hi_cond), &is_false);
  }

  // If upper is equal, compare lower half.
  __ pextrd(left_tmp, left, Immediate(0));
  __ pextrd(right_tmp, right, Immediate(0));
  __ cmpl(left_tmp, right_tmp);
  if (branch != NULL) {
    branch->EmitBranchOnCondition(compiler, lo_cond);
  } else {
    Label done;
    __ j(lo_cond, &is_true);
    __ Bind(&is_false);
    __ LoadObject(result, Bool::False());
    __ jmp(&done);
    __ Bind(&is_true);
    __ LoadObject(result, Bool::True());
    __ Bind(&done);
  }
}


static Condition TokenKindToDoubleCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ: return EQUAL;
    case Token::kNE: return NOT_EQUAL;
    case Token::kLT: return BELOW;
    case Token::kGT: return ABOVE;
    case Token::kLTE: return BELOW_EQUAL;
    case Token::kGTE: return ABOVE_EQUAL;
    default:
      UNREACHABLE();
      return OVERFLOW;
  }
}


static void EmitDoubleComparisonOp(FlowGraphCompiler* compiler,
                                   const LocationSummary& locs,
                                   Token::Kind kind,
                                   BranchInstr* branch) {
  XmmRegister left = locs.in(0).fpu_reg();
  XmmRegister right = locs.in(1).fpu_reg();

  Condition true_condition = TokenKindToDoubleCondition(kind);
  if (branch != NULL) {
    compiler->EmitDoubleCompareBranch(
        true_condition, left, right, branch);
  } else {
    compiler->EmitDoubleCompareBool(
        true_condition, left, right, locs.out().reg());
  }
}


void EqualityCompareInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT((kind() == Token::kNE) || (kind() == Token::kEQ));
  BranchInstr* kNoBranch = NULL;
  if (receiver_class_id() == kSmiCid) {
    // Deoptimizes if both arguments not Smi.
    EmitSmiComparisonOp(compiler, *locs(), kind(), kNoBranch);
    return;
  }
  if (receiver_class_id() == kMintCid) {
    EmitUnboxedMintEqualityOp(compiler, *locs(), kind(), kNoBranch);
    return;
  }
  if (receiver_class_id() == kDoubleCid) {
    EmitDoubleComparisonOp(compiler, *locs(), kind(), kNoBranch);
    return;
  }
  const bool is_checked_strict_equal =
      HasICData() && ic_data()->AllTargetsHaveSameOwner(kInstanceCid);
  if (is_checked_strict_equal) {
    EmitCheckedStrictEqual(compiler, *ic_data(), *locs(), kind(), kNoBranch,
                           deopt_id());
    return;
  }
  if (IsPolymorphic()) {
    EmitGenericEqualityCompare(compiler, locs(), kind(), kNoBranch, *ic_data(),
                               deopt_id(), token_pos());
    return;
  }
  Register left = locs()->in(0).reg();
  Register right = locs()->in(1).reg();
  __ pushl(left);
  __ pushl(right);
  EmitEqualityAsInstanceCall(compiler,
                             deopt_id(),
                             token_pos(),
                             kind(),
                             locs(),
                             *ic_data());
  ASSERT(locs()->out().reg() == EAX);
}


void EqualityCompareInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                          BranchInstr* branch) {
  ASSERT((kind() == Token::kNE) || (kind() == Token::kEQ));
  if (receiver_class_id() == kSmiCid) {
    // Deoptimizes if both arguments not Smi.
    EmitSmiComparisonOp(compiler, *locs(), kind(), branch);
    return;
  }
  if (receiver_class_id() == kMintCid) {
    EmitUnboxedMintEqualityOp(compiler, *locs(), kind(), branch);
    return;
  }
  if (receiver_class_id() == kDoubleCid) {
    EmitDoubleComparisonOp(compiler, *locs(), kind(), branch);
    return;
  }
  const bool is_checked_strict_equal =
      HasICData() && ic_data()->AllTargetsHaveSameOwner(kInstanceCid);
  if (is_checked_strict_equal) {
    EmitCheckedStrictEqual(compiler, *ic_data(), *locs(), kind(), branch,
                           deopt_id());
    return;
  }
  if (IsPolymorphic()) {
    EmitGenericEqualityCompare(compiler, locs(), kind(), branch, *ic_data(),
                               deopt_id(), token_pos());
    return;
  }
  Register left = locs()->in(0).reg();
  Register right = locs()->in(1).reg();
  __ pushl(left);
  __ pushl(right);
  EmitEqualityAsInstanceCall(compiler,
                             deopt_id(),
                             token_pos(),
                             Token::kEQ,  // kNE reverse occurs at branch.
                             locs(),
                             *ic_data());
  if (branch->is_checked()) {
    EmitAssertBoolean(EAX, token_pos(), locs(), compiler);
  }
  Condition branch_condition = (kind() == Token::kNE) ? NOT_EQUAL : EQUAL;
  __ CompareObject(EAX, Bool::True());
  branch->EmitBranchOnCondition(compiler, branch_condition);
}


LocationSummary* RelationalOpInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  if (operands_class_id() == kMintCid) {
    const intptr_t kNumTemps = 2;
    LocationSummary* locs =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    locs->set_in(0, Location::RequiresFpuRegister());
    locs->set_in(1, Location::RequiresFpuRegister());
    locs->set_temp(0, Location::RequiresRegister());
    locs->set_temp(1, Location::RequiresRegister());
    locs->set_out(Location::RequiresRegister());
    return locs;
  }
  if (operands_class_id() == kDoubleCid) {
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RequiresFpuRegister());
    summary->set_in(1, Location::RequiresFpuRegister());
    summary->set_out(Location::RequiresRegister());
    return summary;
  } else if (operands_class_id() == kSmiCid) {
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RegisterOrConstant(left()));
    // Only one input can be a constant operand. The case of two constant
    // operands should be handled by constant propagation.
    summary->set_in(1, summary->in(0).IsConstant()
                           ? Location::RequiresRegister()
                           : Location::RegisterOrConstant(right()));
    summary->set_out(Location::RequiresRegister());
    return summary;
  }
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  // Pick arbitrary fixed input registers because this is a call.
  locs->set_in(0, Location::RegisterLocation(EAX));
  locs->set_in(1, Location::RegisterLocation(ECX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void RelationalOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  if (operands_class_id() == kSmiCid) {
    EmitSmiComparisonOp(compiler, *locs(), kind(), NULL);
    return;
  }
  if (operands_class_id() == kMintCid) {
    EmitUnboxedMintComparisonOp(compiler, *locs(), kind(), NULL);
    return;
  }
  if (operands_class_id() == kDoubleCid) {
    EmitDoubleComparisonOp(compiler, *locs(), kind(), NULL);
    return;
  }

  // Push arguments for the call.
  // TODO(fschneider): Split this instruction into different types to avoid
  // explicitly pushing arguments to the call here.
  Register left = locs()->in(0).reg();
  Register right = locs()->in(1).reg();
  __ pushl(left);
  __ pushl(right);
  if (HasICData() && (ic_data()->NumberOfChecks() > 0)) {
    Label* deopt = compiler->AddDeoptStub(deopt_id(), kDeoptRelationalOp);
    // Load class into EDI. Since this is a call, any register except
    // the fixed input registers would be ok.
    ASSERT((left != EDI) && (right != EDI));
    Label done;
    const intptr_t kNumArguments = 2;
    __ movl(EDI, Immediate(kSmiCid));
    __ testl(left, Immediate(kSmiTagMask));
    __ j(ZERO, &done);
    __ LoadClassId(EDI, left);
    __ Bind(&done);
    compiler->EmitTestAndCall(ICData::Handle(ic_data()->AsUnaryClassChecks()),
                              EDI,  // Class id register.
                              kNumArguments,
                              Array::Handle(),  // No named arguments.
                              deopt,  // Deoptimize target.
                              deopt_id(),
                              token_pos(),
                              locs());
    return;
  }
  const String& function_name =
      String::ZoneHandle(Symbols::New(Token::Str(kind())));
  if (!compiler->is_optimizing()) {
    compiler->AddCurrentDescriptor(PcDescriptors::kDeoptBefore,
                                   deopt_id(),
                                   token_pos());
  }
  const intptr_t kNumArguments = 2;
  const intptr_t kNumArgsChecked = 2;  // Type-feedback.
  ICData& relational_ic_data = ICData::ZoneHandle(ic_data()->raw());
  if (compiler->is_optimizing() && FLAG_propagate_ic_data) {
    ASSERT(!ic_data()->IsNull());
    if (ic_data()->NumberOfChecks() == 0) {
      // IC call for reoptimization populates original ICData.
      relational_ic_data = ic_data()->raw();
    } else {
      // Megamorphic call.
      relational_ic_data = ic_data()->AsUnaryClassChecks();
    }
  } else {
    relational_ic_data = ICData::New(compiler->parsed_function().function(),
                                     function_name,
                                     deopt_id(),
                                     kNumArgsChecked);
  }
  compiler->GenerateInstanceCall(deopt_id(),
                                 token_pos(),
                                 kNumArguments,
                                 Array::ZoneHandle(),  // No optional arguments.
                                 locs(),
                                 relational_ic_data);
}


void RelationalOpInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                       BranchInstr* branch) {
  if (operands_class_id() == kSmiCid) {
    EmitSmiComparisonOp(compiler, *locs(), kind(), branch);
    return;
  }
  if (operands_class_id() == kMintCid) {
    EmitUnboxedMintComparisonOp(compiler, *locs(), kind(), branch);
    return;
  }
  if (operands_class_id() == kDoubleCid) {
    EmitDoubleComparisonOp(compiler, *locs(), kind(), branch);
    return;
  }
  EmitNativeCode(compiler);
  __ CompareObject(EAX, Bool::True());
  branch->EmitBranchOnCondition(compiler, EQUAL);
}


LocationSummary* NativeCallInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 3;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_temp(0, Location::RegisterLocation(EAX));
  locs->set_temp(1, Location::RegisterLocation(ECX));
  locs->set_temp(2, Location::RegisterLocation(EDX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void NativeCallInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->temp(0).reg() == EAX);
  ASSERT(locs()->temp(1).reg() == ECX);
  ASSERT(locs()->temp(2).reg() == EDX);
  Register result = locs()->out().reg();

  // Push the result place holder initialized to NULL.
  __ PushObject(Object::ZoneHandle());
  // Pass a pointer to the first argument in EAX.
  if (!function().HasOptionalParameters()) {
    __ leal(EAX, Address(EBP, (1 + function().NumParameters()) * kWordSize));
  } else {
    __ leal(EAX,
            Address(EBP, ParsedFunction::kFirstLocalSlotIndex * kWordSize));
  }
  __ movl(ECX, Immediate(reinterpret_cast<uword>(native_c_function())));
  __ movl(EDX, Immediate(NativeArguments::ComputeArgcTag(function())));
  compiler->GenerateCall(token_pos(),
                         &StubCode::CallNativeCFunctionLabel(),
                         PcDescriptors::kOther,
                         locs());
  __ popl(result);
}


static bool CanBeImmediateIndex(Value* index, intptr_t cid) {
  if (!index->definition()->IsConstant()) return false;
  const Object& constant = index->definition()->AsConstant()->value();
  if (!constant.IsSmi()) return false;
  const Smi& smi_const = Smi::Cast(constant);
  const intptr_t scale = FlowGraphCompiler::ElementSizeFor(cid);
  const intptr_t data_offset = FlowGraphCompiler::DataOffsetFor(cid);
  const int64_t disp = smi_const.AsInt64Value() * scale + data_offset;
  return Utils::IsInt(32, disp);
}


LocationSummary* StringFromCharCodeInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  // TODO(fschneider): Allow immediate operands for the char code.
  locs->set_in(0, Location::RequiresRegister());
  locs->set_out(Location::RequiresRegister());
  return locs;
}


void StringFromCharCodeInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register char_code = locs()->in(0).reg();
  Register result = locs()->out().reg();
  __ movl(result,
          Immediate(reinterpret_cast<uword>(Symbols::PredefinedAddress())));
  __ movl(result, Address(result,
                          char_code,
                          TIMES_HALF_WORD_SIZE,  // Char code is a smi.
                          Symbols::kNullCharCodeSymbolOffset * kWordSize));
}


intptr_t LoadIndexedInstr::ResultCid() const {
  switch (class_id_) {
    case kArrayCid:
    case kImmutableArrayCid:
      return kDynamicCid;
    case kFloat32ArrayCid :
    case kFloat64ArrayCid :
      return kDoubleCid;
    case kInt8ArrayCid:
    case kUint8ArrayCid:
    case kUint8ClampedArrayCid:
    case kExternalUint8ArrayCid:
    case kInt16ArrayCid:
    case kUint16ArrayCid:
    case kOneByteStringCid:
    case kTwoByteStringCid:
      return kSmiCid;
    case kInt32ArrayCid:
    case kUint32ArrayCid:
      // Result can be smi or mint when boxed.
      return kDynamicCid;
    default:
      UNIMPLEMENTED();
      return kDynamicCid;
  }
}


Representation LoadIndexedInstr::representation() const {
  switch (class_id_) {
    case kArrayCid:
    case kImmutableArrayCid:
    case kInt8ArrayCid:
    case kUint8ArrayCid:
    case kUint8ClampedArrayCid:
    case kExternalUint8ArrayCid:
    case kInt16ArrayCid:
    case kUint16ArrayCid:
    case kOneByteStringCid:
    case kTwoByteStringCid:
      return kTagged;
    case kInt32ArrayCid:
    case kUint32ArrayCid:
      return kUnboxedMint;
    case kFloat32ArrayCid :
    case kFloat64ArrayCid :
      return kUnboxedDouble;
    default:
      UNIMPLEMENTED();
      return kTagged;
  }
}


LocationSummary* LoadIndexedInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  // The smi index is either untagged and tagged again at the end of the
  // operation (element size == 1), or it is left smi tagged (for all element
  // sizes > 1).
  locs->set_in(1, CanBeImmediateIndex(index(), class_id())
                    ? Location::RegisterOrSmiConstant(index())
                    : Location::RequiresRegister());
  if (representation() == kUnboxedDouble) {
    locs->set_out(Location::RequiresFpuRegister());
  } else {
    locs->set_out(Location::RequiresRegister());
  }
  return locs;
}


void LoadIndexedInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register array = locs()->in(0).reg();
  Location index = locs()->in(1);

  if (class_id() == kExternalUint8ArrayCid) {
    Register result = locs()->out().reg();
    const Address& element_address = index.IsRegister()
        ? FlowGraphCompiler::ExternalElementAddressForRegIndex(
            class_id(), result, index.reg())
        : FlowGraphCompiler::ExternalElementAddressForIntIndex(
            class_id(), result, Smi::Cast(index.constant()).Value());
    if (index.IsRegister()) {
      __ SmiUntag(index.reg());
    }
    __ movl(result,
            FieldAddress(array, ExternalUint8Array::external_data_offset()));
    __ movl(result,
            Address(result, ExternalByteArrayData<uint8_t>::data_offset()));
    __ movzxb(result, element_address);
    __ SmiTag(result);
    if (index.IsRegister()) {
      __ SmiTag(index.reg());  // Re-tag.
    }
    return;
  }

  FieldAddress element_address = index.IsRegister()
      ? FlowGraphCompiler::ElementAddressForRegIndex(
          class_id(), array, index.reg())
      : FlowGraphCompiler::ElementAddressForIntIndex(
          class_id(), array, Smi::Cast(index.constant()).Value());

  if ((representation() == kUnboxedDouble) ||
      (representation() == kUnboxedMint)) {
    XmmRegister result = locs()->out().fpu_reg();
    switch (class_id()) {
      case kInt32ArrayCid:
        __ movss(result, element_address);
        __ pmovsxdq(result, result);
        break;
      case kUint32ArrayCid:
        __ xorpd(result, result);
        __ movss(result, element_address);
        break;
      case kFloat32ArrayCid:
        // Load single precision float and promote to double.
        __ movss(result, element_address);
        __ cvtss2sd(result, locs()->out().fpu_reg());
        break;
      case kFloat64ArrayCid:
        __ movsd(result, element_address);
        break;
    }
    return;
  }

  Register result = locs()->out().reg();
  switch (class_id()) {
    case kInt8ArrayCid:
    case kUint8ArrayCid:
    case kUint8ClampedArrayCid:
    case kOneByteStringCid:
      if (index.IsRegister()) {
        __ SmiUntag(index.reg());
      }
      if (class_id() == kInt8ArrayCid) {
        __ movsxb(result, element_address);
      } else {
        __ movzxb(result, element_address);
      }
      __ SmiTag(result);
      if (index.IsRegister()) {
        __ SmiTag(index.reg());  // Re-tag.
      }
      break;
    case kInt16ArrayCid:
      __ movsxw(result, element_address);
      __ SmiTag(result);
      break;
    case kUint16ArrayCid:
    case kTwoByteStringCid:
      __ movzxw(result, element_address);
      __ SmiTag(result);
      break;
    default:
      ASSERT((class_id() == kArrayCid) || (class_id() == kImmutableArrayCid));
      __ movl(result, element_address);
      break;
  }
}


Representation StoreIndexedInstr::RequiredInputRepresentation(
    intptr_t idx) const {
  if ((idx == 0) || (idx == 1)) return kTagged;
  ASSERT(idx == 2);
  switch (class_id_) {
    case kArrayCid:
    case kInt8ArrayCid:
    case kUint8ArrayCid:
    case kUint8ClampedArrayCid:
    case kInt16ArrayCid:
    case kUint16ArrayCid:
      return kTagged;
    case kInt32ArrayCid:
    case kUint32ArrayCid:
      return kUnboxedMint;
    case kFloat32ArrayCid :
    case kFloat64ArrayCid :
      return kUnboxedDouble;
    default:
      UNIMPLEMENTED();
      return kTagged;
  }
}


LocationSummary* StoreIndexedInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 3;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  // The smi index is either untagged and tagged again at the end of the
  // operation (element size == 1), or it is left smi tagged (for all element
  // sizes > 1).
  locs->set_in(1, CanBeImmediateIndex(index(), class_id())
                    ? Location::RegisterOrSmiConstant(index())
                    : Location::RequiresRegister());
  switch (class_id()) {
    case kArrayCid:
      locs->set_in(2, ShouldEmitStoreBarrier()
                        ? Location::WritableRegister()
                        : Location::RegisterOrConstant(value()));
      break;
    case kInt8ArrayCid:
    case kUint8ArrayCid:
    case kUint8ClampedArrayCid:
      // TODO(fschneider): Add location constraint for byte registers (EAX,
      // EBX, ECX, EDX) instead of using a fixed register.
      locs->set_in(2, Location::FixedRegisterOrSmiConstant(value(), EAX));
      break;
    case kInt16ArrayCid:
    case kUint16ArrayCid:
      // Writable register because the value must be untagged before storing.
      locs->set_in(2, Location::WritableRegister());
      break;
    case kFloat32ArrayCid:
      // Need temp register for float-to-double conversion.
      locs->AddTemp(Location::RequiresFpuRegister());
      // Fall through.
    case kInt32ArrayCid:
    case kUint32ArrayCid:
    case kFloat64ArrayCid:
      // TODO(srdjan): Support Float64 constants.
      locs->set_in(2, Location::RequiresFpuRegister());
      break;
    default:
      UNREACHABLE();
      return NULL;
  }
  return locs;
}


void StoreIndexedInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register array = locs()->in(0).reg();
  Location index = locs()->in(1);

  FieldAddress element_address = index.IsRegister() ?
      FlowGraphCompiler::ElementAddressForRegIndex(
          class_id(), array, index.reg()) :
      FlowGraphCompiler::ElementAddressForIntIndex(
          class_id(), array, Smi::Cast(index.constant()).Value());

  switch (class_id()) {
    case kArrayCid:
      if (ShouldEmitStoreBarrier()) {
        Register value = locs()->in(2).reg();
        __ StoreIntoObject(array, element_address, value);
      } else if (locs()->in(2).IsConstant()) {
        const Object& constant = locs()->in(2).constant();
        __ StoreIntoObjectNoBarrier(array, element_address, constant);
      } else {
        Register value = locs()->in(2).reg();
        __ StoreIntoObjectNoBarrier(array, element_address, value);
      }
      break;
    case kInt8ArrayCid:
    case kUint8ArrayCid:
      if (index.IsRegister()) {
        __ SmiUntag(index.reg());
      }
      if (locs()->in(2).IsConstant()) {
        const Smi& constant = Smi::Cast(locs()->in(2).constant());
        __ movb(element_address,
                Immediate(static_cast<int8_t>(constant.Value())));
      } else {
        ASSERT(locs()->in(2).reg() == EAX);
        __ SmiUntag(EAX);
        __ movb(element_address, AL);
      }
      if (index.IsRegister()) {
        __ SmiTag(index.reg());  // Re-tag.
      }
      break;
    case kUint8ClampedArrayCid: {
      if (index.IsRegister()) {
        __ SmiUntag(index.reg());
      }
      if (locs()->in(2).IsConstant()) {
        const Smi& constant = Smi::Cast(locs()->in(2).constant());
        intptr_t value = constant.Value();
        // Clamp to 0x0 or 0xFF respectively.
        if (value > 0xFF) {
          value = 0xFF;
        } else if (value < 0) {
          value = 0;
        }
        __ movb(element_address,
                Immediate(static_cast<int8_t>(value)));
      } else {
        ASSERT(locs()->in(2).reg() == EAX);
        Label store_value, store_0xff;
        __ SmiUntag(EAX);
        __ cmpl(EAX, Immediate(0xFF));
        __ j(BELOW_EQUAL, &store_value, Assembler::kNearJump);
        // Clamp to 0x0 or 0xFF respectively.
        __ j(GREATER, &store_0xff);
        __ xorl(EAX, EAX);
        __ jmp(&store_value, Assembler::kNearJump);
        __ Bind(&store_0xff);
        __ movl(EAX, Immediate(0xFF));
        __ Bind(&store_value);
        __ movb(element_address, AL);
      }
      if (index.IsRegister()) {
        __ SmiTag(index.reg());  // Re-tag.
      }
      break;
    }
    case kInt16ArrayCid:
    case kUint16ArrayCid: {
      Register value = locs()->in(2).reg();
      __ SmiUntag(value);
      __ movw(element_address, value);
      break;
    }
    case kInt32ArrayCid:
    case kUint32ArrayCid:
      __ movss(element_address, locs()->in(2).fpu_reg());
      break;
    case kFloat32ArrayCid:
      // Convert to single precision.
      __ cvtsd2ss(locs()->temp(0).fpu_reg(), locs()->in(2).fpu_reg());
      // Store.
      __ movss(element_address, locs()->temp(0).fpu_reg());
      break;
    case kFloat64ArrayCid:
      __ movsd(element_address, locs()->in(2).fpu_reg());
      break;
    default:
      UNREACHABLE();
  }
}


LocationSummary* StoreInstanceFieldInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t num_temps =  0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, num_temps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_in(1, ShouldEmitStoreBarrier()
                       ? Location::WritableRegister()
                       : Location::RegisterOrConstant(value()));
  return summary;
}


void StoreInstanceFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register instance_reg = locs()->in(0).reg();
  if (ShouldEmitStoreBarrier()) {
    Register value_reg = locs()->in(1).reg();
    __ StoreIntoObject(instance_reg,
        FieldAddress(instance_reg, field().Offset()), value_reg);
  } else {
    if (locs()->in(1).IsConstant()) {
      __ StoreIntoObjectNoBarrier(
          instance_reg,
          FieldAddress(instance_reg, field().Offset()),
          locs()->in(1).constant());
    } else {
      Register value_reg = locs()->in(1).reg();
      __ StoreIntoObjectNoBarrier(instance_reg,
          FieldAddress(instance_reg, field().Offset()), value_reg);
    }
  }
}


LocationSummary* LoadStaticFieldInstr::MakeLocationSummary() const {
  return LocationSummary::Make(0,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void LoadStaticFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out().reg();
  __ LoadObject(result, field());
  __ movl(result, FieldAddress(result, Field::value_offset()));
}


LocationSummary* StoreStaticFieldInstr::MakeLocationSummary() const {
  LocationSummary* locs = new LocationSummary(1, 1, LocationSummary::kNoCall);
  locs->set_in(0, value()->NeedsStoreBuffer() ? Location::WritableRegister()
                                              : Location::RequiresRegister());
  locs->set_temp(0, Location::RequiresRegister());
  return locs;
}


void StoreStaticFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register temp = locs()->temp(0).reg();

  __ LoadObject(temp, field());
  if (this->value()->NeedsStoreBuffer()) {
    __ StoreIntoObject(temp, FieldAddress(temp, Field::value_offset()), value);
  } else {
    __ StoreIntoObjectNoBarrier(
        temp, FieldAddress(temp, Field::value_offset()), value);
  }
}


LocationSummary* InstanceOfInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 3;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  summary->set_in(0, Location::RegisterLocation(EAX));
  summary->set_in(1, Location::RegisterLocation(ECX));
  summary->set_in(2, Location::RegisterLocation(EDX));
  summary->set_out(Location::RegisterLocation(EAX));
  return summary;
}


void InstanceOfInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->in(0).reg() == EAX);  // Value.
  ASSERT(locs()->in(1).reg() == ECX);  // Instantiator.
  ASSERT(locs()->in(2).reg() == EDX);  // Instantiator type arguments.

  compiler->GenerateInstanceOf(token_pos(),
                               type(),
                               negate_result(),
                               locs());
  ASSERT(locs()->out().reg() == EAX);
}


LocationSummary* CreateArrayInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(ECX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void CreateArrayInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Allocate the array.  EDX = length, ECX = element type.
  ASSERT(locs()->in(0).reg() == ECX);
  __ movl(EDX, Immediate(Smi::RawValue(ArgumentCount())));
  compiler->GenerateCall(token_pos(),
                         &StubCode::AllocateArrayLabel(),
                         PcDescriptors::kOther,
                         locs());
  ASSERT(locs()->out().reg() == EAX);

  // Pop the element values from the stack into the array.
  __ leal(EDX, FieldAddress(EAX, Array::data_offset()));
  for (int i = ArgumentCount() - 1; i >= 0; --i) {
    __ popl(Address(EDX, i * kWordSize));
  }
}


LocationSummary*
AllocateObjectWithBoundsCheckInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(EAX));
  locs->set_in(1, Location::RegisterLocation(ECX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void AllocateObjectWithBoundsCheckInstr::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  const Class& cls = Class::ZoneHandle(constructor().Owner());
  Register type_arguments = locs()->in(0).reg();
  Register instantiator_type_arguments = locs()->in(1).reg();
  Register result = locs()->out().reg();

  // Push the result place holder initialized to NULL.
  __ PushObject(Object::ZoneHandle());
  __ PushObject(cls);
  __ pushl(type_arguments);
  __ pushl(instantiator_type_arguments);
  compiler->GenerateCallRuntime(token_pos(),
                                kAllocateObjectWithBoundsCheckRuntimeEntry,
                                locs());
  // Pop instantiator type arguments, type arguments, and class.
  // source location.
  __ Drop(3);
  __ popl(result);  // Pop new instance.
}


LocationSummary* LoadFieldInstr::MakeLocationSummary() const {
  return LocationSummary::Make(1,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void LoadFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register instance_reg = locs()->in(0).reg();
  Register result_reg = locs()->out().reg();

  __ movl(result_reg, FieldAddress(instance_reg, offset_in_bytes()));
}


LocationSummary* InstantiateTypeArgumentsInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(EAX));
  locs->set_temp(0, Location::RegisterLocation(ECX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void InstantiateTypeArgumentsInstr::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  Register instantiator_reg = locs()->in(0).reg();
  Register temp = locs()->temp(0).reg();
  Register result_reg = locs()->out().reg();

  // 'instantiator_reg' is the instantiator AbstractTypeArguments object
  // (or null).
  // If the instantiator is null and if the type argument vector
  // instantiated from null becomes a vector of dynamic, then use null as
  // the type arguments.
  Label type_arguments_instantiated;
  const intptr_t len = type_arguments().Length();
  if (type_arguments().IsRawInstantiatedRaw(len)) {
    const Immediate& raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    __ cmpl(instantiator_reg, raw_null);
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
  }
  // Instantiate non-null type arguments.
  if (type_arguments().IsUninstantiatedIdentity()) {
    // Check if the instantiator type argument vector is a TypeArguments of a
    // matching length and, if so, use it as the instantiated type_arguments.
    // No need to check the instantiator ('instantiator_reg') for null here,
    // because a null instantiator will have the wrong class (Null instead of
    // TypeArguments).
    Label type_arguments_uninstantiated;
    __ CompareClassId(instantiator_reg, kTypeArgumentsCid, temp);
    __ j(NOT_EQUAL, &type_arguments_uninstantiated, Assembler::kNearJump);
    __ cmpl(FieldAddress(instantiator_reg, TypeArguments::length_offset()),
            Immediate(Smi::RawValue(len)));
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
    __ Bind(&type_arguments_uninstantiated);
  }
  // A runtime call to instantiate the type arguments is required.
  __ PushObject(Object::ZoneHandle());  // Make room for the result.
  __ PushObject(type_arguments());
  __ pushl(instantiator_reg);  // Push instantiator type arguments.
  compiler->GenerateCallRuntime(token_pos(),
                                kInstantiateTypeArgumentsRuntimeEntry,
                                locs());
  __ Drop(2);  // Drop instantiator and uninstantiated type arguments.
  __ popl(result_reg);  // Pop instantiated type arguments.
  __ Bind(&type_arguments_instantiated);
  ASSERT(instantiator_reg == result_reg);
  // 'result_reg': Instantiated type arguments.
}


LocationSummary*
ExtractConstructorTypeArgumentsInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_out(Location::SameAsFirstInput());
  locs->set_temp(0, Location::RequiresRegister());
  return locs;
}


void ExtractConstructorTypeArgumentsInstr::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  Register instantiator_reg = locs()->in(0).reg();
  Register result_reg = locs()->out().reg();
  ASSERT(instantiator_reg == result_reg);
  Register temp_reg = locs()->temp(0).reg();

  // instantiator_reg is the instantiator type argument vector, i.e. an
  // AbstractTypeArguments object (or null).
  // If the instantiator is null and if the type argument vector
  // instantiated from null becomes a vector of dynamic, then use null as
  // the type arguments.
  Label type_arguments_instantiated;
  const intptr_t len = type_arguments().Length();
  if (type_arguments().IsRawInstantiatedRaw(len)) {
    const Immediate& raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    __ cmpl(instantiator_reg, raw_null);
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
  }
  // Instantiate non-null type arguments.
  if (type_arguments().IsUninstantiatedIdentity()) {
    // Check if the instantiator type argument vector is a TypeArguments of a
    // matching length and, if so, use it as the instantiated type_arguments.
    // No need to check instantiator_reg for null here, because a null
    // instantiator will have the wrong class (Null instead of TypeArguments).
    Label type_arguments_uninstantiated;
    __ CompareClassId(instantiator_reg, kTypeArgumentsCid, temp_reg);
    __ j(NOT_EQUAL, &type_arguments_uninstantiated, Assembler::kNearJump);
    const Immediate& arguments_length =
        Immediate(Smi::RawValue(type_arguments().Length()));
    __ cmpl(FieldAddress(instantiator_reg, TypeArguments::length_offset()),
        arguments_length);
    __ j(EQUAL, &type_arguments_instantiated, Assembler::kNearJump);
    __ Bind(&type_arguments_uninstantiated);
  }
  // In the non-factory case, we rely on the allocation stub to
  // instantiate the type arguments.
  __ LoadObject(result_reg, type_arguments());
  // result_reg: uninstantiated type arguments.
  __ Bind(&type_arguments_instantiated);
  // result_reg: uninstantiated or instantiated type arguments.
}


LocationSummary*
ExtractConstructorInstantiatorInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RequiresRegister());
  locs->set_out(Location::SameAsFirstInput());
  locs->set_temp(0, Location::RequiresRegister());
  return locs;
}


void ExtractConstructorInstantiatorInstr::EmitNativeCode(
    FlowGraphCompiler* compiler) {
  Register instantiator_reg = locs()->in(0).reg();
  ASSERT(locs()->out().reg() == instantiator_reg);
  Register temp_reg = locs()->temp(0).reg();

  // instantiator_reg is the instantiator AbstractTypeArguments object
  // (or null).  If the instantiator is null and if the type argument vector
  // instantiated from null becomes a vector of dynamic, then use null as
  // the type arguments and do not pass the instantiator.
  Label done;
  const intptr_t len = type_arguments().Length();
  if (type_arguments().IsRawInstantiatedRaw(len)) {
    const Immediate& raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    Label instantiator_not_null;
    __ cmpl(instantiator_reg, raw_null);
    __ j(NOT_EQUAL, &instantiator_not_null, Assembler::kNearJump);
    // Null was used in VisitExtractConstructorTypeArguments as the
    // instantiated type arguments, no proper instantiator needed.
    __ movl(instantiator_reg,
            Immediate(Smi::RawValue(StubCode::kNoInstantiator)));
    __ jmp(&done);
    __ Bind(&instantiator_not_null);
  }
  // Instantiate non-null type arguments.
  if (type_arguments().IsUninstantiatedIdentity()) {
    // TODO(regis): The following emitted code is duplicated in
    // VisitExtractConstructorTypeArguments above. The reason is that the code
    // is split between two computations, so that each one produces a
    // single value, rather than producing a pair of values.
    // If this becomes an issue, we should expose these tests at the IL level.

    // Check if the instantiator type argument vector is a TypeArguments of a
    // matching length and, if so, use it as the instantiated type_arguments.
    // No need to check the instantiator (RAX) for null here, because a null
    // instantiator will have the wrong class (Null instead of TypeArguments).
    __ CompareClassId(instantiator_reg, kTypeArgumentsCid, temp_reg);
    __ j(NOT_EQUAL, &done, Assembler::kNearJump);
    const Immediate& arguments_length =
        Immediate(Smi::RawValue(type_arguments().Length()));
    __ cmpl(FieldAddress(instantiator_reg, TypeArguments::length_offset()),
        arguments_length);
    __ j(NOT_EQUAL, &done, Assembler::kNearJump);
    // The instantiator was used in VisitExtractConstructorTypeArguments as the
    // instantiated type arguments, no proper instantiator needed.
    __ movl(instantiator_reg,
            Immediate(Smi::RawValue(StubCode::kNoInstantiator)));
  }
  __ Bind(&done);
  // instantiator_reg: instantiator or kNoInstantiator.
}


LocationSummary* AllocateContextInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 1;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_temp(0, Location::RegisterLocation(EDX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void AllocateContextInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(locs()->temp(0).reg() == EDX);
  ASSERT(locs()->out().reg() == EAX);

  __ movl(EDX, Immediate(num_context_variables()));
  const ExternalLabel label("alloc_context",
                            StubCode::AllocateContextEntryPoint());
  compiler->GenerateCall(token_pos(),
                         &label,
                         PcDescriptors::kOther,
                         locs());
}


LocationSummary* CloneContextInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  locs->set_in(0, Location::RegisterLocation(EAX));
  locs->set_out(Location::RegisterLocation(EAX));
  return locs;
}


void CloneContextInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register context_value = locs()->in(0).reg();
  Register result = locs()->out().reg();

  __ PushObject(Object::ZoneHandle());  // Make room for the result.
  __ pushl(context_value);
  compiler->GenerateCallRuntime(token_pos(),
                                kCloneContextRuntimeEntry,
                                locs());
  __ popl(result);  // Remove argument.
  __ popl(result);  // Get result (cloned context).
}


LocationSummary* CatchEntryInstr::MakeLocationSummary() const {
  return LocationSummary::Make(0,
                               Location::NoLocation(),
                               LocationSummary::kNoCall);
}


// Restore stack and initialize the two exception variables:
// exception and stack trace variables.
void CatchEntryInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Restore RSP from RBP as we are coming from a throw and the code for
  // popping arguments has not been run.
  const intptr_t locals_space_size = compiler->StackSize() * kWordSize;
  ASSERT(locals_space_size >= 0);
  const intptr_t offset_size =
      -locals_space_size + FlowGraphCompiler::kLocalsOffsetFromFP;
  __ leal(ESP, Address(EBP, offset_size));

  ASSERT(!exception_var().is_captured());
  ASSERT(!stacktrace_var().is_captured());
  __ movl(Address(EBP, exception_var().index() * kWordSize),
          kExceptionObjectReg);
  __ movl(Address(EBP, stacktrace_var().index() * kWordSize),
          kStackTraceObjectReg);
}


LocationSummary* CheckStackOverflowInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 0;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs,
                          kNumTemps,
                          LocationSummary::kCallOnSlowPath);
  return summary;
}


class CheckStackOverflowSlowPath : public SlowPathCode {
 public:
  explicit CheckStackOverflowSlowPath(CheckStackOverflowInstr* instruction)
      : instruction_(instruction) { }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    __ Comment("CheckStackOverflowSlowPath");
    __ Bind(entry_label());
    compiler->SaveLiveRegisters(instruction_->locs());
    compiler->GenerateCallRuntime(instruction_->token_pos(),
                                  kStackOverflowRuntimeEntry,
                                  instruction_->locs());
    compiler->RestoreLiveRegisters(instruction_->locs());
    __ jmp(exit_label());
  }

 private:
  CheckStackOverflowInstr* instruction_;
};


void CheckStackOverflowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  CheckStackOverflowSlowPath* slow_path = new CheckStackOverflowSlowPath(this);
  compiler->AddSlowPathCode(slow_path);

  __ cmpl(ESP,
          Address::Absolute(Isolate::Current()->stack_limit_address()));
  __ j(BELOW_EQUAL, slow_path->entry_label());
  __ Bind(slow_path->exit_label());
}


LocationSummary* BinarySmiOpInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  if (op_kind() == Token::kTRUNCDIV) {
    const intptr_t kNumTemps = 1;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    // Both inputs must be writable because they will be untagged.
    summary->set_in(0, Location::RegisterLocation(EAX));
    summary->set_in(1, Location::WritableRegister());
    summary->set_out(Location::SameAsFirstInput());
    // Will be used for sign extension and division.
    summary->set_temp(0, Location::RegisterLocation(EDX));
    return summary;
  } else if (op_kind() == Token::kSHR) {
    const intptr_t kNumTemps = 0;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::FixedRegisterOrSmiConstant(right(), ECX));
    summary->set_out(Location::SameAsFirstInput());
    return summary;
  } else if (op_kind() == Token::kSHL) {
    const intptr_t kNumTemps = 1;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::FixedRegisterOrSmiConstant(right(), ECX));
    summary->set_temp(0, Location::RequiresRegister());
    summary->set_out(Location::SameAsFirstInput());
    return summary;
  } else {
    const intptr_t kNumTemps = 0;
    LocationSummary* summary =
        new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
    summary->set_in(0, Location::RequiresRegister());
    summary->set_in(1, Location::RegisterOrSmiConstant(right()));
    summary->set_out(Location::SameAsFirstInput());
    return summary;
  }
}


void BinarySmiOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register left = locs()->in(0).reg();
  Register result = locs()->out().reg();
  ASSERT(left == result);
  Label* deopt = NULL;
  if (CanDeoptimize()) {
      deopt  = compiler->AddDeoptStub(deopt_id(),
                                      kDeoptBinarySmiOp);
  }

  if (locs()->in(1).IsConstant()) {
    const Object& constant = locs()->in(1).constant();
    ASSERT(constant.IsSmi());
    const int32_t imm =
        reinterpret_cast<int32_t>(constant.raw());
    switch (op_kind()) {
      case Token::kADD:
        __ addl(left, Immediate(imm));
        if (deopt != NULL) __ j(OVERFLOW, deopt);
        break;
      case Token::kSUB: {
        __ subl(left, Immediate(imm));
        if (deopt != NULL) __ j(OVERFLOW, deopt);
        break;
      }
      case Token::kMUL: {
        // Keep left value tagged and untag right value.
        const intptr_t value = Smi::Cast(constant).Value();
        __ imull(left, Immediate(value));
        if (deopt != NULL) __ j(OVERFLOW, deopt);
        break;
      }
      case Token::kBIT_AND: {
        // No overflow check.
        __ andl(left, Immediate(imm));
        break;
      }
      case Token::kBIT_OR: {
        // No overflow check.
        __ orl(left, Immediate(imm));
        break;
      }
      case Token::kBIT_XOR: {
        // No overflow check.
        __ xorl(left, Immediate(imm));
        break;
      }
      case Token::kSHR: {
        // sarl operation masks the count to 5 bits.
        const intptr_t kCountLimit = 0x1F;
        intptr_t value = Smi::Cast(constant).Value();

        if (value == 0) {
          // TODO(vegorov): should be handled outside.
          break;
        } else if (value < 0) {
          // TODO(vegorov): should be handled outside.
          __ jmp(deopt);
          break;
        }

        value = value + kSmiTagSize;
        if (value >= kCountLimit) value = kCountLimit;

        __ sarl(left, Immediate(value));
        __ SmiTag(left);
        break;
      }
      case Token::kSHL: {
        // shll operation masks the count to 5 bits.
        const intptr_t kCountLimit = 0x1F;
        intptr_t value = Smi::Cast(constant).Value();
        if (value == 0) break;
        if ((value < 0) || (value >= kCountLimit)) {
          // This condition may not be known earlier in some cases because
          // of constant propagation, inlining, etc.
          __ jmp(deopt);
          break;
        }
        Register temp = locs()->temp(0).reg();
        __ movl(temp, left);
        __ shll(left, Immediate(value));
        __ sarl(left, Immediate(value));
        __ cmpl(left, temp);
        __ j(NOT_EQUAL, deopt);  // Overflow.
        // Shift for result now we know there is no overflow.
        __ shll(left, Immediate(value));
        break;
      }

      default:
        UNREACHABLE();
        break;
    }
    return;
  }

  Register right = locs()->in(1).reg();
  switch (op_kind()) {
    case Token::kADD: {
      __ addl(left, right);
      if (deopt != NULL) __ j(OVERFLOW, deopt);
      break;
    }
    case Token::kSUB: {
      __ subl(left, right);
      if (deopt != NULL) __ j(OVERFLOW, deopt);
      break;
    }
    case Token::kMUL: {
      __ SmiUntag(left);
      __ imull(left, right);
      if (deopt != NULL) __ j(OVERFLOW, deopt);
      break;
    }
    case Token::kBIT_AND: {
      // No overflow check.
      __ andl(left, right);
      break;
    }
    case Token::kBIT_OR: {
      // No overflow check.
      __ orl(left, right);
      break;
    }
    case Token::kBIT_XOR: {
      // No overflow check.
      __ xorl(left, right);
      break;
    }
    case Token::kTRUNCDIV: {
      // Handle divide by zero in runtime.
      __ testl(right, right);
      __ j(ZERO, deopt);
      ASSERT(left == EAX);
      ASSERT((right != EDX) && (right != EAX));
      ASSERT(locs()->temp(0).reg() == EDX);
      ASSERT(result == EAX);
      __ SmiUntag(left);
      __ SmiUntag(right);
      __ cdq();  // Sign extend EAX -> EDX:EAX.
      __ idivl(right);  //  EAX: quotient, EDX: remainder.
      // Check the corner case of dividing the 'MIN_SMI' with -1, in which
      // case we cannot tag the result.
      __ cmpl(result, Immediate(0x40000000));
      __ j(EQUAL, deopt);
      __ SmiTag(result);
      break;
    }
    case Token::kSHR: {
      if (CanDeoptimize()) {
        __ cmpl(right, Immediate(0));
        __ j(LESS, deopt);
      }
      __ SmiUntag(right);
      // sarl operation masks the count to 5 bits.
      const intptr_t kCountLimit = 0x1F;
      Range* right_range = this->right()->definition()->range();
      if ((right_range == NULL) ||
          !right_range->IsWithin(RangeBoundary::kMinusInfinity, kCountLimit)) {
       __ cmpl(right, Immediate(kCountLimit));
        Label count_ok;
        __ j(LESS, &count_ok, Assembler::kNearJump);
        __ movl(right, Immediate(kCountLimit));
        __ Bind(&count_ok);
      }
      ASSERT(right == ECX);  // Count must be in ECX
      __ SmiUntag(left);
      __ sarl(left, right);
      __ SmiTag(left);
      break;
    }
    case Token::kSHL: {
      Register temp = locs()->temp(0).reg();
      // Check if count too large for handling it inlined.
      __ movl(temp, left);
      Range* right_range = this->right()->definition()->range();
      const bool right_needs_check =
          (right_range == NULL) || !right_range->IsWithin(0, (Smi::kBits - 1));
      if (right_needs_check) {
        __ cmpl(right,
          Immediate(reinterpret_cast<int32_t>(Smi::New(Smi::kBits))));
        __ j(ABOVE_EQUAL, deopt);
      }
      ASSERT(right == ECX);  // Count must be in ECX
      __ SmiUntag(right);
      // Overflow test (preserve temp and right);
      __ shll(left, right);
      __ sarl(left, right);
      __ cmpl(left, temp);
      __ j(NOT_EQUAL, deopt);  // Overflow.
      // Shift for result now we know there is no overflow.
      __ shll(left, right);
      break;
    }
    case Token::kDIV: {
      // Dispatches to 'Double./'.
      // TODO(srdjan): Implement as conversion to double and double division.
      UNREACHABLE();
      break;
    }
    case Token::kMOD: {
      // TODO(srdjan): Implement.
      UNREACHABLE();
      break;
    }
    case Token::kOR:
    case Token::kAND: {
      // Flow graph builder has dissected this operation to guarantee correct
      // behavior (short-circuit evaluation).
      UNREACHABLE();
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
}


LocationSummary* CheckEitherNonSmiInstr::MakeLocationSummary() const {
  ASSERT((left()->ResultCid() != kDoubleCid) &&
         (right()->ResultCid() != kDoubleCid));
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 1;
  LocationSummary* summary =
    new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_in(1, Location::RequiresRegister());
  summary->set_temp(0, Location::RequiresRegister());
  return summary;
}


void CheckEitherNonSmiInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(deopt_id(), kDeoptBinaryDoubleOp);
  Register temp = locs()->temp(0).reg();
  __ movl(temp, locs()->in(0).reg());
  __ orl(temp, locs()->in(1).reg());
  __ testl(temp, Immediate(kSmiTagMask));
  __ j(ZERO, deopt);
}


LocationSummary* BoxDoubleInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs,
                          kNumTemps,
                          LocationSummary::kCallOnSlowPath);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_out(Location::RequiresRegister());
  return summary;
}


class BoxDoubleSlowPath : public SlowPathCode {
 public:
  explicit BoxDoubleSlowPath(BoxDoubleInstr* instruction)
      : instruction_(instruction) { }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    __ Comment("BoxDoubleSlowPath");
    __ Bind(entry_label());
    const Class& double_class = compiler->double_class();
    const Code& stub =
        Code::Handle(StubCode::GetAllocationStubForClass(double_class));
    const ExternalLabel label(double_class.ToCString(), stub.EntryPoint());

    LocationSummary* locs = instruction_->locs();
    locs->live_registers()->Remove(locs->out());

    compiler->SaveLiveRegisters(locs);
    compiler->GenerateCall(instruction_->token_pos(),
                           &label,
                           PcDescriptors::kOther,
                           locs);
    if (EAX != locs->out().reg()) __ movl(locs->out().reg(), EAX);
    compiler->RestoreLiveRegisters(locs);

    __ jmp(exit_label());
  }

 private:
  BoxDoubleInstr* instruction_;
};


void BoxDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  BoxDoubleSlowPath* slow_path = new BoxDoubleSlowPath(this);
  compiler->AddSlowPathCode(slow_path);

  Register out_reg = locs()->out().reg();
  XmmRegister value = locs()->in(0).fpu_reg();

  AssemblerMacros::TryAllocate(compiler->assembler(),
                               compiler->double_class(),
                               slow_path->entry_label(),
                               Assembler::kFarJump,
                               out_reg);
  __ Bind(slow_path->exit_label());
  __ movsd(FieldAddress(out_reg, Double::value_offset()), value);
}


LocationSummary* UnboxDoubleInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = CanDeoptimize() ? 1 : 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  if (CanDeoptimize()) summary->set_temp(0, Location::RequiresRegister());
  summary->set_out(Location::RequiresFpuRegister());
  return summary;
}


void UnboxDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const intptr_t value_cid = value()->ResultCid();
  const Register value = locs()->in(0).reg();
  const XmmRegister result = locs()->out().fpu_reg();

  if (value_cid == kDoubleCid) {
    __ movsd(result, FieldAddress(value, Double::value_offset()));
  } else if (value_cid == kSmiCid) {
    __ SmiUntag(value);  // Untag input before conversion.
    __ cvtsi2sd(result, value);
    __ SmiTag(value);  // Restore input register.
  } else {
    Label* deopt = compiler->AddDeoptStub(deopt_id_, kDeoptBinaryDoubleOp);
    compiler->LoadDoubleOrSmiToFpu(result,
                                   value,
                                   locs()->temp(0).reg(),
                                   deopt);
  }
}


LocationSummary* BinaryDoubleOpInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_in(1, Location::RequiresFpuRegister());
  summary->set_out(Location::SameAsFirstInput());
  return summary;
}


void BinaryDoubleOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  XmmRegister left = locs()->in(0).fpu_reg();
  XmmRegister right = locs()->in(1).fpu_reg();

  ASSERT(locs()->out().fpu_reg() == left);

  switch (op_kind()) {
    case Token::kADD: __ addsd(left, right); break;
    case Token::kSUB: __ subsd(left, right); break;
    case Token::kMUL: __ mulsd(left, right); break;
    case Token::kDIV: __ divsd(left, right); break;
    default: UNREACHABLE();
  }
}


LocationSummary* MathSqrtInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_out(Location::RequiresFpuRegister());
  return summary;
}


void MathSqrtInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ sqrtsd(locs()->out().fpu_reg(), locs()->in(0).fpu_reg());
}


LocationSummary* UnarySmiOpInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_out(Location::SameAsFirstInput());
  return summary;
}


void UnarySmiOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  ASSERT(value == locs()->out().reg());
  switch (op_kind()) {
    case Token::kNEGATE: {
      Label* deopt = compiler->AddDeoptStub(deopt_id(),
                                            kDeoptUnaryOp);
      __ negl(value);
      __ j(OVERFLOW, deopt);
      break;
    }
    case Token::kBIT_NOT:
      __ notl(value);
      __ andl(value, Immediate(~kSmiTagMask));  // Remove inverted smi-tag.
      break;
    default:
      UNREACHABLE();
  }
}


LocationSummary* SmiToDoubleInstr::MakeLocationSummary() const {
  return MakeCallSummary();  // Calls a stub to allocate result.
}


void SmiToDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out().reg();

  Label* deopt = compiler->AddDeoptStub(instance_call()->deopt_id(),
                                        kDeoptIntegerToDouble);

  const Class& double_class = compiler->double_class();
  const Code& stub =
      Code::Handle(StubCode::GetAllocationStubForClass(double_class));
  const ExternalLabel label(double_class.ToCString(), stub.EntryPoint());

  // TODO(vegorov): allocate box in the driver loop to avoid spilling.
  compiler->GenerateCall(instance_call()->token_pos(),
                         &label,
                         PcDescriptors::kOther,
                         locs());
  ASSERT(result == EAX);
  Register value = EBX;
  // Preserve argument on the stack until after the deoptimization point.
  __ movl(value, Address(ESP, 0));

  __ testl(value, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);  // Deoptimize if not Smi.
  __ SmiUntag(value);
  __ cvtsi2sd(XMM0, value);
  __ movsd(FieldAddress(result, Double::value_offset()), XMM0);
  __ Drop(1);
}


LocationSummary* DoubleToIntegerInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* result =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kCall);
  result->set_in(0, Location::RegisterLocation(ECX));
  result->set_out(Location::RegisterLocation(EAX));
  return result;
}


void DoubleToIntegerInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register result = locs()->out().reg();
  Register value_obj = locs()->in(0).reg();
  XmmRegister value_double = XMM0;
  ASSERT(result == EAX);
  ASSERT(result != value_obj);
  __ movsd(value_double, FieldAddress(value_obj, Double::value_offset()));
  __ cvttsd2si(result, value_double);
  // Overflow is signalled with minint.
  Label do_call, done;
  // Check for overflow and that it fits into Smi.
  __ cmpl(result, Immediate(0xC0000000));
  __ j(NEGATIVE, &do_call, Assembler::kNearJump);
  __ SmiTag(result);
  __ jmp(&done);
  __ Bind(&do_call);
  __ pushl(value_obj);
  ASSERT(instance_call()->HasICData());
  const ICData& ic_data = *instance_call()->ic_data();
  ASSERT((ic_data.NumberOfChecks() == 1));
  const Function& target = Function::ZoneHandle(ic_data.GetTargetAt(0));

  const intptr_t kNumberOfArguments = 1;
  compiler->GenerateStaticCall(instance_call()->deopt_id(),
                               instance_call()->token_pos(),
                               target,
                               kNumberOfArguments,
                               Array::Handle(),  // No argument names.,
                               locs());
  __ Bind(&done);
}


LocationSummary* DoubleToSmiInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* result = new LocationSummary(
      kNumInputs, kNumTemps, LocationSummary::kNoCall);
  result->set_in(0, Location::RequiresFpuRegister());
  result->set_out(Location::RequiresRegister());
  return result;
}


void DoubleToSmiInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(deopt_id(), kDeoptDoubleToSmi);
  Register result = locs()->out().reg();
  XmmRegister value = locs()->in(0).fpu_reg();
  __ cvttsd2si(result, value);
  // Overflow is signalled with minint.
  Label do_call, done;
  // Check for overflow and that it fits into Smi.
  __ cmpl(result, Immediate(0xC0000000));
  __ j(NEGATIVE, deopt);
  __ SmiTag(result);
}


LocationSummary* DoubleToDoubleInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps =
      (recognized_kind() == MethodRecognizer::kDoubleRound) ? 1 : 0;
  LocationSummary* result =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  result->set_in(0, Location::RequiresFpuRegister());
  result->set_out(Location::RequiresFpuRegister());
  if (recognized_kind() == MethodRecognizer::kDoubleRound) {
    result->set_temp(0, Location::RequiresFpuRegister());
  }
  return result;
}


void DoubleToDoubleInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  XmmRegister value = locs()->in(0).fpu_reg();
  XmmRegister result = locs()->out().fpu_reg();
  switch (recognized_kind()) {
    case MethodRecognizer::kDoubleTruncate:
      __ roundsd(result, value,  Assembler::kRoundToZero);
      break;
    case MethodRecognizer::kDoubleFloor:
      __ roundsd(result, value,  Assembler::kRoundDown);
      break;
    case MethodRecognizer::kDoubleCeil:
      __ roundsd(result, value,  Assembler::kRoundUp);
      break;
    case MethodRecognizer::kDoubleRound: {
      XmmRegister temp = locs()->temp(0).fpu_reg();
      __ DoubleRound(result, value, temp);
      break;
    }
    default:
      UNREACHABLE();
  }
}


LocationSummary* InvokeMathCFunctionInstr::MakeLocationSummary() const {
  ASSERT((InputCount() == 1) || (InputCount() == 2));
  const intptr_t kNumTemps = 0;
  LocationSummary* result =
      new LocationSummary(InputCount(), kNumTemps, LocationSummary::kCall);
  result->set_in(0, Location::FpuRegisterLocation(XMM1, Location::kDouble));
  if (InputCount() == 2) {
    result->set_in(1, Location::FpuRegisterLocation(XMM2, Location::kDouble));
  }
  result->set_out(Location::FpuRegisterLocation(XMM1, Location::kDouble));
  return result;
}


void InvokeMathCFunctionInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ EnterFrame(0);
  __ ReserveAlignedFrameSpace(kDoubleSize * InputCount());
  for (intptr_t i = 0; i < InputCount(); i++) {
    __ movsd(Address(ESP, kDoubleSize * i), locs()->in(i).fpu_reg());
  }
  __ CallRuntime(TargetFunction());
  __ fstpl(Address(ESP, 0));
  __ movsd(locs()->out().fpu_reg(), Address(ESP, 0));
  __ leave();
}


LocationSummary* PolymorphicInstanceCallInstr::MakeLocationSummary() const {
  return MakeCallSummary();
}


void PolymorphicInstanceCallInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(instance_call()->deopt_id(),
                                        kDeoptPolymorphicInstanceCallTestFail);
  if (ic_data().NumberOfChecks() == 0) {
    __ jmp(deopt);
    return;
  }
  ASSERT(ic_data().num_args_tested() == 1);
  if (!with_checks()) {
    ASSERT(ic_data().HasOneTarget());
    const Function& target = Function::ZoneHandle(ic_data().GetTargetAt(0));
    compiler->GenerateStaticCall(instance_call()->deopt_id(),
                                 instance_call()->token_pos(),
                                 target,
                                 instance_call()->ArgumentCount(),
                                 instance_call()->argument_names(),
                                 locs());
    return;
  }

  // Load receiver into EAX.
  __ movl(EAX,
      Address(ESP, (instance_call()->ArgumentCount() - 1) * kWordSize));

  Label done;
  if (ic_data().GetReceiverClassIdAt(0) == kSmiCid) {
    __ movl(EDI, Immediate(kSmiCid));
    __ testl(EAX, Immediate(kSmiTagMask));
    __ j(ZERO, &done, Assembler::kNearJump);
  } else {
    __ testl(EAX, Immediate(kSmiTagMask));
    __ j(ZERO, deopt);
  }
  __ LoadClassId(EDI, EAX);
  __ Bind(&done);

  compiler->EmitTestAndCall(ic_data(),
                            EDI,  // Class id register.
                            instance_call()->ArgumentCount(),
                            instance_call()->argument_names(),
                            deopt,
                            instance_call()->deopt_id(),
                            instance_call()->token_pos(),
                            locs());
}


LocationSummary* BranchInstr::MakeLocationSummary() const {
  UNREACHABLE();
  return NULL;
}


void BranchInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  comparison()->EmitBranchCode(compiler, this);
}


LocationSummary* CheckClassInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 1;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  summary->set_temp(0, Location::RequiresRegister());
  return summary;
}


void CheckClassInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT((unary_checks().GetReceiverClassIdAt(0) != kSmiCid) ||
         (unary_checks().NumberOfChecks() > 1));
  Register value = locs()->in(0).reg();
  Register temp = locs()->temp(0).reg();
  Label* deopt = compiler->AddDeoptStub(deopt_id(),
                                        kDeoptCheckClass);
  Label is_ok;
  intptr_t cix = 0;
  if (unary_checks().GetReceiverClassIdAt(cix) == kSmiCid) {
    __ testl(value, Immediate(kSmiTagMask));
    __ j(ZERO, &is_ok);
    cix++;  // Skip first check.
  } else {
    __ testl(value, Immediate(kSmiTagMask));
    __ j(ZERO, deopt);
  }
  __ LoadClassId(temp, value);
  const intptr_t num_checks = unary_checks().NumberOfChecks();
  const bool use_near_jump = num_checks < 5;
  for (intptr_t i = cix; i < num_checks; i++) {
    ASSERT(unary_checks().GetReceiverClassIdAt(i) != kSmiCid);
    __ cmpl(temp, Immediate(unary_checks().GetReceiverClassIdAt(i)));
    if (i == (num_checks - 1)) {
      __ j(NOT_EQUAL, deopt);
    } else {
      if (use_near_jump) {
        __ j(EQUAL, &is_ok, Assembler::kNearJump);
      } else {
        __ j(EQUAL, &is_ok);
      }
    }
  }
  __ Bind(&is_ok);
}


LocationSummary* CheckSmiInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  return summary;
}


void CheckSmiInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // TODO(srdjan): Check if we can remove this by reordering CSE and LICM.
  if (value()->ResultCid() == kSmiCid) return;
  Register value = locs()->in(0).reg();
  Label* deopt = compiler->AddDeoptStub(deopt_id(),
                                        kDeoptCheckSmi);
  __ testl(value, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, deopt);
}


LocationSummary* CheckArrayBoundInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RegisterOrSmiConstant(array()));
  locs->set_in(1, Location::RegisterOrSmiConstant(index()));
  return locs;
}


void CheckArrayBoundInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Label* deopt = compiler->AddDeoptStub(deopt_id(),
                                        kDeoptCheckArrayBound);
  if (locs()->in(0).IsConstant() && locs()->in(1).IsConstant()) {
    // Unconditionally deoptimize for constant bounds checks because they
    // only occur only when index is out-of-bounds.
    __ jmp(deopt);
    return;
  }

  intptr_t length_offset = LengthOffsetFor(array_type());
  if (locs()->in(1).IsConstant()) {
    Register receiver = locs()->in(0).reg();
    const Object& constant = locs()->in(1).constant();
    ASSERT(constant.IsSmi());
    const int32_t imm =
        reinterpret_cast<int32_t>(constant.raw());
    __ cmpl(FieldAddress(receiver, length_offset), Immediate(imm));
    __ j(BELOW_EQUAL, deopt);
  } else if (locs()->in(0).IsConstant()) {
    ASSERT(locs()->in(0).constant().IsArray() ||
           locs()->in(0).constant().IsString());
    intptr_t length = locs()->in(0).constant().IsArray()
        ? Array::Cast(locs()->in(0).constant()).Length()
        : String::Cast(locs()->in(0).constant()).Length();
    Register index = locs()->in(1).reg();
    __ cmpl(index,
        Immediate(reinterpret_cast<int32_t>(Smi::New(length))));
    __ j(ABOVE_EQUAL, deopt);
  } else {
    Register receiver = locs()->in(0).reg();
    Register index = locs()->in(1).reg();
    __ cmpl(index, FieldAddress(receiver, length_offset));
    __ j(ABOVE_EQUAL, deopt);
  }
}


LocationSummary* UnboxIntegerInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = CanDeoptimize() ? 1 : 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresRegister());
  if (CanDeoptimize()) summary->set_temp(0, Location::RequiresRegister());
  summary->set_out(Location::RequiresFpuRegister());
  return summary;
}


void UnboxIntegerInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const intptr_t value_cid = value()->ResultCid();
  const Register value = locs()->in(0).reg();
  const XmmRegister result = locs()->out().fpu_reg();

  if (value_cid == kMintCid) {
    __ movsd(result, FieldAddress(value, Mint::value_offset()));
  } else if (value_cid == kSmiCid) {
    __ SmiUntag(value);  // Untag input before conversion.
    __ movd(result, value);
    __ pmovsxdq(result, result);
    __ SmiTag(value);  // Restore input register.
  } else {
    Register temp = locs()->temp(0).reg();
    Label* deopt = compiler->AddDeoptStub(deopt_id_, kDeoptUnboxInteger);
    Label is_smi, done;
    __ testl(value, Immediate(kSmiTagMask));
    __ j(ZERO, &is_smi);
    __ CompareClassId(value, kMintCid, temp);
    __ j(NOT_EQUAL, deopt);
    __ movsd(result, FieldAddress(value, Mint::value_offset()));
    __ jmp(&done);
    __ Bind(&is_smi);
    __ movl(temp, value);
    __ SmiUntag(temp);
    __ movd(result, temp);
    __ pmovsxdq(result, result);
    __ Bind(&done);
  }
}


LocationSummary* BoxIntegerInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 2;
  LocationSummary* summary =
      new LocationSummary(kNumInputs,
                          kNumTemps,
                          LocationSummary::kCallOnSlowPath);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_temp(0, Location::RegisterLocation(EAX));
  summary->set_temp(1, Location::RegisterLocation(EDX));
  // TODO(fschneider): Save one temp by using result register as a temp.
  summary->set_out(Location::RequiresRegister());
  return summary;
}


class BoxIntegerSlowPath : public SlowPathCode {
 public:
  explicit BoxIntegerSlowPath(BoxIntegerInstr* instruction)
      : instruction_(instruction) { }

  virtual void EmitNativeCode(FlowGraphCompiler* compiler) {
    __ Comment("BoxIntegerSlowPath");
    __ Bind(entry_label());
    const Class& mint_class =
        Class::ZoneHandle(Isolate::Current()->object_store()->mint_class());
    const Code& stub =
        Code::Handle(StubCode::GetAllocationStubForClass(mint_class));
    const ExternalLabel label(mint_class.ToCString(), stub.EntryPoint());

    LocationSummary* locs = instruction_->locs();
    locs->live_registers()->Remove(locs->out());

    compiler->SaveLiveRegisters(locs);
    compiler->GenerateCall(0,  // No token pos.
                           &label,
                           PcDescriptors::kOther,
                           locs);
    if (EAX != locs->out().reg()) __ movl(locs->out().reg(), EAX);
    compiler->RestoreLiveRegisters(locs);

    __ jmp(exit_label());
  }

 private:
  BoxIntegerInstr* instruction_;
};


void BoxIntegerInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  BoxIntegerSlowPath* slow_path = new BoxIntegerSlowPath(this);
  compiler->AddSlowPathCode(slow_path);

  Register out_reg = locs()->out().reg();
  XmmRegister value = locs()->in(0).fpu_reg();

  // Unboxed operations produce smis or mint-sized values.
  // Check if value fits into a smi.
  Label not_smi, done;
  __ pextrd(EDX, value, Immediate(1));  // Upper half.
  __ pextrd(EAX, value, Immediate(0));  // Lower half.
  // 1. Compute (x + -kMinSmi) which has to be in the range
  //    0 .. -kMinSmi+kMaxSmi for x to fit into a smi.
  __ addl(EAX, Immediate(0x40000000));
  __ adcl(EDX, Immediate(0));
  // 2. Unsigned compare to -kMinSmi+kMaxSmi.
  __ cmpl(EAX, Immediate(0x80000000));
  __ sbbl(EDX, Immediate(0));
  __ j(ABOVE_EQUAL, &not_smi);
  // 3. Restore lower half if result is a smi.
  __ subl(EAX, Immediate(0x40000000));

  __ SmiTag(EAX);
  __ movl(out_reg, EAX);
  __ jmp(&done);

  __ Bind(&not_smi);
  AssemblerMacros::TryAllocate(
      compiler->assembler(),
      Class::ZoneHandle(Isolate::Current()->object_store()->mint_class()),
      slow_path->entry_label(),
      Assembler::kFarJump,
      out_reg);
  __ Bind(slow_path->exit_label());
  __ movsd(FieldAddress(out_reg, Mint::value_offset()), value);
  __ Bind(&done);
}


LocationSummary* BinaryMintOpInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  switch (op_kind()) {
    case Token::kBIT_AND:
    case Token::kBIT_OR:
    case Token::kBIT_XOR: {
      const intptr_t kNumTemps = 0;
      LocationSummary* summary =
          new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
      summary->set_in(0, Location::RequiresFpuRegister());
      summary->set_in(1, Location::RequiresFpuRegister());
      summary->set_out(Location::SameAsFirstInput());
      return summary;
    }
    case Token::kADD:
    case Token::kSUB: {
      const intptr_t kNumTemps = 2;
      LocationSummary* summary =
          new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
      summary->set_in(0, Location::RequiresFpuRegister());
      summary->set_in(1, Location::RequiresFpuRegister());
      summary->set_temp(0, Location::RequiresRegister());
      summary->set_temp(1, Location::RequiresRegister());
      summary->set_out(Location::SameAsFirstInput());
      return summary;
    }
    default:
      UNREACHABLE();
      return NULL;
  }
}


void BinaryMintOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  XmmRegister left = locs()->in(0).fpu_reg();
  XmmRegister right = locs()->in(1).fpu_reg();

  ASSERT(locs()->out().fpu_reg() == left);

  switch (op_kind()) {
    case Token::kBIT_AND: __ andpd(left, right); break;
    case Token::kBIT_OR:  __ orpd(left, right); break;
    case Token::kBIT_XOR: __ xorpd(left, right); break;
    case Token::kADD:
    case Token::kSUB: {
      Register lo = locs()->temp(0).reg();
      Register hi = locs()->temp(1).reg();
      Label* deopt  = compiler->AddDeoptStub(deopt_id(),
                                             kDeoptBinaryMintOp);
      Label done, overflow;
      __ pextrd(lo, right, Immediate(0));  // Lower half
      __ pextrd(hi, right, Immediate(1));  // Upper half
      __ subl(ESP, Immediate(2 * kWordSize));
      __ movq(Address(ESP, 0), left);
      if (op_kind() == Token::kADD) {
        __ addl(Address(ESP, 0), lo);
        __ adcl(Address(ESP, 1 * kWordSize), hi);
      } else {
        __ subl(Address(ESP, 0), lo);
        __ sbbl(Address(ESP, 1 * kWordSize), hi);
      }
      __ j(OVERFLOW, &overflow);
      __ movq(left, Address(ESP, 0));
      __ addl(ESP, Immediate(2 * kWordSize));
      __ jmp(&done);
      __ Bind(&overflow);
      __ addl(ESP, Immediate(2 * kWordSize));
      __ jmp(deopt);
      __ Bind(&done);
      break;
    }
    default: UNREACHABLE();
  }
}


LocationSummary* ShiftMintOpInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = op_kind() == Token::kSHL ? 2 : 1;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_in(1, Location::RegisterLocation(ECX));
  summary->set_temp(0, Location::RequiresRegister());
  if (op_kind() == Token::kSHL) {
    summary->set_temp(1, Location::RequiresRegister());
  }
  summary->set_out(Location::SameAsFirstInput());
  return summary;
}


void ShiftMintOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  XmmRegister left = locs()->in(0).fpu_reg();
  ASSERT(locs()->in(1).reg() == ECX);
  ASSERT(locs()->out().fpu_reg() == left);

  Label* deopt  = compiler->AddDeoptStub(deopt_id(),
                                         kDeoptShiftMintOp);
  Label done;
  __ testl(ECX, ECX);
  __ j(ZERO, &done);  // Shift by 0 is a nop.
  __ subl(ESP, Immediate(2 * kWordSize));
  __ movq(Address(ESP, 0), left);
  // Deoptimize if shift count is > 31.
  // sarl operation masks the count to 5 bits and
  // shrd is undefined with count > operand size (32)
  // TODO(fschneider): Support shift counts > 31 without deoptimization.
  __ SmiUntag(ECX);
  const Immediate& kCountLimit = Immediate(31);
  __ cmpl(ECX, kCountLimit);
  __ j(ABOVE, deopt);
  switch (op_kind()) {
    case Token::kSHR: {
      Register temp = locs()->temp(0).reg();
      __ movl(temp, Address(ESP, 1 * kWordSize));  // High half.
      __ shrd(Address(ESP, 0), temp);  // Shift count in CL.
      __ sarl(Address(ESP, 1 * kWordSize), ECX);  // Shift count in CL.
      break;
    }
    case Token::kSHL: {
      Register temp1 = locs()->temp(0).reg();
      Register temp2 = locs()->temp(1).reg();
      __ movl(temp1, Address(ESP, 0 * kWordSize));  // Low 32 bits.
      __ movl(temp2, Address(ESP, 1 * kWordSize));  // High 32 bits.
      __ shll(Address(ESP, 0 * kWordSize), ECX);  // Shift count in CL.
      __ shld(Address(ESP, 1 * kWordSize), temp1);  // Shift count in CL.
      // Check for overflow by shifting back the high 32 bits
      // and comparing with the input.
      __ movl(temp1, temp2);
      __ movl(temp2, Address(ESP, 1 * kWordSize));
      __ sarl(temp2, ECX);
      __ cmpl(temp1, temp2);
      __ j(NOT_EQUAL, deopt);
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
  __ movq(left, Address(ESP, 0));
  __ addl(ESP, Immediate(2 * kWordSize));
  __ Bind(&done);
}


LocationSummary* UnaryMintOpInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 1;
  const intptr_t kNumTemps = 0;
  LocationSummary* summary =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  summary->set_in(0, Location::RequiresFpuRegister());
  summary->set_out(Location::SameAsFirstInput());
  return summary;
}


void UnaryMintOpInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(op_kind() == Token::kBIT_NOT);
  XmmRegister value = locs()->in(0).fpu_reg();
  ASSERT(value == locs()->out().fpu_reg());
  __ pcmpeqq(XMM0, XMM0);  // Generate all 1's.
  __ pxor(value, XMM0);
}


LocationSummary* ThrowInstr::MakeLocationSummary() const {
  return new LocationSummary(0, 0, LocationSummary::kCall);
}



void ThrowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  compiler->GenerateCallRuntime(token_pos(),
                                kThrowRuntimeEntry,
                                locs());
  __ int3();
}


LocationSummary* ReThrowInstr::MakeLocationSummary() const {
  return new LocationSummary(0, 0, LocationSummary::kCall);
}


void ReThrowInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  compiler->GenerateCallRuntime(token_pos(),
                                kReThrowRuntimeEntry,
                                locs());
  __ int3();
}


LocationSummary* GotoInstr::MakeLocationSummary() const {
  return new LocationSummary(0, 0, LocationSummary::kNoCall);
}


void GotoInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // Add deoptimization descriptor for deoptimizing instructions
  // that may be inserted before this instruction.
  if (!compiler->is_optimizing()) {
    compiler->AddCurrentDescriptor(PcDescriptors::kDeoptBefore,
                                   GetDeoptId(),
                                   0);  // No token position.
  }

  if (HasParallelMove()) {
    compiler->parallel_move_resolver()->EmitNativeCode(parallel_move());
  }

  // We can fall through if the successor is the next block in the list.
  // Otherwise, we need a jump.
  if (!compiler->IsNextBlock(successor())) {
    __ jmp(compiler->GetBlockLabel(successor()));
  }
}


static Condition NegateCondition(Condition condition) {
  switch (condition) {
    case EQUAL:         return NOT_EQUAL;
    case NOT_EQUAL:     return EQUAL;
    case LESS:          return GREATER_EQUAL;
    case LESS_EQUAL:    return GREATER;
    case GREATER:       return LESS_EQUAL;
    case GREATER_EQUAL: return LESS;
    case BELOW:         return ABOVE_EQUAL;
    case BELOW_EQUAL:   return ABOVE;
    case ABOVE:         return BELOW_EQUAL;
    case ABOVE_EQUAL:   return BELOW;
    default:
      OS::Print("Error %d\n", condition);
      UNIMPLEMENTED();
      return EQUAL;
  }
}


void ControlInstruction::EmitBranchOnValue(FlowGraphCompiler* compiler,
                                           bool value) {
  if (value && compiler->IsNextBlock(false_successor())) {
    __ jmp(compiler->GetBlockLabel(true_successor()));
  } else if (!value && compiler->IsNextBlock(true_successor())) {
    __ jmp(compiler->GetBlockLabel(false_successor()));
  }
}


void ControlInstruction::EmitBranchOnCondition(FlowGraphCompiler* compiler,
                                               Condition true_condition) {
  if (compiler->IsNextBlock(false_successor())) {
    // If the next block is the false successor we will fall through to it.
    __ j(true_condition, compiler->GetBlockLabel(true_successor()));
  } else {
    // If the next block is the true successor we negate comparison and fall
    // through to it.
    ASSERT(compiler->IsNextBlock(true_successor()));
    Condition false_condition = NegateCondition(true_condition);
    __ j(false_condition, compiler->GetBlockLabel(false_successor()));
  }
}


LocationSummary* CurrentContextInstr::MakeLocationSummary() const {
  return LocationSummary::Make(0,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void CurrentContextInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  __ MoveRegister(locs()->out().reg(), CTX);
}


LocationSummary* StrictCompareInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, Location::RegisterOrConstant(left()));
  locs->set_in(1, Location::RegisterOrConstant(right()));
  locs->set_out(Location::RequiresRegister());
  return locs;
}


// Special code for numbers (compare values instead of references.)
void StrictCompareInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  ASSERT(kind() == Token::kEQ_STRICT || kind() == Token::kNE_STRICT);
  Location left = locs()->in(0);
  Location right = locs()->in(1);
  if (left.IsConstant() && right.IsConstant()) {
    // TODO(vegorov): should be eliminated earlier by constant propagation.
    const bool result = (kind() == Token::kEQ_STRICT) ?
        left.constant().raw() == right.constant().raw() :
        left.constant().raw() != right.constant().raw();
    __ LoadObject(locs()->out().reg(), result ? Bool::True() : Bool::False());
    return;
  }
  if (left.IsConstant()) {
    compiler->EmitEqualityRegConstCompare(right.reg(),
                                          left.constant(),
                                          needs_number_check());
  } else if (right.IsConstant()) {
    compiler->EmitEqualityRegConstCompare(left.reg(),
                                          right.constant(),
                                          needs_number_check());
  } else {
    compiler->EmitEqualityRegRegCompare(left.reg(),
                                       right.reg(),
                                       needs_number_check());
  }

  Register result = locs()->out().reg();
  Label load_true, done;
  Condition true_condition = (kind() == Token::kEQ_STRICT) ? EQUAL : NOT_EQUAL;
  __ j(true_condition, &load_true, Assembler::kNearJump);
  __ LoadObject(result, Bool::False());
  __ jmp(&done, Assembler::kNearJump);
  __ Bind(&load_true);
  __ LoadObject(result, Bool::True());
  __ Bind(&done);
}


void StrictCompareInstr::EmitBranchCode(FlowGraphCompiler* compiler,
                                        BranchInstr* branch) {
  ASSERT(kind() == Token::kEQ_STRICT || kind() == Token::kNE_STRICT);
  Location left = locs()->in(0);
  Location right = locs()->in(1);
  if (left.IsConstant() && right.IsConstant()) {
    // TODO(vegorov): should be eliminated earlier by constant propagation.
    const bool result = (kind() == Token::kEQ_STRICT) ?
        left.constant().raw() == right.constant().raw() :
        left.constant().raw() != right.constant().raw();
    branch->EmitBranchOnValue(compiler, result);
    return;
  }
  if (left.IsConstant()) {
    compiler->EmitEqualityRegConstCompare(right.reg(),
                                          left.constant(),
                                          needs_number_check());
  } else if (right.IsConstant()) {
    compiler->EmitEqualityRegConstCompare(left.reg(),
                                          right.constant(),
                                          needs_number_check());
  } else {
    compiler->EmitEqualityRegRegCompare(left.reg(),
                                        right.reg(),
                                        needs_number_check());
  }

  Condition true_condition = (kind() == Token::kEQ_STRICT) ? EQUAL : NOT_EQUAL;
  branch->EmitBranchOnCondition(compiler, true_condition);
}


void ClosureCallInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  // The arguments to the stub include the closure, as does the arguments
  // descriptor.
  Register temp_reg = locs()->temp(0).reg();
  int argument_count = ArgumentCount();
  const Array& arguments_descriptor =
      Array::ZoneHandle(ArgumentsDescriptor::New(argument_count,
                                                 argument_names()));
  __ LoadObject(temp_reg, arguments_descriptor);
  compiler->GenerateDartCall(deopt_id(),
                             token_pos(),
                             &StubCode::CallClosureFunctionLabel(),
                             PcDescriptors::kOther,
                             locs());
  __ Drop(argument_count);
}


LocationSummary* BooleanNegateInstr::MakeLocationSummary() const {
  return LocationSummary::Make(1,
                               Location::RequiresRegister(),
                               LocationSummary::kNoCall);
}


void BooleanNegateInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value = locs()->in(0).reg();
  Register result = locs()->out().reg();

  Label done;
  __ LoadObject(result, Bool::True());
  __ CompareRegisters(result, value);
  __ j(NOT_EQUAL, &done, Assembler::kNearJump);
  __ LoadObject(result, Bool::False());
  __ Bind(&done);
}


LocationSummary* ChainContextInstr::MakeLocationSummary() const {
  return LocationSummary::Make(1,
                               Location::NoLocation(),
                               LocationSummary::kNoCall);
}


void ChainContextInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register context_value = locs()->in(0).reg();

  // Chain the new context in context_value to its parent in CTX.
  __ StoreIntoObject(context_value,
                     FieldAddress(context_value, Context::parent_offset()),
                     CTX);
  // Set new context as current context.
  __ MoveRegister(CTX, context_value);
}


LocationSummary* StoreVMFieldInstr::MakeLocationSummary() const {
  const intptr_t kNumInputs = 2;
  const intptr_t kNumTemps = 0;
  LocationSummary* locs =
      new LocationSummary(kNumInputs, kNumTemps, LocationSummary::kNoCall);
  locs->set_in(0, value()->NeedsStoreBuffer() ? Location::WritableRegister()
                                              : Location::RequiresRegister());
  locs->set_in(1, Location::RequiresRegister());
  return locs;
}


void StoreVMFieldInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  Register value_reg = locs()->in(0).reg();
  Register dest_reg = locs()->in(1).reg();

  if (value()->NeedsStoreBuffer()) {
    __ StoreIntoObject(dest_reg, FieldAddress(dest_reg, offset_in_bytes()),
                       value_reg);
  } else {
    __ StoreIntoObjectNoBarrier(
        dest_reg, FieldAddress(dest_reg, offset_in_bytes()), value_reg);
  }
}


LocationSummary* AllocateObjectInstr::MakeLocationSummary() const {
  return MakeCallSummary();
}


void AllocateObjectInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Class& cls = Class::ZoneHandle(constructor().Owner());
  const Code& stub = Code::Handle(StubCode::GetAllocationStubForClass(cls));
  const ExternalLabel label(cls.ToCString(), stub.EntryPoint());
  compiler->GenerateCall(token_pos(),
                         &label,
                         PcDescriptors::kOther,
                         locs());
  __ Drop(ArgumentCount());  // Discard arguments.
}


LocationSummary* CreateClosureInstr::MakeLocationSummary() const {
  return MakeCallSummary();
}


void CreateClosureInstr::EmitNativeCode(FlowGraphCompiler* compiler) {
  const Function& closure_function = function();
  ASSERT(!closure_function.IsImplicitStaticClosureFunction());
  const Code& stub = Code::Handle(
      StubCode::GetAllocationStubForClosure(closure_function));
  const ExternalLabel label(closure_function.ToCString(), stub.EntryPoint());
  compiler->GenerateCall(token_pos(),
                         &label,
                         PcDescriptors::kOther,
                         locs());
  __ Drop(2);  // Discard type arguments and receiver.
}

}  // namespace dart

#undef __

#endif  // defined TARGET_ARCH_IA32
