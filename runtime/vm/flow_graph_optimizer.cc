// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/flow_graph_optimizer.h"

#include "vm/bit_vector.h"
#include "vm/cha.h"
#include "vm/flow_graph_builder.h"
#include "vm/flow_graph_compiler.h"
#include "vm/hash_map.h"
#include "vm/il_printer.h"
#include "vm/intermediate_language.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/resolver.h"
#include "vm/scopes.h"
#include "vm/symbols.h"

namespace dart {

DECLARE_FLAG(bool, eliminate_type_checks);
DECLARE_FLAG(bool, enable_type_checks);
DEFINE_FLAG(bool, trace_optimization, false, "Print optimization details.");
DECLARE_FLAG(bool, trace_type_check_elimination);
DEFINE_FLAG(bool, use_cha, true, "Use class hierarchy analysis.");
DEFINE_FLAG(bool, load_cse, true, "Use redundant load elimination.");
DEFINE_FLAG(bool, trace_range_analysis, false, "Trace range analysis progress");
DEFINE_FLAG(bool, trace_constant_propagation, false,
    "Print constant propagation and useless code elimination.");
DEFINE_FLAG(bool, array_bounds_check_elimination, true,
    "Eliminate redundant bounds checks.");
DEFINE_FLAG(int, max_polymorphic_checks, 4,
    "Maximum number of polymorphic check, otherwise it is megamorphic.");
DEFINE_FLAG(bool, remove_redundant_phis, true, "Remove redundant phis.");


void FlowGraphOptimizer::ApplyICData() {
  VisitBlocks();
}


// Attempts to convert an instance call (IC call) using propagated class-ids,
// e.g., receiver class id.
void FlowGraphOptimizer::ApplyClassIds() {
  ASSERT(current_iterator_ == NULL);
  for (intptr_t i = 0; i < block_order_.length(); ++i) {
    BlockEntryInstr* entry = block_order_[i];
    ForwardInstructionIterator it(entry);
    current_iterator_ = &it;
    for (; !it.Done(); it.Advance()) {
      if (it.Current()->IsInstanceCall()) {
        InstanceCallInstr* call = it.Current()->AsInstanceCall();
        if (call->HasICData()) {
          if (TryCreateICData(call)) {
            VisitInstanceCall(call);
          }
        }
      } else if (it.Current()->IsPolymorphicInstanceCall()) {
        SpecializePolymorphicInstanceCall(
            it.Current()->AsPolymorphicInstanceCall());
      } else if (it.Current()->IsStrictCompare()) {
        VisitStrictCompare(it.Current()->AsStrictCompare());
      } else if (it.Current()->IsBranch()) {
        ComparisonInstr* compare = it.Current()->AsBranch()->comparison();
        if (compare->IsStrictCompare()) {
          VisitStrictCompare(compare->AsStrictCompare());
        }
      }
    }
    current_iterator_ = NULL;
  }
}


// Attempt to build ICData for call using propagated class-ids.
bool FlowGraphOptimizer::TryCreateICData(InstanceCallInstr* call) {
  ASSERT(call->HasICData());
  if (call->ic_data()->NumberOfChecks() > 0) {
    // This occurs when an instance call has too many checks.
    // TODO(srdjan): Replace IC call with megamorphic call.
    return false;
  }
  GrowableArray<intptr_t> class_ids(call->ic_data()->num_args_tested());
  ASSERT(call->ic_data()->num_args_tested() <= call->ArgumentCount());
  for (intptr_t i = 0; i < call->ic_data()->num_args_tested(); i++) {
    intptr_t cid = call->ArgumentAt(i)->value()->ResultCid();
    class_ids.Add(cid);
  }
  // TODO(srdjan): Test for other class_ids > 1.
  if (class_ids.length() != 1) return false;
  if (class_ids[0] != kDynamicCid) {
    const intptr_t num_named_arguments = call->argument_names().IsNull() ?
        0 : call->argument_names().Length();
    const Class& receiver_class = Class::Handle(
        Isolate::Current()->class_table()->At(class_ids[0]));
    Function& function = Function::Handle();
    function = Resolver::ResolveDynamicForReceiverClass(
        receiver_class,
        call->function_name(),
        call->ArgumentCount(),
        num_named_arguments);
    if (function.IsNull()) {
      return false;
    }
    // Create new ICData, do not modify the one attached to the instruction
    // since it is attached to the assembly instruction itself.
    // TODO(srdjan): Prevent modification of ICData object that is
    // referenced in assembly code.
    ICData& ic_data = ICData::ZoneHandle(ICData::New(
        flow_graph_->parsed_function().function(),
        call->function_name(),
        call->deopt_id(),
        class_ids.length()));
    ic_data.AddReceiverCheck(class_ids[0], function);
    call->set_ic_data(&ic_data);
    return true;
  }
  return false;
}


static const ICData& SpecializeICData(const ICData& ic_data, intptr_t cid) {
  ASSERT(ic_data.num_args_tested() == 1);

  if ((ic_data.NumberOfChecks() == 1) &&
      (ic_data.GetReceiverClassIdAt(0) == cid)) {
    return ic_data;  // Nothing to do
  }

  const ICData& new_ic_data = ICData::ZoneHandle(ICData::New(
      Function::Handle(ic_data.function()),
      String::Handle(ic_data.target_name()),
      ic_data.deopt_id(),
      ic_data.num_args_tested()));

  const Function& function =
      Function::Handle(ic_data.GetTargetForReceiverClassId(cid));
  if (!function.IsNull()) {
    new_ic_data.AddReceiverCheck(cid, function);
  }

  return new_ic_data;
}


void FlowGraphOptimizer::SpecializePolymorphicInstanceCall(
    PolymorphicInstanceCallInstr* call) {
  if (!call->with_checks()) {
    return;  // Already specialized.
  }

  const intptr_t receiver_cid  = call->ArgumentAt(0)->value()->ResultCid();
  if (receiver_cid == kDynamicCid) {
    return;  // No information about receiver was infered.
  }

  const ICData& ic_data = SpecializeICData(call->ic_data(), receiver_cid);

  const bool with_checks = false;
  PolymorphicInstanceCallInstr* specialized =
      new PolymorphicInstanceCallInstr(call->instance_call(),
                                       ic_data,
                                       with_checks);
  call->ReplaceWith(specialized, current_iterator());
}


static void EnsureSSATempIndex(FlowGraph* graph,
                               Definition* defn,
                               Definition* replacement) {
  if ((replacement->ssa_temp_index() == -1) &&
      (defn->ssa_temp_index() != -1)) {
    replacement->set_ssa_temp_index(graph->alloc_ssa_temp_index());
  }
}


static void ReplaceCurrentInstruction(ForwardInstructionIterator* it,
                                      Instruction* current,
                                      Instruction* replacement,
                                      FlowGraph* graph) {
  Definition* current_defn = current->AsDefinition();
  if ((replacement != NULL) && (current_defn != NULL)) {
    Definition* replacement_defn = replacement->AsDefinition();
    ASSERT(replacement_defn != NULL);
    current_defn->ReplaceUsesWith(replacement_defn);
    EnsureSSATempIndex(graph, current_defn, replacement_defn);

    if (FLAG_trace_optimization) {
      OS::Print("Replacing v%"Pd" with v%"Pd"\n",
                current_defn->ssa_temp_index(),
                replacement_defn->ssa_temp_index());
    }
  } else if (FLAG_trace_optimization) {
    if (current_defn == NULL) {
      OS::Print("Removing %s\n", current->DebugName());
    } else {
      ASSERT(!current_defn->HasUses());
      OS::Print("Removing v%"Pd".\n", current_defn->ssa_temp_index());
    }
  }
  it->RemoveCurrentFromGraph();
}


void FlowGraphOptimizer::Canonicalize() {
  for (intptr_t i = 0; i < block_order_.length(); ++i) {
    BlockEntryInstr* entry = block_order_[i];
    entry->Accept(this);
    for (ForwardInstructionIterator it(entry); !it.Done(); it.Advance()) {
      Instruction* current = it.Current();
      Instruction* replacement = current->Canonicalize(this);
      if (replacement != current) {
        // For non-definitions Canonicalize should return either NULL or
        // this.
        ASSERT((replacement == NULL) || current->IsDefinition());
        ReplaceCurrentInstruction(&it, current, replacement, flow_graph_);
      }
    }
  }
}


void FlowGraphOptimizer::InsertConversion(Representation from,
                                          Representation to,
                                          Instruction* instr,
                                          Value* use,
                                          Definition* def,
                                          Instruction* deopt_target) {
  Definition* converted = NULL;
  if ((from == kTagged) && (to == kUnboxedMint)) {
    const intptr_t deopt_id = (deopt_target != NULL) ?
        deopt_target->DeoptimizationTarget() : Isolate::kNoDeoptId;
    ASSERT((deopt_target != NULL) || (def->GetPropagatedCid() == kDoubleCid));
    converted = new UnboxIntegerInstr(new Value(def), deopt_id);
  } else if ((from == kUnboxedMint) && (to == kTagged)) {
    converted = new BoxIntegerInstr(new Value(def));
  } else if (from == kUnboxedMint && to == kUnboxedDouble) {
    // Convert by boxing/unboxing.
    // TODO(fschneider): Implement direct unboxed mint-to-double conversion.
    BoxIntegerInstr* boxed = new BoxIntegerInstr(new Value(def));
    InsertBefore(instr, boxed, NULL, Definition::kValue);
    const intptr_t deopt_id = (deopt_target != NULL) ?
        deopt_target->DeoptimizationTarget() : Isolate::kNoDeoptId;
    converted = new UnboxDoubleInstr(new Value(boxed), deopt_id);
  } else if ((from == kUnboxedDouble) && (to == kTagged)) {
    converted = new BoxDoubleInstr(new Value(def), NULL);
  } else if ((from == kTagged) && (to == kUnboxedDouble)) {
    const intptr_t deopt_id = (deopt_target != NULL) ?
        deopt_target->DeoptimizationTarget() : Isolate::kNoDeoptId;
    ASSERT((deopt_target != NULL) || (def->GetPropagatedCid() == kDoubleCid));
    if (def->IsConstant() && def->AsConstant()->value().IsSmi()) {
      const double dbl_val =
          Smi::Cast(def->AsConstant()->value()).AsDoubleValue();
      const Double& dbl_obj =
          Double::ZoneHandle(Double::New(dbl_val, Heap::kOld));
      ConstantInstr* double_const = new ConstantInstr(dbl_obj);
      InsertBefore(instr, double_const, NULL, Definition::kValue);
      converted = new UnboxDoubleInstr(new Value(double_const), deopt_id);
    } else {
      converted = new UnboxDoubleInstr(new Value(def), deopt_id);
    }
  }
  ASSERT(converted != NULL);
  InsertBefore(instr, converted, use->instruction()->env(),
               Definition::kValue);
  use->set_definition(converted);
}


void FlowGraphOptimizer::InsertConversionsFor(Definition* def) {
  const Representation from_rep = def->representation();

  for (Value* use = def->input_use_list();
       use != NULL;
       use = use->next_use()) {
    const Representation to_rep =
        use->instruction()->RequiredInputRepresentation(use->use_index());
    if (from_rep == to_rep) {
      continue;
    }

    Instruction* deopt_target = NULL;
    Instruction* instr = use->instruction();
    if (instr->IsPhi()) {
      if (!instr->AsPhi()->is_alive()) continue;

      // For phis conversions have to be inserted in the predecessor.
      const BlockEntryInstr* pred =
          instr->AsPhi()->block()->PredecessorAt(use->use_index());
      instr = pred->last_instruction();
    } else {
      deopt_target = instr;
    }

    InsertConversion(from_rep, to_rep, instr, use, def, deopt_target);
  }
}


void FlowGraphOptimizer::SelectRepresentations() {
  // Convervatively unbox all phis that were proven to be of type Double.
  for (intptr_t i = 0; i < block_order_.length(); ++i) {
    JoinEntryInstr* join_entry = block_order_[i]->AsJoinEntry();
    if (join_entry == NULL) continue;

    if (join_entry->phis() != NULL) {
      for (intptr_t i = 0; i < join_entry->phis()->length(); ++i) {
        PhiInstr* phi = (*join_entry->phis())[i];
        if (phi == NULL) continue;
        if (phi->GetPropagatedCid() == kDoubleCid) {
          phi->set_representation(kUnboxedDouble);
        }
      }
    }
  }

  // Process all instructions and insert conversions where needed.
  GraphEntryInstr* graph_entry = block_order_[0]->AsGraphEntry();

  // Visit incoming parameters and constants.
  for (intptr_t i = 0; i < graph_entry->initial_definitions()->length(); i++) {
    InsertConversionsFor((*graph_entry->initial_definitions())[i]);
  }

  for (intptr_t i = 0; i < block_order_.length(); ++i) {
    BlockEntryInstr* entry = block_order_[i];

    JoinEntryInstr* join_entry = entry->AsJoinEntry();
    if ((join_entry != NULL) && (join_entry->phis() != NULL)) {
      for (intptr_t i = 0; i < join_entry->phis()->length(); ++i) {
        PhiInstr* phi = (*join_entry->phis())[i];
        if ((phi != NULL) && (phi->is_alive())) {
          InsertConversionsFor(phi);
        }
      }
    }

    for (ForwardInstructionIterator it(entry); !it.Done(); it.Advance()) {
      Definition* def = it.Current()->AsDefinition();
      if (def != NULL) {
        InsertConversionsFor(def);
      }
    }
  }
}


static bool ICDataHasReceiverArgumentClassIds(const ICData& ic_data,
                                              intptr_t receiver_class_id,
                                              intptr_t argument_class_id) {
  ASSERT(receiver_class_id != kIllegalCid);
  ASSERT(argument_class_id != kIllegalCid);
  if (ic_data.num_args_tested() != 2) return false;

  Function& target = Function::Handle();
  const intptr_t len = ic_data.NumberOfChecks();
  for (intptr_t i = 0; i < len; i++) {
    GrowableArray<intptr_t> class_ids;
    ic_data.GetCheckAt(i, &class_ids, &target);
    ASSERT(class_ids.length() == 2);
    if ((class_ids[0] == receiver_class_id) &&
        (class_ids[1] == argument_class_id)) {
      return true;
    }
  }
  return false;
}


static bool ClassIdIsOneOf(intptr_t class_id,
                           const GrowableArray<intptr_t>& class_ids) {
  for (intptr_t i = 0; i < class_ids.length(); i++) {
    if (class_ids[i] == class_id) {
      return true;
    }
  }
  return false;
}


// Returns true if ICData tests two arguments and all ICData cids are in the
// required sets 'receiver_class_ids' or 'argument_class_ids', respectively.
static bool ICDataHasOnlyReceiverArgumentClassIds(
    const ICData& ic_data,
    const GrowableArray<intptr_t>& receiver_class_ids,
    const GrowableArray<intptr_t>& argument_class_ids) {
  if (ic_data.num_args_tested() != 2) return false;
  Function& target = Function::Handle();
  const intptr_t len = ic_data.NumberOfChecks();
  for (intptr_t i = 0; i < len; i++) {
    GrowableArray<intptr_t> class_ids;
    ic_data.GetCheckAt(i, &class_ids, &target);
    ASSERT(class_ids.length() == 2);
    if (!ClassIdIsOneOf(class_ids[0], receiver_class_ids) ||
        !ClassIdIsOneOf(class_ids[1], argument_class_ids)) {
      return false;
    }
  }
  return true;
}


static bool HasOnlyOneSmi(const ICData& ic_data) {
  return (ic_data.NumberOfChecks() == 1)
      && ic_data.HasReceiverClassId(kSmiCid);
}


static bool HasOnlySmiOrMint(const ICData& ic_data) {
  if (ic_data.NumberOfChecks() == 1) {
    return ic_data.HasReceiverClassId(kSmiCid)
        || ic_data.HasReceiverClassId(kMintCid);
  }
  return (ic_data.NumberOfChecks() == 2)
      && ic_data.HasReceiverClassId(kSmiCid)
      && ic_data.HasReceiverClassId(kMintCid);
}


static bool HasOnlyTwoSmis(const ICData& ic_data) {
  return (ic_data.NumberOfChecks() == 1) &&
      ICDataHasReceiverArgumentClassIds(ic_data, kSmiCid, kSmiCid);
}


// Returns false if the ICData contains anything other than the 4 combinations
// of Mint and Smi for the receiver and argument classes.
static bool HasTwoMintOrSmi(const ICData& ic_data) {
  GrowableArray<intptr_t> class_ids(2);
  class_ids.Add(kSmiCid);
  class_ids.Add(kMintCid);
  return ICDataHasOnlyReceiverArgumentClassIds(ic_data, class_ids, class_ids);
}


static bool HasOnlyOneDouble(const ICData& ic_data) {
  return (ic_data.NumberOfChecks() == 1)
      && ic_data.HasReceiverClassId(kDoubleCid);
}


static bool ShouldSpecializeForDouble(const ICData& ic_data) {
  // Unboxed double operation can't handle case of two smis.
  if (ICDataHasReceiverArgumentClassIds(ic_data, kSmiCid, kSmiCid)) {
    return false;
  }

  // Check that it have seen only smis and doubles.
  GrowableArray<intptr_t> class_ids(2);
  class_ids.Add(kSmiCid);
  class_ids.Add(kDoubleCid);
  return ICDataHasOnlyReceiverArgumentClassIds(ic_data, class_ids, class_ids);
}


static void RemovePushArguments(InstanceCallInstr* call) {
  // Remove original push arguments.
  for (intptr_t i = 0; i < call->ArgumentCount(); ++i) {
    PushArgumentInstr* push = call->ArgumentAt(i);
    push->ReplaceUsesWith(push->value()->definition());
    push->RemoveFromGraph();
  }
}


static void RemovePushArguments(StaticCallInstr* call) {
  // Remove original push arguments.
  for (intptr_t i = 0; i < call->ArgumentCount(); ++i) {
    PushArgumentInstr* push = call->ArgumentAt(i);
    push->ReplaceUsesWith(push->value()->definition());
    push->RemoveFromGraph();
  }
}


static intptr_t ReceiverClassId(InstanceCallInstr* call) {
  if (!call->HasICData()) return kIllegalCid;

  const ICData& ic_data = ICData::Handle(call->ic_data()->AsUnaryClassChecks());

  if (ic_data.NumberOfChecks() == 0) return kIllegalCid;
  // TODO(vegorov): Add multiple receiver type support.
  if (ic_data.NumberOfChecks() != 1) return kIllegalCid;
  ASSERT(ic_data.HasOneTarget());

  Function& target = Function::Handle();
  intptr_t class_id;
  ic_data.GetOneClassCheckAt(0, &class_id, &target);
  return class_id;
}


void FlowGraphOptimizer::AddCheckClass(InstanceCallInstr* call,
                                       Value* value) {
  // Type propagation has not run yet, we cannot eliminate the check.
  const ICData& unary_checks =
      ICData::ZoneHandle(call->ic_data()->AsUnaryClassChecks());
  Instruction* check = NULL;
  if ((unary_checks.NumberOfChecks() == 1) &&
      (unary_checks.GetReceiverClassIdAt(0) == kSmiCid)) {
    check = new CheckSmiInstr(value, call->deopt_id());
  } else {
    check = new CheckClassInstr(value, call->deopt_id(), unary_checks);
  }
  InsertBefore(call, check, call->env(), Definition::kEffect);
}


static bool ArgIsAlwaysSmi(const ICData& ic_data, intptr_t arg_n) {
  ASSERT(ic_data.num_args_tested() > arg_n);
  if (ic_data.NumberOfChecks() == 0) return false;
  GrowableArray<intptr_t> class_ids;
  Function& target = Function::Handle();
  const intptr_t len = ic_data.NumberOfChecks();
  for (intptr_t i = 0; i < len; i++) {
    ic_data.GetCheckAt(i, &class_ids, &target);
    if (class_ids[arg_n] != kSmiCid) return false;
  }
  return true;
}


// Returns array classid to load from, array and idnex value

intptr_t FlowGraphOptimizer::PrepareIndexedOp(InstanceCallInstr* call,
                                              intptr_t class_id,
                                              Value** array,
                                              Value** index) {
  *array = call->ArgumentAt(0)->value();
  *index = call->ArgumentAt(1)->value();
  // Insert class check and index smi checks and attach a copy of the
  // original environment because the operation can still deoptimize.
  AddCheckClass(call, (*array)->Copy());
  InsertBefore(call,
               new CheckSmiInstr((*index)->Copy(), call->deopt_id()),
               call->env(),
               Definition::kEffect);
  // If both index and array are constants, then do a compile-time check.
  // TODO(srdjan): Remove once constant propagation handles bounds checks.
  bool skip_check = false;
  if ((*array)->BindsToConstant() && (*index)->BindsToConstant()) {
    ConstantInstr* array_def = (*array)->definition()->AsConstant();
    const ImmutableArray& constant_array =
        ImmutableArray::Cast(array_def->value());
    ConstantInstr* index_def = (*index)->definition()->AsConstant();
    if (index_def->value().IsSmi()) {
      intptr_t constant_index = Smi::Cast(index_def->value()).Value();
      skip_check = (constant_index < constant_array.Length());
    }
  }
  if (!skip_check) {
    // Insert array bounds check.
    InsertBefore(call,
                 new CheckArrayBoundInstr((*array)->Copy(),
                                          (*index)->Copy(),
                                          class_id,
                                          call),
                 call->env(),
                 Definition::kEffect);
  }
  if (class_id == kGrowableObjectArrayCid) {
    // Insert data elements load.
    LoadFieldInstr* elements =
        new LoadFieldInstr((*array)->Copy(),
                           GrowableObjectArray::data_offset(),
                           Type::ZoneHandle(Type::DynamicType()));
    elements->set_result_cid(kArrayCid);
    InsertBefore(call, elements, NULL, Definition::kValue);
    *array = new Value(elements);
    return kArrayCid;
  }
  return class_id;
}


bool FlowGraphOptimizer::TryReplaceWithStoreIndexed(InstanceCallInstr* call) {
  const intptr_t class_id = ReceiverClassId(call);
  ICData& value_check = ICData::ZoneHandle();
  switch (class_id) {
    case kArrayCid:
    case kGrowableObjectArrayCid:
      if (ArgIsAlwaysSmi(*call->ic_data(), 2)) {
        value_check = call->ic_data()->AsUnaryClassChecksForArgNr(2);
      }
      break;
    case kInt8ArrayCid:
    case kUint8ArrayCid:
    case kUint8ClampedArrayCid:
    case kInt16ArrayCid:
    case kUint16ArrayCid:
      // Check that value is always smi.
      value_check = call->ic_data()->AsUnaryClassChecksForArgNr(2);
      if ((value_check.NumberOfChecks() != 1) ||
          (value_check.GetReceiverClassIdAt(0) != kSmiCid)) {
        return false;
      }
      break;
    case kInt32ArrayCid:
    case kUint32ArrayCid: {
      // Check if elements fit into a smi or the platform supports unboxed
      // mints.
      if ((kSmiBits < 32) && !FlowGraphCompiler::SupportsUnboxedMints()) {
        return false;
      }
      // Check that value is always smi or mint, if the platform has unboxed
      // mints (ia32 with at least SSE 4.1).
      value_check = call->ic_data()->AsUnaryClassChecksForArgNr(2);
      for (intptr_t i = 0; i < value_check.NumberOfChecks(); i++) {
        intptr_t cid = value_check.GetReceiverClassIdAt(i);
        if (FlowGraphCompiler::SupportsUnboxedMints()) {
          if ((cid != kSmiCid) && (cid != kMintCid)) {
            return false;
          }
        } else if (cid != kSmiCid) {
          return false;
        }
      }
      break;
    }
    case kFloat32ArrayCid:
    case kFloat64ArrayCid: {
      // Check that value is always double.
      value_check = call->ic_data()->AsUnaryClassChecksForArgNr(2);
      if ((value_check.NumberOfChecks() != 1) ||
          (value_check.GetReceiverClassIdAt(0) != kDoubleCid)) {
        return false;
      }
      break;
    }
    default:
      // TODO(fschneider): Add support for other array types.
      return false;
  }

  if (FLAG_enable_type_checks) {
    Value* array = call->ArgumentAt(0)->value();
    Value* value = call->ArgumentAt(2)->value();
    // Only type check for the value. A type check for the index is not
    // needed here because we insert a deoptimizing smi-check for the case
    // the index is not a smi.
    const Function& target =
        Function::ZoneHandle(call->ic_data()->GetTargetAt(0));
    const AbstractType& value_type =
        AbstractType::ZoneHandle(target.ParameterTypeAt(2));
    Value* instantiator = NULL;
    Value* type_args = NULL;
    switch (class_id) {
      case kArrayCid:
      case kGrowableObjectArrayCid: {
        const Class& instantiator_class = Class::Handle(target.Owner());
        intptr_t type_arguments_field_offset =
            instantiator_class.type_arguments_field_offset();
        LoadFieldInstr* load_type_args =
            new LoadFieldInstr(array->Copy(),
                               type_arguments_field_offset,
                               Type::ZoneHandle());  // No type.
        InsertBefore(call, load_type_args, NULL, Definition::kValue);
        instantiator = array->Copy();
        type_args = new Value(load_type_args);
        break;
      }
      case kInt8ArrayCid:
      case kUint8ArrayCid:
      case kUint8ClampedArrayCid:
      case kInt16ArrayCid:
      case kUint16ArrayCid:
      case kInt32ArrayCid:
      case kUint32ArrayCid:
        ASSERT(value_type.IsIntType());
        // Fall through.
      case kFloat32ArrayCid:
      case kFloat64ArrayCid: {
        instantiator = new Value(flow_graph_->constant_null());
        type_args = new Value(flow_graph_->constant_null());
        ASSERT((class_id != kFloat32ArrayCid && class_id != kFloat64ArrayCid) ||
               value_type.IsDoubleType());
        ASSERT(value_type.IsInstantiated());
        break;
      }
      default:
        // TODO(fschneider): Add support for other array types.
        UNREACHABLE();
    }
    AssertAssignableInstr* assert_value =
        new AssertAssignableInstr(call->token_pos(),
                                  value->Copy(),
                                  instantiator,
                                  type_args,
                                  value_type,
                                  Symbols::Value());
    InsertBefore(call, assert_value, NULL, Definition::kValue);
  }

  Value* array = NULL;
  Value* index = NULL;
  intptr_t array_cid = PrepareIndexedOp(call, class_id, &array, &index);
  Value* value = call->ArgumentAt(2)->value();
  // Check if store barrier is needed.
  bool needs_store_barrier = true;
  if (!value_check.IsNull()) {
    needs_store_barrier = false;
    if (value_check.NumberOfChecks() == 1 &&
        value_check.GetReceiverClassIdAt(0) == kSmiCid) {
      InsertBefore(call,
                   new CheckSmiInstr(value->Copy(), call->deopt_id()),
                   call->env(),
                   Definition::kEffect);
    } else {
      InsertBefore(call,
                   new CheckClassInstr(value->Copy(),
                                       call->deopt_id(),
                                       value_check),
                   call->env(),
                   Definition::kEffect);
    }
  }

  Definition* array_op =
      new StoreIndexedInstr(array, index, value,
                            needs_store_barrier, array_cid, call->deopt_id());
  call->ReplaceWith(array_op, current_iterator());
  RemovePushArguments(call);
  return true;
}



bool FlowGraphOptimizer::TryReplaceWithLoadIndexed(InstanceCallInstr* call) {
  const intptr_t class_id = ReceiverClassId(call);
  switch (class_id) {
    case kArrayCid:
    case kImmutableArrayCid:
    case kGrowableObjectArrayCid:
    case kFloat32ArrayCid:
    case kFloat64ArrayCid:
    case kInt8ArrayCid:
    case kUint8ArrayCid:
    case kUint8ClampedArrayCid:
    case kExternalUint8ArrayCid:
    case kInt16ArrayCid:
    case kUint16ArrayCid:
      break;
    case kInt32ArrayCid:
    case kUint32ArrayCid:
      // Check if elements fit into a smi or the platform supports unboxed
      // mints.
      if ((kSmiBits < 32) && !FlowGraphCompiler::SupportsUnboxedMints()) {
        return false;
      }
      break;
    default:
      return false;
  }
  Value* array = NULL;
  Value* index = NULL;
  intptr_t array_cid = PrepareIndexedOp(call, class_id, &array, &index);
  Definition* array_op = new LoadIndexedInstr(array, index, array_cid);
  call->ReplaceWith(array_op, current_iterator());
  RemovePushArguments(call);
  return true;
}


void FlowGraphOptimizer::InsertBefore(Instruction* next,
                                      Instruction* instr,
                                      Environment* env,
                                      Definition::UseKind use_kind) {
  if (env != NULL) env->DeepCopyTo(instr);
  if (use_kind == Definition::kValue) {
    ASSERT(instr->IsDefinition());
    instr->AsDefinition()->set_ssa_temp_index(
        flow_graph_->alloc_ssa_temp_index());
  }
  instr->InsertBefore(next);
}


void FlowGraphOptimizer::InsertAfter(Instruction* prev,
                                     Instruction* instr,
                                     Environment* env,
                                     Definition::UseKind use_kind) {
  if (env != NULL) env->DeepCopyTo(instr);
  if (use_kind == Definition::kValue) {
    ASSERT(instr->IsDefinition());
    instr->AsDefinition()->set_ssa_temp_index(
        flow_graph_->alloc_ssa_temp_index());
  }
  instr->InsertAfter(prev);
}


bool FlowGraphOptimizer::TryReplaceWithBinaryOp(InstanceCallInstr* call,
                                                Token::Kind op_kind) {
  intptr_t operands_type = kIllegalCid;
  ASSERT(call->HasICData());
  const ICData& ic_data = *call->ic_data();
  switch (op_kind) {
    case Token::kADD:
    case Token::kSUB:
      if (HasOnlyTwoSmis(ic_data)) {
        // Don't generate smi code if the IC data is marked because
        // of an overflow.
        operands_type = (ic_data.deopt_reason() == kDeoptBinarySmiOp)
            ? kMintCid
            : kSmiCid;
      } else if (HasTwoMintOrSmi(ic_data) &&
                 FlowGraphCompiler::SupportsUnboxedMints()) {
        // Don't generate mint code if the IC data is marked because of an
        // overflow.
        if (ic_data.deopt_reason() == kDeoptBinaryMintOp) return false;
        operands_type = kMintCid;
      } else if (ShouldSpecializeForDouble(ic_data)) {
        operands_type = kDoubleCid;
      } else {
        return false;
      }
      break;
    case Token::kMUL:
      if (HasOnlyTwoSmis(ic_data)) {
        // Don't generate smi code if the IC data is marked because of an
        // overflow.
        // TODO(fschneider): Add unboxed mint multiplication.
        if (ic_data.deopt_reason() == kDeoptBinarySmiOp) return false;
        operands_type = kSmiCid;
      } else if (ShouldSpecializeForDouble(ic_data)) {
        operands_type = kDoubleCid;
      } else {
        return false;
      }
      break;
    case Token::kDIV:
      if (ShouldSpecializeForDouble(ic_data)) {
        operands_type = kDoubleCid;
      } else {
        return false;
      }
      break;
    case Token::kMOD:
      if (HasOnlyTwoSmis(ic_data)) {
        operands_type = kSmiCid;
      } else {
        return false;
      }
      break;
    case Token::kBIT_AND:
    case Token::kBIT_OR:
    case Token::kBIT_XOR:
      if (HasOnlyTwoSmis(ic_data)) {
        operands_type = kSmiCid;
      } else if (HasTwoMintOrSmi(ic_data)) {
        operands_type = kMintCid;
      } else {
        return false;
      }
      break;
    case Token::kSHR:
    case Token::kSHL:
      if (HasOnlyTwoSmis(ic_data)) {
        // Left shift may overflow from smi into mint or big ints.
        // Don't generate smi code if the IC data is marked because
        // of an overflow.
        if (ic_data.deopt_reason() == kDeoptShiftMintOp) return false;
        operands_type = (ic_data.deopt_reason() == kDeoptBinarySmiOp)
            ? kMintCid
            : kSmiCid;
      } else if (HasTwoMintOrSmi(ic_data) &&
                 HasOnlyOneSmi(ICData::Handle(
                     ic_data.AsUnaryClassChecksForArgNr(1)))) {
        // Don't generate mint code if the IC data is marked because of an
        // overflow.
        if (ic_data.deopt_reason() == kDeoptShiftMintOp) return false;
        // Check for smi/mint << smi or smi/mint >> smi.
        operands_type = kMintCid;
      } else {
        return false;
      }
      break;
    case Token::kTRUNCDIV:
      if (HasOnlyTwoSmis(ic_data)) {
        if (ic_data.deopt_reason() == kDeoptBinarySmiOp) return false;
        operands_type = kSmiCid;
      } else {
        return false;
      }
      break;
    default:
      UNREACHABLE();
  };

  ASSERT(call->ArgumentCount() == 2);
  if (operands_type == kDoubleCid) {
    Value* left = call->ArgumentAt(0)->value();
    Value* right = call->ArgumentAt(1)->value();

    // Check that either left or right are not a smi.  Result or a
    // binary operation with two smis is a smi not a double.
    InsertBefore(call,
                 new CheckEitherNonSmiInstr(left->Copy(),
                                            right->Copy(),
                                            call),
                 call->env(),
                 Definition::kEffect);

    BinaryDoubleOpInstr* double_bin_op =
        new BinaryDoubleOpInstr(op_kind, left->Copy(), right->Copy(), call);
    call->ReplaceWith(double_bin_op, current_iterator());
    RemovePushArguments(call);
  } else if (operands_type == kMintCid) {
    if (!FlowGraphCompiler::SupportsUnboxedMints()) return false;
    Value* left = call->ArgumentAt(0)->value();
    Value* right = call->ArgumentAt(1)->value();
    if ((op_kind == Token::kSHR) || (op_kind == Token::kSHL)) {
      ShiftMintOpInstr* shift_op =
          new ShiftMintOpInstr(op_kind, left, right, call);
      call->ReplaceWith(shift_op, current_iterator());
    } else {
      BinaryMintOpInstr* bin_op =
          new BinaryMintOpInstr(op_kind, left, right, call);
      call->ReplaceWith(bin_op, current_iterator());
    }
    RemovePushArguments(call);
  } else if (op_kind == Token::kMOD) {
    // TODO(vegorov): implement fast path code for modulo.
    ASSERT(operands_type == kSmiCid);
    if (!call->ArgumentAt(1)->value()->BindsToConstant()) return false;
    const Object& obj = call->ArgumentAt(1)->value()->BoundConstant();
    if (!obj.IsSmi()) return false;
    const intptr_t value = Smi::Cast(obj).Value();
    if ((value > 0) && Utils::IsPowerOfTwo(value)) {
      Value* left = call->ArgumentAt(0)->value();
      // Insert smi check and attach a copy of the original
      // environment because the smi operation can still deoptimize.
      InsertBefore(call,
                   new CheckSmiInstr(left->Copy(), call->deopt_id()),
                   call->env(),
                   Definition::kEffect);
      ConstantInstr* c = new ConstantInstr(Smi::Handle(Smi::New(value - 1)));
      InsertBefore(call, c, NULL, Definition::kValue);
      BinarySmiOpInstr* bin_op =
          new BinarySmiOpInstr(Token::kBIT_AND, call, left, new Value(c));
      call->ReplaceWith(bin_op, current_iterator());
      RemovePushArguments(call);
    } else {
      // Did not replace.
      return false;
    }
  } else {
    ASSERT(operands_type == kSmiCid);
    Value* left = call->ArgumentAt(0)->value();
    Value* right = call->ArgumentAt(1)->value();
    // Insert two smi checks and attach a copy of the original
    // environment because the smi operation can still deoptimize.
    InsertBefore(call,
                 new CheckSmiInstr(left->Copy(), call->deopt_id()),
                 call->env(),
                 Definition::kEffect);
    InsertBefore(call,
                 new CheckSmiInstr(right->Copy(), call->deopt_id()),
                 call->env(),
                 Definition::kEffect);
    BinarySmiOpInstr* bin_op = new BinarySmiOpInstr(op_kind, call, left, right);
    call->ReplaceWith(bin_op, current_iterator());
    RemovePushArguments(call);
  }
  return true;
}


bool FlowGraphOptimizer::TryReplaceWithUnaryOp(InstanceCallInstr* call,
                                               Token::Kind op_kind) {
  ASSERT(call->ArgumentCount() == 1);
  Definition* unary_op = NULL;
  if (HasOnlyOneSmi(*call->ic_data())) {
    Value* value = call->ArgumentAt(0)->value();
    InsertBefore(call,
                 new CheckSmiInstr(value->Copy(), call->deopt_id()),
                 call->env(),
                 Definition::kEffect);
    unary_op = new UnarySmiOpInstr(op_kind, call, value);
  } else if ((op_kind == Token::kBIT_NOT) &&
             HasOnlySmiOrMint(*call->ic_data()) &&
             FlowGraphCompiler::SupportsUnboxedMints()) {
    Value* value = call->ArgumentAt(0)->value();
    unary_op = new UnaryMintOpInstr(op_kind, value, call);
  } else if (HasOnlyOneDouble(*call->ic_data()) &&
             (op_kind == Token::kNEGATE)) {
    Value* value = call->ArgumentAt(0)->value();
    AddCheckClass(call, value->Copy());
    ConstantInstr* minus_one =
        new ConstantInstr(Double::ZoneHandle(Double::NewCanonical(-1)));
    InsertBefore(call, minus_one, NULL, Definition::kValue);
    unary_op = new BinaryDoubleOpInstr(Token::kMUL,
                                       value,
                                       new Value(minus_one),
                                       call);
  }
  if (unary_op == NULL) return false;

  call->ReplaceWith(unary_op, current_iterator());
  RemovePushArguments(call);
  return true;
}


// Using field class
static RawField* GetField(intptr_t class_id, const String& field_name) {
  Class& cls = Class::Handle(Isolate::Current()->class_table()->At(class_id));
  Field& field = Field::Handle();
  while (!cls.IsNull()) {
    field = cls.LookupInstanceField(field_name);
    if (!field.IsNull()) {
      return field.raw();
    }
    cls = cls.SuperClass();
  }
  return Field::null();
}


// Use CHA to determine if the call needs a class check: if the callee's
// receiver is the same as the caller's receiver and there are no overriden
// callee functions, then no class check is needed.
bool FlowGraphOptimizer::InstanceCallNeedsClassCheck(
    InstanceCallInstr* call) const {
  if (!FLAG_use_cha) return true;
  Definition* callee_receiver = call->ArgumentAt(0)->value()->definition();
  ASSERT(callee_receiver != NULL);
  const Function& function = flow_graph_->parsed_function().function();
  if (function.IsDynamicFunction() &&
      callee_receiver->IsParameter() &&
      (callee_receiver->AsParameter()->index() == 0)) {
    return CHA::HasOverride(Class::Handle(function.Owner()),
                            call->function_name());
  }
  return true;
}


bool FlowGraphOptimizer::MethodExtractorNeedsClassCheck(
    InstanceCallInstr* call) const {
  if (!FLAG_use_cha) return true;
  Definition* callee_receiver = call->ArgumentAt(0)->value()->definition();
  ASSERT(callee_receiver != NULL);
  const Function& function = flow_graph_->parsed_function().function();
  if (function.IsDynamicFunction() &&
      callee_receiver->IsParameter() &&
      (callee_receiver->AsParameter()->index() == 0)) {
    const String& field_name =
      String::Handle(Field::NameFromGetter(call->function_name()));
    return CHA::HasOverride(Class::Handle(function.Owner()), field_name);
  }
  return true;
}


void FlowGraphOptimizer::InlineImplicitInstanceGetter(InstanceCallInstr* call) {
  ASSERT(call->HasICData());
  const ICData& ic_data = *call->ic_data();
  Function& target = Function::Handle();
  GrowableArray<intptr_t> class_ids;
  ic_data.GetCheckAt(0, &class_ids, &target);
  ASSERT(class_ids.length() == 1);
  // Inline implicit instance getter.
  const String& field_name =
      String::Handle(Field::NameFromGetter(call->function_name()));
  const Field& field = Field::Handle(GetField(class_ids[0], field_name));
  ASSERT(!field.IsNull());

  if (InstanceCallNeedsClassCheck(call)) {
    AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());
  }
  // Detach environment from the original instruction because it can't
  // deoptimize.
  call->set_env(NULL);
  LoadFieldInstr* load = new LoadFieldInstr(
      call->ArgumentAt(0)->value(),
      field.Offset(),
      AbstractType::ZoneHandle(field.type()),
      field.is_final());
  call->ReplaceWith(load, current_iterator());
  RemovePushArguments(call);
}


void FlowGraphOptimizer::InlineArrayLengthGetter(InstanceCallInstr* call,
                                                 intptr_t length_offset,
                                                 bool is_immutable,
                                                 MethodRecognizer::Kind kind) {
  // Check receiver class.
  AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());

  LoadFieldInstr* load = new LoadFieldInstr(
      call->ArgumentAt(0)->value(),
      length_offset,
      Type::ZoneHandle(Type::SmiType()),
      is_immutable);
  load->set_result_cid(kSmiCid);
  load->set_recognized_kind(kind);
  call->ReplaceWith(load, current_iterator());
  RemovePushArguments(call);
}


void FlowGraphOptimizer::InlineGrowableArrayCapacityGetter(
    InstanceCallInstr* call) {
  // Check receiver class.
  AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());

  // TODO(srdjan): type of load should be GrowableObjectArrayType.
  LoadFieldInstr* data_load = new LoadFieldInstr(
      call->ArgumentAt(0)->value(),
      Array::data_offset(),
      Type::ZoneHandle(Type::DynamicType()));
  data_load->set_result_cid(kArrayCid);
  InsertBefore(call, data_load, NULL, Definition::kValue);

  LoadFieldInstr* length_load = new LoadFieldInstr(
      new Value(data_load),
      Array::length_offset(),
      Type::ZoneHandle(Type::SmiType()));
  length_load->set_result_cid(kSmiCid);
  length_load->set_recognized_kind(MethodRecognizer::kObjectArrayLength);

  call->ReplaceWith(length_load, current_iterator());
  RemovePushArguments(call);
}


static LoadFieldInstr* BuildLoadStringLength(Value* str) {
  const bool is_immutable = true;  // String length is immutable.
  LoadFieldInstr* load = new LoadFieldInstr(
      str,
      String::length_offset(),
      Type::ZoneHandle(Type::SmiType()),
      is_immutable);
  load->set_result_cid(kSmiCid);
  return load;
}


void FlowGraphOptimizer::InlineStringLengthGetter(InstanceCallInstr* call) {
  // Check receiver class.
  AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());

  LoadFieldInstr* load = BuildLoadStringLength(call->ArgumentAt(0)->value());
  load->set_recognized_kind(MethodRecognizer::kStringBaseLength);
  call->ReplaceWith(load, current_iterator());
  RemovePushArguments(call);
}


void FlowGraphOptimizer::InlineStringIsEmptyGetter(InstanceCallInstr* call) {
  // Check receiver class.
  AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());

  LoadFieldInstr* load = BuildLoadStringLength(call->ArgumentAt(0)->value());
  InsertBefore(call, load, NULL, Definition::kValue);

  ConstantInstr* zero = new ConstantInstr(Smi::Handle(Smi::New(0)));
  InsertBefore(call, zero, NULL, Definition::kValue);

  StrictCompareInstr* compare =
      new StrictCompareInstr(Token::kEQ_STRICT,
                             new Value(load),
                             new Value(zero));
  call->ReplaceWith(compare, current_iterator());
  RemovePushArguments(call);
}


static intptr_t OffsetForLengthGetter(MethodRecognizer::Kind kind) {
  switch (kind) {
    case MethodRecognizer::kObjectArrayLength:
    case MethodRecognizer::kImmutableArrayLength:
      return Array::length_offset();
    case MethodRecognizer::kByteArrayBaseLength:
      return ByteArray::length_offset();
    case MethodRecognizer::kGrowableArrayLength:
      return GrowableObjectArray::length_offset();
    default:
      UNREACHABLE();
      return 0;
  }
}


// Only unique implicit instance getters can be currently handled.
bool FlowGraphOptimizer::TryInlineInstanceGetter(InstanceCallInstr* call) {
  ASSERT(call->HasICData());
  const ICData& ic_data = *call->ic_data();
  if (ic_data.NumberOfChecks() == 0) {
    // No type feedback collected.
    return false;
  }
  Function& target = Function::Handle(ic_data.GetTargetAt(0));
  if (target.kind() == RawFunction::kImplicitGetter) {
    if (!ic_data.HasOneTarget()) {
      // TODO(srdjan): Implement for mutiple targets.
      return false;
    }
    InlineImplicitInstanceGetter(call);
    return true;
  } else if (target.kind() == RawFunction::kMethodExtractor) {
    return false;
  }

  // Not an implicit getter.
  MethodRecognizer::Kind recognized_kind =
      MethodRecognizer::RecognizeKind(target);

  // VM objects length getter.
  switch (recognized_kind) {
    case MethodRecognizer::kObjectArrayLength:
    case MethodRecognizer::kImmutableArrayLength:
    case MethodRecognizer::kByteArrayBaseLength:
    case MethodRecognizer::kGrowableArrayLength: {
      if (!ic_data.HasOneTarget()) {
        // TODO(srdjan): Implement for mutiple targets.
        return false;
      }
      const bool is_immutable =
          (recognized_kind != MethodRecognizer::kGrowableArrayLength);
      InlineArrayLengthGetter(call,
                              OffsetForLengthGetter(recognized_kind),
                              is_immutable,
                              recognized_kind);
      return true;
    }
    case MethodRecognizer::kGrowableArrayCapacity:
      InlineGrowableArrayCapacityGetter(call);
      return true;
    case MethodRecognizer::kStringBaseLength:
      if (!ic_data.HasOneTarget()) {
        // Target is not only StringBase_get_length.
        return false;
      }
      InlineStringLengthGetter(call);
      return true;
    case MethodRecognizer::kStringBaseIsEmpty:
      if (!ic_data.HasOneTarget()) {
        // Target is not only StringBase_get_isEmpty.
        return false;
      }
      InlineStringIsEmptyGetter(call);
      return true;
    default:
      ASSERT(recognized_kind == MethodRecognizer::kUnknown);
  }
  return false;
}


LoadIndexedInstr* FlowGraphOptimizer::BuildStringCharCodeAt(
    InstanceCallInstr* call,
    intptr_t cid) {
  Value* str = call->ArgumentAt(0)->value();
  Value* index = call->ArgumentAt(1)->value();
  AddCheckClass(call, str->Copy());
  InsertBefore(call,
               new CheckSmiInstr(index->Copy(), call->deopt_id()),
               call->env(),
               Definition::kEffect);
  // If both index and string are constants, then do a compile-time check.
  // TODO(srdjan): Remove once constant propagation handles bounds checks.
  bool skip_check = false;
  if (str->BindsToConstant() && index->BindsToConstant()) {
    ConstantInstr* string_def = str->definition()->AsConstant();
    const String& constant_string =
        String::Cast(string_def->value());
    ConstantInstr* index_def = index->definition()->AsConstant();
    if (index_def->value().IsSmi()) {
      intptr_t constant_index = Smi::Cast(index_def->value()).Value();
      skip_check = (constant_index < constant_string.Length());
    }
  }
  if (!skip_check) {
    // Insert bounds check.
    InsertBefore(call,
                 new CheckArrayBoundInstr(str->Copy(),
                                          index->Copy(),
                                          cid,
                                          call),
                 call->env(),
                 Definition::kEffect);
  }
  return new LoadIndexedInstr(str, index, cid);
}


void FlowGraphOptimizer::ReplaceWithMathCFunction(
  InstanceCallInstr* call,
  MethodRecognizer::Kind recognized_kind) {
  AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());
  ZoneGrowableArray<Value*>* args =
      new ZoneGrowableArray<Value*>(call->ArgumentCount());
  for (intptr_t i = 0; i < call->ArgumentCount(); i++) {
    args->Add(call->ArgumentAt(i)->value());
  }
  InvokeMathCFunctionInstr* invoke =
      new InvokeMathCFunctionInstr(args, call, recognized_kind);
  call->ReplaceWith(invoke, current_iterator());
  RemovePushArguments(call);
}


// Inline only simple, frequently called core library methods.
bool FlowGraphOptimizer::TryInlineInstanceMethod(InstanceCallInstr* call) {
  ASSERT(call->HasICData());
  const ICData& ic_data = *call->ic_data();
  if ((ic_data.NumberOfChecks() == 0) || !ic_data.HasOneTarget()) {
    // No type feedback collected or multiple targets found.
    return false;
  }
  Function& target = Function::Handle();
  GrowableArray<intptr_t> class_ids;
  ic_data.GetCheckAt(0, &class_ids, &target);
  MethodRecognizer::Kind recognized_kind =
      MethodRecognizer::RecognizeKind(target);
  if ((recognized_kind == MethodRecognizer::kStringBaseCharCodeAt) &&
      (ic_data.NumberOfChecks() == 1) &&
      ((class_ids[0] == kOneByteStringCid) ||
       (class_ids[0] == kTwoByteStringCid))) {
    LoadIndexedInstr* instr = BuildStringCharCodeAt(call, class_ids[0]);
    call->ReplaceWith(instr, current_iterator());
    RemovePushArguments(call);
    return true;
  }
  if ((recognized_kind == MethodRecognizer::kStringBaseCharAt) &&
      (ic_data.NumberOfChecks() == 1) &&
      (class_ids[0] == kOneByteStringCid)) {
    // TODO(fschneider): Handle TwoByteString.
    LoadIndexedInstr* load_char_code =
        BuildStringCharCodeAt(call, class_ids[0]);
    InsertBefore(call, load_char_code, NULL, Definition::kValue);
    StringFromCharCodeInstr* char_at =
        new StringFromCharCodeInstr(new Value(load_char_code),
                                    kOneByteStringCid);
    call->ReplaceWith(char_at, current_iterator());
    RemovePushArguments(call);
    return true;
  }

  if ((recognized_kind == MethodRecognizer::kIntegerToDouble) &&
      (class_ids[0] == kSmiCid)) {
    SmiToDoubleInstr* s2d_instr = new SmiToDoubleInstr(call);
    call->ReplaceWith(s2d_instr, current_iterator());
    // Pushed arguments are not removed because SmiToDouble is implemented
    // as a call.
    return true;
  }

  if (class_ids[0] == kDoubleCid) {
    switch (recognized_kind) {
      case MethodRecognizer::kDoubleToInteger: {
        AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());
        ASSERT(call->HasICData());
        const ICData& ic_data = *call->ic_data();
        Definition* d2i_instr = NULL;
        if (ic_data.deopt_reason() == kDeoptDoubleToSmi) {
          // Do not repeatedly deoptimize because result didn't fit into Smi.
          d2i_instr = new DoubleToIntegerInstr(call->ArgumentAt(0)->value(),
                                               call);
        } else {
          // Optimistically assume result fits into Smi.
          d2i_instr = new DoubleToSmiInstr(call->ArgumentAt(0)->value(), call);
        }
        call->ReplaceWith(d2i_instr, current_iterator());
        RemovePushArguments(call);
        return true;
      }
      case MethodRecognizer::kDoubleMod:
      case MethodRecognizer::kDoublePow:
        ReplaceWithMathCFunction(call, recognized_kind);
        return true;
      case MethodRecognizer::kDoubleTruncate:
      case MethodRecognizer::kDoubleRound:
      case MethodRecognizer::kDoubleFloor:
      case MethodRecognizer::kDoubleCeil:
        if (!CPUFeatures::double_truncate_round_supported()) {
          ReplaceWithMathCFunction(call, recognized_kind);
        } else {
          AddCheckClass(call, call->ArgumentAt(0)->value()->Copy());
          DoubleToDoubleInstr* d2d_instr =
              new DoubleToDoubleInstr(call->ArgumentAt(0)->value(),
                                      call,
                                      recognized_kind);
          call->ReplaceWith(d2d_instr, current_iterator());
          RemovePushArguments(call);
        }
        return true;
      default:
        // Unsupported method.
        return false;
    }
  }

  return false;
}


// Returns a Boolean constant if all classes in ic_data yield the same type-test
// result and the type tests do not depend on type arguments. Otherwise return
// Bool::null().
RawBool* FlowGraphOptimizer::InstanceOfAsBool(const ICData& ic_data,
                                              const AbstractType& type) const {
  ASSERT(ic_data.num_args_tested() == 1);  // Unary checks only.
  if (!type.IsInstantiated() || type.IsMalformed()) return Bool::null();
  const Class& type_class = Class::Handle(type.type_class());
  if (type_class.HasTypeArguments()) return Bool::null();
  const ClassTable& class_table = *Isolate::Current()->class_table();
  Bool& prev = Bool::Handle();
  Class& cls = Class::Handle();
  for (int i = 0; i < ic_data.NumberOfChecks(); i++) {
    cls = class_table.At(ic_data.GetReceiverClassIdAt(i));
    if (cls.HasTypeArguments()) return Bool::null();
    const bool is_subtype = cls.IsSubtypeOf(TypeArguments::Handle(),
                                            type_class,
                                            TypeArguments::Handle(),
                                            NULL);
    if (prev.IsNull()) {
      prev = is_subtype ? Bool::True().raw() : Bool::False().raw();
    } else {
      if (is_subtype != prev.value()) return Bool::null();
    }
  }
  return prev.raw();
}


// TODO(srdjan): Use ICData to check if always true or false.
void FlowGraphOptimizer::ReplaceWithInstanceOf(InstanceCallInstr* call) {
  ASSERT(Token::IsTypeTestOperator(call->token_kind()));
  Value* left_val = call->ArgumentAt(0)->value();
  Value* instantiator_val = call->ArgumentAt(1)->value();
  Value* type_args_val = call->ArgumentAt(2)->value();
  const AbstractType& type =
      AbstractType::Cast(call->ArgumentAt(3)->value()->BoundConstant());
  const bool negate =
      Bool::Cast(call->ArgumentAt(4)->value()->BoundConstant()).value();
  const ICData& unary_checks =
      ICData::ZoneHandle(call->ic_data()->AsUnaryClassChecks());
  if (unary_checks.NumberOfChecks() <= FLAG_max_polymorphic_checks) {
    Bool& as_bool = Bool::ZoneHandle(InstanceOfAsBool(unary_checks, type));
    if (!as_bool.IsNull()) {
      AddCheckClass(call, left_val->Copy());
      if (negate) {
        as_bool = as_bool.value() ? Bool::False().raw() : Bool::True().raw();
      }
      ConstantInstr* bool_const = new ConstantInstr(as_bool);
      call->ReplaceWith(bool_const, current_iterator());
      RemovePushArguments(call);
      return;
    }
  }
  InstanceOfInstr* instance_of =
      new InstanceOfInstr(call->token_pos(),
                          left_val,
                          instantiator_val,
                          type_args_val,
                          type,
                          negate);
  call->ReplaceWith(instance_of, current_iterator());
  RemovePushArguments(call);
}


// Tries to optimize instance call by replacing it with a faster instruction
// (e.g, binary op, field load, ..).
void FlowGraphOptimizer::VisitInstanceCall(InstanceCallInstr* instr) {
  if (!instr->HasICData() || (instr->ic_data()->NumberOfChecks() == 0)) {
    // An instance call without ICData will trigger deoptimization.
    return;
  }

  const Token::Kind op_kind = instr->token_kind();
  // Type test is special as it always gets converted into inlined code.
  if (Token::IsTypeTestOperator(op_kind)) {
    ReplaceWithInstanceOf(instr);
    return;
  }

  const ICData& unary_checks =
      ICData::ZoneHandle(instr->ic_data()->AsUnaryClassChecks());

  if ((unary_checks.NumberOfChecks() > FLAG_max_polymorphic_checks) &&
      InstanceCallNeedsClassCheck(instr)) {
    // Too many checks, it will be megamorphic which needs unary checks.
    instr->set_ic_data(&unary_checks);
    return;
  }

  if ((op_kind == Token::kASSIGN_INDEX) &&
      TryReplaceWithStoreIndexed(instr)) {
    return;
  }
  if ((op_kind == Token::kINDEX) && TryReplaceWithLoadIndexed(instr)) {
    return;
  }
  if (Token::IsBinaryOperator(op_kind) &&
      TryReplaceWithBinaryOp(instr, op_kind)) {
    return;
  }
  if (Token::IsPrefixOperator(op_kind) &&
      TryReplaceWithUnaryOp(instr, op_kind)) {
    return;
  }
  if ((op_kind == Token::kGET) && TryInlineInstanceGetter(instr)) {
    return;
  }
  if ((op_kind == Token::kSET) &&
      TryInlineInstanceSetter(instr, unary_checks)) {
    return;
  }
  if (TryInlineInstanceMethod(instr)) {
    return;
  }

  const bool has_one_target = unary_checks.HasOneTarget();

  if (has_one_target) {
    const bool is_method_extraction =
        Function::Handle(unary_checks.GetTargetAt(0)).IsMethodExtractor();

    if ((is_method_extraction && !MethodExtractorNeedsClassCheck(instr)) ||
        (!is_method_extraction && !InstanceCallNeedsClassCheck(instr))) {
      const bool call_with_checks = false;
      PolymorphicInstanceCallInstr* call =
          new PolymorphicInstanceCallInstr(instr, unary_checks,
                                           call_with_checks);
      instr->ReplaceWith(call, current_iterator());
      return;
    }
  }

  if (unary_checks.NumberOfChecks() <= FLAG_max_polymorphic_checks) {
    bool call_with_checks;
    if (has_one_target) {
      // Type propagation has not run yet, we cannot eliminate the check.
      AddCheckClass(instr, instr->ArgumentAt(0)->value()->Copy());
      // Call can still deoptimize, do not detach environment from instr.
      call_with_checks = false;
    } else {
      call_with_checks = true;
    }
    PolymorphicInstanceCallInstr* call =
        new PolymorphicInstanceCallInstr(instr, unary_checks,
                                         call_with_checks);
    instr->ReplaceWith(call, current_iterator());
  }
}


void FlowGraphOptimizer::VisitStaticCall(StaticCallInstr* call) {
  MethodRecognizer::Kind recognized_kind =
      MethodRecognizer::RecognizeKind(call->function());
  if (recognized_kind == MethodRecognizer::kMathSqrt) {
    MathSqrtInstr* sqrt = new MathSqrtInstr(call->ArgumentAt(0)->value(), call);
    call->ReplaceWith(sqrt, current_iterator());
    RemovePushArguments(call);
  }
}


bool FlowGraphOptimizer::TryInlineInstanceSetter(InstanceCallInstr* instr,
                                                 const ICData& unary_ic_data) {
  ASSERT((unary_ic_data.NumberOfChecks() > 0) &&
      (unary_ic_data.num_args_tested() == 1));
  if (FLAG_enable_type_checks) {
    // TODO(srdjan): Add assignable check node if --enable_type_checks.
    return false;
  }

  ASSERT(instr->HasICData());
  if (unary_ic_data.NumberOfChecks() == 0) {
    // No type feedback collected.
    return false;
  }
  if (!unary_ic_data.HasOneTarget()) {
    // TODO(srdjan): Implement when not all targets are the same.
    return false;
  }
  Function& target = Function::Handle();
  intptr_t class_id;
  unary_ic_data.GetOneClassCheckAt(0, &class_id, &target);
  if (target.kind() != RawFunction::kImplicitSetter) {
    // Not an implicit setter.
    // TODO(srdjan): Inline special setters.
    return false;
  }
  // Inline implicit instance setter.
  const String& field_name =
      String::Handle(Field::NameFromSetter(instr->function_name()));
  const Field& field = Field::Handle(GetField(class_id, field_name));
  ASSERT(!field.IsNull());

  if (InstanceCallNeedsClassCheck(instr)) {
    AddCheckClass(instr, instr->ArgumentAt(0)->value()->Copy());
  }
  bool needs_store_barrier = true;
  if (ArgIsAlwaysSmi(*instr->ic_data(), 1)) {
    InsertBefore(instr,
                 new CheckSmiInstr(instr->ArgumentAt(1)->value()->Copy(),
                                   instr->deopt_id()),
                 instr->env(),
                 Definition::kEffect);
    needs_store_barrier = false;
  }
  // Detach environment from the original instruction because it can't
  // deoptimize.
  instr->set_env(NULL);
  StoreInstanceFieldInstr* store = new StoreInstanceFieldInstr(
      field,
      instr->ArgumentAt(0)->value(),
      instr->ArgumentAt(1)->value(),
      needs_store_barrier);
  instr->ReplaceWith(store, current_iterator());
  RemovePushArguments(instr);
  return true;
}


static void HandleRelationalOp(FlowGraphOptimizer* optimizer,
                               RelationalOpInstr* comp,
                               Instruction* instr) {
  if (!comp->HasICData() || (comp->ic_data()->NumberOfChecks() == 0)) {
    return;
  }
  const ICData& ic_data = *comp->ic_data();
  if (ic_data.NumberOfChecks() == 1) {
    ASSERT(ic_data.HasOneTarget());
    if (HasOnlyTwoSmis(ic_data)) {
      optimizer->InsertBefore(
          instr,
          new CheckSmiInstr(comp->left()->Copy(), comp->deopt_id()),
          instr->env(),
          Definition::kEffect);
      optimizer->InsertBefore(
          instr,
          new CheckSmiInstr(comp->right()->Copy(), comp->deopt_id()),
          instr->env(),
          Definition::kEffect);
      comp->set_operands_class_id(kSmiCid);
    } else if (ShouldSpecializeForDouble(ic_data)) {
      comp->set_operands_class_id(kDoubleCid);
    } else if (HasTwoMintOrSmi(*comp->ic_data()) &&
               FlowGraphCompiler::SupportsUnboxedMints()) {
      comp->set_operands_class_id(kMintCid);
    } else {
      ASSERT(comp->operands_class_id() == kIllegalCid);
    }
  } else if (HasTwoMintOrSmi(*comp->ic_data()) &&
             FlowGraphCompiler::SupportsUnboxedMints()) {
    comp->set_operands_class_id(kMintCid);
  }
}


void FlowGraphOptimizer::VisitRelationalOp(RelationalOpInstr* instr) {
  HandleRelationalOp(this, instr, instr);
}


template <typename T>
static void HandleEqualityCompare(FlowGraphOptimizer* optimizer,
                                  EqualityCompareInstr* comp,
                                  T instr,
                                  ForwardInstructionIterator* iterator) {
  // If one of the inputs is null, no ICdata will be collected.
  if (comp->left()->BindsToConstantNull() ||
      comp->right()->BindsToConstantNull()) {
    Token::Kind strict_kind = (comp->kind() == Token::kEQ) ?
        Token::kEQ_STRICT : Token::kNE_STRICT;
    StrictCompareInstr* strict_comp =
        new StrictCompareInstr(strict_kind, comp->left(), comp->right());
    instr->ReplaceWith(strict_comp, iterator);
    return;
  }
  if (!comp->HasICData() || (comp->ic_data()->NumberOfChecks() == 0)) {
    return;
  }
  ASSERT(comp->ic_data()->num_args_tested() == 2);
  if (comp->ic_data()->NumberOfChecks() == 1) {
    GrowableArray<intptr_t> class_ids;
    Function& target = Function::Handle();
    comp->ic_data()->GetCheckAt(0, &class_ids, &target);
    // TODO(srdjan): allow for mixed mode int/double comparison.

    if ((class_ids[0] == kSmiCid) && (class_ids[1] == kSmiCid)) {
      optimizer->InsertBefore(
          instr,
          new CheckSmiInstr(comp->left()->Copy(), comp->deopt_id()),
          instr->env(),
          Definition::kEffect);
      optimizer->InsertBefore(
          instr,
          new CheckSmiInstr(comp->right()->Copy(), comp->deopt_id()),
          instr->env(),
          Definition::kEffect);
      comp->set_receiver_class_id(kSmiCid);
    } else if ((class_ids[0] == kDoubleCid) && (class_ids[1] == kDoubleCid)) {
      comp->set_receiver_class_id(kDoubleCid);
    } else if (HasTwoMintOrSmi(*comp->ic_data()) &&
               FlowGraphCompiler::SupportsUnboxedMints()) {
      comp->set_receiver_class_id(kMintCid);
    } else {
      ASSERT(comp->receiver_class_id() == kIllegalCid);
    }
  } else if (HasTwoMintOrSmi(*comp->ic_data()) &&
             FlowGraphCompiler::SupportsUnboxedMints()) {
    comp->set_receiver_class_id(kMintCid);
  }

  if (comp->receiver_class_id() != kIllegalCid) {
    // Done.
    return;
  }

  // Check if ICDData contains checks with Smi/Null combinations. In that case
  // we can still emit the optimized Smi equality operation but need to add
  // checks for null or Smi.
  // TODO(srdjan): Add it for Double and Mint.
  GrowableArray<intptr_t> smi_or_null(2);
  smi_or_null.Add(kSmiCid);
  smi_or_null.Add(kNullCid);
  if (ICDataHasOnlyReceiverArgumentClassIds(
        *comp->ic_data(), smi_or_null, smi_or_null)) {
    const ICData& unary_checks_0 =
        ICData::ZoneHandle(comp->ic_data()->AsUnaryClassChecks());
    const intptr_t deopt_id = comp->deopt_id();
    if ((unary_checks_0.NumberOfChecks() == 1) &&
        (unary_checks_0.GetReceiverClassIdAt(0) == kSmiCid)) {
      // Smi only.
      optimizer->InsertBefore(
        instr,
        new CheckSmiInstr(comp->left()->Copy(), deopt_id),
        instr->env(),
        Definition::kEffect);
    } else {
      // Smi or NULL.
      optimizer->InsertBefore(
        instr,
        new CheckClassInstr(comp->left()->Copy(), deopt_id, unary_checks_0),
        instr->env(),
        Definition::kEffect);
    }

    const ICData& unary_checks_1 =
        ICData::ZoneHandle(comp->ic_data()->AsUnaryClassChecksForArgNr(1));
    if ((unary_checks_1.NumberOfChecks() == 1) &&
        (unary_checks_1.GetReceiverClassIdAt(0) == kSmiCid)) {
      // Smi only.
      optimizer->InsertBefore(
        instr,
        new CheckSmiInstr(comp->right()->Copy(), deopt_id),
        instr->env(),
        Definition::kEffect);
    } else {
      // Smi or NULL.
      optimizer->InsertBefore(
        instr,
        new CheckClassInstr(comp->right()->Copy(), deopt_id, unary_checks_1),
        instr->env(),
        Definition::kEffect);
    }
    comp->set_receiver_class_id(kSmiCid);
  }
}


void FlowGraphOptimizer::VisitEqualityCompare(EqualityCompareInstr* instr) {
  HandleEqualityCompare(this, instr, instr, current_iterator());
}


void FlowGraphOptimizer::VisitBranch(BranchInstr* instr) {
  ComparisonInstr* comparison = instr->comparison();
  if (comparison->IsRelationalOp()) {
    HandleRelationalOp(this, comparison->AsRelationalOp(), instr);
  } else if (comparison->IsEqualityCompare()) {
    HandleEqualityCompare(this, comparison->AsEqualityCompare(), instr,
                          current_iterator());
  } else {
    ASSERT(comparison->IsStrictCompare());
    // Nothing to do.
  }
}


static bool MayBeBoxableNumber(intptr_t cid) {
  return (cid == kDynamicCid) ||
         (cid == kMintCid) ||
         (cid == kBigintCid) ||
         (cid == kDoubleCid);
}


// Check if number check is not needed.
void FlowGraphOptimizer::VisitStrictCompare(StrictCompareInstr* instr) {
  if (!instr->needs_number_check()) return;

  // If one of the input is not a boxable number (Mint, Double, Bigint), no
  // need for number checks.
  if (!MayBeBoxableNumber(instr->left()->ResultCid()) ||
      !MayBeBoxableNumber(instr->right()->ResultCid()))  {
    instr->set_needs_number_check(false);
  }
}


// SminessPropagator ensures that CheckSmis are eliminated across phis.
class SminessPropagator : public ValueObject {
 public:
  explicit SminessPropagator(FlowGraph* flow_graph)
      : flow_graph_(flow_graph),
        known_smis_(new BitVector(flow_graph_->current_ssa_temp_index())),
        rollback_checks_(10),
        in_worklist_(NULL),
        worklist_(0) { }

  void Propagate();

 private:
  void PropagateSminessRecursive(BlockEntryInstr* block);
  void AddToWorklist(PhiInstr* phi);
  PhiInstr* RemoveLastFromWorklist();
  void ProcessPhis();

  FlowGraph* flow_graph_;

  BitVector* known_smis_;
  GrowableArray<intptr_t> rollback_checks_;

  BitVector* in_worklist_;
  GrowableArray<PhiInstr*> worklist_;

  DISALLOW_COPY_AND_ASSIGN(SminessPropagator);
};


void SminessPropagator::AddToWorklist(PhiInstr* phi) {
  if (in_worklist_ == NULL) {
    in_worklist_ = new BitVector(flow_graph_->current_ssa_temp_index());
  }
  if (!in_worklist_->Contains(phi->ssa_temp_index())) {
    in_worklist_->Add(phi->ssa_temp_index());
    worklist_.Add(phi);
  }
}


PhiInstr* SminessPropagator::RemoveLastFromWorklist() {
  PhiInstr* phi = worklist_.RemoveLast();
  ASSERT(in_worklist_->Contains(phi->ssa_temp_index()));
  in_worklist_->Remove(phi->ssa_temp_index());
  return phi;
}


static bool IsDefinitelySmiPhi(PhiInstr* phi) {
  for (intptr_t i = 0; i < phi->InputCount(); i++) {
    const intptr_t cid = phi->InputAt(i)->ResultCid();
    if (cid != kSmiCid) {
      return false;
    }
  }
  return true;
}


static bool IsPossiblySmiPhi(PhiInstr* phi) {
  for (intptr_t i = 0; i < phi->InputCount(); i++) {
    const intptr_t cid = phi->InputAt(i)->ResultCid();
    if ((cid != kSmiCid) && (cid != kDynamicCid)) {
      return false;
    }
  }
  return true;
}


void SminessPropagator::ProcessPhis() {
  // First optimistically mark all possible smi-phis: phi is possibly a smi if
  // its operands are either smis or phis in the worklist.
  for (intptr_t i = 0; i < worklist_.length(); i++) {
    PhiInstr* phi = worklist_[i];
    ASSERT(phi->GetPropagatedCid() == kDynamicCid);
    phi->SetPropagatedCid(kSmiCid);

    // Append all phis that use this phi and can potentially be smi to the
    // end of worklist.
    for (Value* use = phi->input_use_list();
         use != NULL;
         use = use->next_use()) {
      PhiInstr* phi_use = use->instruction()->AsPhi();
      if ((phi_use != NULL) &&
          (phi_use->GetPropagatedCid() == kDynamicCid) &&
          IsPossiblySmiPhi(phi_use)) {
        AddToWorklist(phi_use);
      }
    }
  }

  // Now unmark phis that are not definitely smi: that is have only
  // smi operands.
  while (!worklist_.is_empty()) {
    PhiInstr* phi = RemoveLastFromWorklist();
    if (!IsDefinitelySmiPhi(phi)) {
      // Phi result is not a smi. Propagate this fact to phis that depend on it.
      phi->SetPropagatedCid(kDynamicCid);
      for (Value* use = phi->input_use_list();
           use != NULL;
           use = use->next_use()) {
        PhiInstr* phi_use = use->instruction()->AsPhi();
        if ((phi_use != NULL) && (phi_use->GetPropagatedCid() == kSmiCid)) {
          AddToWorklist(phi_use);
        }
      }
    }
  }
}


void SminessPropagator::PropagateSminessRecursive(BlockEntryInstr* block) {
  const intptr_t rollback_point = rollback_checks_.length();

  for (ForwardInstructionIterator it(block); !it.Done(); it.Advance()) {
    Instruction* instr = it.Current();
    if (instr->IsCheckSmi()) {
      const intptr_t value_ssa_index =
          instr->InputAt(0)->definition()->ssa_temp_index();
      if (!known_smis_->Contains(value_ssa_index)) {
        known_smis_->Add(value_ssa_index);
        rollback_checks_.Add(value_ssa_index);
      }
    } else if (instr->IsBranch()) {
      for (intptr_t i = 0; i < instr->InputCount(); i++) {
        Value* use = instr->InputAt(i);
        if (known_smis_->Contains(use->definition()->ssa_temp_index())) {
          use->set_reaching_cid(kSmiCid);
        }
      }
    }
  }

  for (intptr_t i = 0; i < block->dominated_blocks().length(); ++i) {
    PropagateSminessRecursive(block->dominated_blocks()[i]);
  }

  if (block->last_instruction()->SuccessorCount() == 1 &&
      block->last_instruction()->SuccessorAt(0)->IsJoinEntry()) {
    JoinEntryInstr* join =
        block->last_instruction()->SuccessorAt(0)->AsJoinEntry();
    intptr_t pred_index = join->IndexOfPredecessor(block);
    ASSERT(pred_index >= 0);
    if (join->phis() != NULL) {
      for (intptr_t i = 0; i < join->phis()->length(); ++i) {
        PhiInstr* phi = (*join->phis())[i];
        if (phi == NULL) continue;
        Value* use = phi->InputAt(pred_index);
        const intptr_t value_ssa_index = use->definition()->ssa_temp_index();
        if (known_smis_->Contains(value_ssa_index) &&
            (phi->GetPropagatedCid() != kSmiCid)) {
          use->set_reaching_cid(kSmiCid);
          AddToWorklist(phi);
        }
      }
    }
  }

  for (intptr_t i = rollback_point; i < rollback_checks_.length(); i++) {
    known_smis_->Remove(rollback_checks_[i]);
  }
  rollback_checks_.TruncateTo(rollback_point);
}


void SminessPropagator::Propagate() {
  PropagateSminessRecursive(flow_graph_->graph_entry());
  ProcessPhis();
}


void FlowGraphOptimizer::PropagateSminess() {
  SminessPropagator propagator(flow_graph_);
  propagator.Propagate();
}


// Range analysis for smi values.
class RangeAnalysis : public ValueObject {
 public:
  explicit RangeAnalysis(FlowGraph* flow_graph)
      : flow_graph_(flow_graph),
        marked_defns_(NULL) { }

  // Infer ranges for all values and remove overflow checks from binary smi
  // operations when proven redundant.
  void Analyze();

 private:
  // Collect all values that were proven to be smi in smi_values_ array and all
  // CheckSmi instructions in smi_check_ array.
  void CollectSmiValues();

  // Iterate over smi values and constrain them at branch successors.
  // Additionally constraint values after CheckSmi instructions.
  void InsertConstraints();

  // Iterate over uses of the given definition and discover branches that
  // constrain it. Insert appropriate Constraint instructions at true
  // and false successor and rename all dominated uses to refer to a
  // Constraint instead of this definition.
  void InsertConstraintsFor(Definition* defn);

  // Create a constraint for defn, insert it after given instruction and
  // rename all uses that are dominated by it.
  ConstraintInstr* InsertConstraintFor(Definition* defn,
                                       Range* constraint,
                                       Instruction* after);

  void ConstrainValueAfterBranch(Definition* defn, Value* use);
  void ConstrainValueAfterCheckArrayBound(Definition* defn,
                                          CheckArrayBoundInstr* check);
  Definition* LoadArrayLength(CheckArrayBoundInstr* check);

  // Replace uses of the definition def that are dominated by instruction dom
  // with uses of other definition.
  void RenameDominatedUses(Definition* def,
                           Instruction* dom,
                           Definition* other);


  // Walk the dominator tree and infer ranges for smi values.
  void InferRanges();
  void InferRangesRecursive(BlockEntryInstr* block);

  enum Direction {
    kUnknown,
    kPositive,
    kNegative,
    kBoth
  };

  Range* InferInductionVariableRange(JoinEntryInstr* loop_header,
                                     PhiInstr* var);

  void ResetWorklist();
  void MarkDefinition(Definition* defn);

  static Direction ToDirection(Value* val);

  static Direction Invert(Direction direction) {
    return (direction == kPositive) ? kNegative : kPositive;
  }

  static void UpdateDirection(Direction* direction,
                              Direction new_direction) {
    if (*direction != new_direction) {
      if (*direction != kUnknown) new_direction = kBoth;
      *direction = new_direction;
    }
  }

  // Remove artificial Constraint instructions and replace them with actual
  // unconstrained definitions.
  void RemoveConstraints();

  FlowGraph* flow_graph_;

  GrowableArray<Definition*> smi_values_;  // Value that are known to be smi.
  GrowableArray<CheckSmiInstr*> smi_checks_;  // All CheckSmi instructions.

  // All Constraints inserted during InsertConstraints phase. They are treated
  // as smi values.
  GrowableArray<ConstraintInstr*> constraints_;

  // Bitvector for a quick filtering of known smi values.
  BitVector* smi_definitions_;

  // Worklist for induction variables analysis.
  GrowableArray<Definition*> worklist_;
  BitVector* marked_defns_;

  class ArrayLengthData : public ValueObject {
   public:
    ArrayLengthData(Definition* array, Definition* array_length)
        : array_(array), array_length_(array_length) { }

    ArrayLengthData(const ArrayLengthData& other)
        : ValueObject(),
          array_(other.array_),
          array_length_(other.array_length_) { }

    ArrayLengthData& operator=(const ArrayLengthData& other) {
      array_ = other.array_;
      array_length_ = other.array_length_;
      return *this;
    }

    Definition* array() const { return array_; }
    Definition* array_length() const { return array_length_; }

    typedef Definition* Value;
    typedef Definition* Key;
    typedef class ArrayLengthData Pair;

    // KeyValueTrait members.
    static Key KeyOf(const ArrayLengthData& data) {
      return data.array();
    }

    static Value ValueOf(const ArrayLengthData& data) {
      return data.array_length();
    }

    static inline intptr_t Hashcode(Key key) {
      return reinterpret_cast<intptr_t>(key);
    }

    static inline bool IsKeyEqual(const ArrayLengthData& kv, Key key) {
      return kv.array() == key;
    }

   private:
    Definition* array_;
    Definition* array_length_;
  };

  DirectChainedHashMap<ArrayLengthData> array_lengths_;

  DISALLOW_COPY_AND_ASSIGN(RangeAnalysis);
};


void RangeAnalysis::Analyze() {
  CollectSmiValues();
  InsertConstraints();
  InferRanges();
  RemoveConstraints();
}


void RangeAnalysis::CollectSmiValues() {
  for (BlockIterator block_it = flow_graph_->reverse_postorder_iterator();
       !block_it.Done();
       block_it.Advance()) {
    BlockEntryInstr* block = block_it.Current();
    for (ForwardInstructionIterator instr_it(block);
         !instr_it.Done();
         instr_it.Advance()) {
      Instruction* current = instr_it.Current();
      Definition* defn = current->AsDefinition();
      if (defn != NULL) {
        if ((defn->GetPropagatedCid() == kSmiCid) &&
            (defn->ssa_temp_index() != -1)) {
          smi_values_.Add(defn);
        }
      } else if (current->IsCheckSmi()) {
        smi_checks_.Add(current->AsCheckSmi());
      }
    }

    JoinEntryInstr* join = block->AsJoinEntry();
    if (join != NULL) {
      for (PhiIterator phi_it(join); !phi_it.Done(); phi_it.Advance()) {
        PhiInstr* current = phi_it.Current();
        if (current->GetPropagatedCid() == kSmiCid) {
          smi_values_.Add(current);
        }
      }
    }
  }
}


// Returns true if use is dominated by the given instruction.
// Note: uses that occur at instruction itself are not dominated by it.
static bool IsDominatedUse(Instruction* dom, Value* use) {
  BlockEntryInstr* dom_block = dom->GetBlock();

  Instruction* instr = use->instruction();

  PhiInstr* phi = instr->AsPhi();
  if (phi != NULL) {
    return dom_block->Dominates(phi->block()->PredecessorAt(use->use_index()));
  }

  BlockEntryInstr* use_block = instr->GetBlock();
  if (use_block == dom_block) {
    // Fast path for the case of block entry.
    if (dom_block == dom) return true;

    for (Instruction* curr = dom->next(); curr != NULL; curr = curr->next()) {
      if (curr == instr) return true;
    }

    return false;
  }

  return dom_block->Dominates(use_block);
}


void RangeAnalysis::RenameDominatedUses(Definition* def,
                                        Instruction* dom,
                                        Definition* other) {
  Value* next_use = NULL;
  Value* prev_use = NULL;
  for (Value* use = def->input_use_list();
       use != NULL;
       use = next_use) {
    next_use = use->next_use();

    // Skip dead phis.
    if (use->instruction()->IsPhi() &&
        !use->instruction()->AsPhi()->is_alive()) {
      prev_use = use;
      continue;
    }

    if (IsDominatedUse(dom, use)) {
      if (prev_use != NULL) {
        prev_use->set_next_use(next_use);
      } else {
        def->set_input_use_list(next_use);
      }
      use->set_definition(other);
      use->AddToInputUseList();
    } else {
      prev_use = use;
    }
  }
}


// For a comparison operation return an operation for the equivalent flipped
// comparison: a (op) b === b (op') a.
static Token::Kind FlipComparison(Token::Kind op) {
  switch (op) {
    case Token::kEQ: return Token::kEQ;
    case Token::kNE: return Token::kNE;
    case Token::kLT: return Token::kGT;
    case Token::kGT: return Token::kLT;
    case Token::kLTE: return Token::kGTE;
    case Token::kGTE: return Token::kLTE;
    default:
      UNREACHABLE();
      return Token::kILLEGAL;
  }
}

// For a comparison operation return an operation for the negated comparison:
// !(a (op) b) === a (op') b
static Token::Kind NegateComparison(Token::Kind op) {
  switch (op) {
    case Token::kEQ: return Token::kNE;
    case Token::kNE: return Token::kEQ;
    case Token::kLT: return Token::kGTE;
    case Token::kGT: return Token::kLTE;
    case Token::kLTE: return Token::kGT;
    case Token::kGTE: return Token::kLT;
    default:
      UNREACHABLE();
      return Token::kILLEGAL;
  }
}


// Given a boundary (right operand) and a comparison operation return
// a symbolic range constraint for the left operand of the comparison assuming
// that it evaluated to true.
// For example for the comparison a < b symbol a is constrained with range
// [Smi::kMinValue, b - 1].
static Range* ConstraintRange(Token::Kind op, Definition* boundary) {
  switch (op) {
    case Token::kEQ:
      return new Range(RangeBoundary::FromDefinition(boundary),
                       RangeBoundary::FromDefinition(boundary));
    case Token::kNE:
      return Range::Unknown();
    case Token::kLT:
      return new Range(RangeBoundary::MinSmi(),
                       RangeBoundary::FromDefinition(boundary, -1));
    case Token::kGT:
      return new Range(RangeBoundary::FromDefinition(boundary, 1),
                       RangeBoundary::MaxSmi());
    case Token::kLTE:
      return new Range(RangeBoundary::MinSmi(),
                       RangeBoundary::FromDefinition(boundary));
    case Token::kGTE:
      return new Range(RangeBoundary::FromDefinition(boundary),
                       RangeBoundary::MaxSmi());
    default:
      UNREACHABLE();
      return Range::Unknown();
  }
}


ConstraintInstr* RangeAnalysis::InsertConstraintFor(Definition* defn,
                                                    Range* constraint_range,
                                                    Instruction* after) {
  // No need to constrain constants.
  if (defn->IsConstant()) return NULL;

  ConstraintInstr* constraint =
      new ConstraintInstr(new Value(defn), constraint_range);
  constraint->InsertAfter(after);
  constraint->set_ssa_temp_index(flow_graph_->alloc_ssa_temp_index());
  RenameDominatedUses(defn, after, constraint);
  constraints_.Add(constraint);
  constraint->value()->set_instruction(constraint);
  constraint->value()->set_use_index(0);
  constraint->value()->AddToInputUseList();
  return constraint;
}


void RangeAnalysis::ConstrainValueAfterBranch(Definition* defn, Value* use) {
  BranchInstr* branch = use->instruction()->AsBranch();
  RelationalOpInstr* rel_op = branch->comparison()->AsRelationalOp();
  if ((rel_op != NULL) && (rel_op->operands_class_id() == kSmiCid)) {
    // Found comparison of two smis. Constrain defn at true and false
    // successors using the other operand as a boundary.
    Definition* boundary;
    Token::Kind op_kind;
    if (use->use_index() == 0) {  // Left operand.
      boundary = rel_op->InputAt(1)->definition();
      op_kind = rel_op->kind();
    } else {
      ASSERT(use->use_index() == 1);  // Right operand.
      boundary = rel_op->InputAt(0)->definition();
      // InsertConstraintFor assumes that defn is left operand of a
      // comparison if it is right operand flip the comparison.
      op_kind = FlipComparison(rel_op->kind());
    }

    // Constrain definition at the true successor.
    ConstraintInstr* true_constraint =
        InsertConstraintFor(defn,
                            ConstraintRange(op_kind, boundary),
                            branch->true_successor());
    // Mark true_constraint an artificial use of boundary. This ensures
    // that constraint's range is recalculated if boundary's range changes.
    if (true_constraint != NULL) true_constraint->AddDependency(boundary);

    // Constrain definition with a negated condition at the false successor.
    ConstraintInstr* false_constraint =
        InsertConstraintFor(
            defn,
            ConstraintRange(NegateComparison(op_kind), boundary),
            branch->false_successor());
    // Mark false_constraint an artificial use of boundary. This ensures
    // that constraint's range is recalculated if boundary's range changes.
    if (false_constraint != NULL) false_constraint->AddDependency(boundary);
  }
}

void RangeAnalysis::InsertConstraintsFor(Definition* defn) {
  for (Value* use = defn->input_use_list();
       use != NULL;
       use = use->next_use()) {
    if (use->instruction()->IsBranch()) {
      ConstrainValueAfterBranch(defn, use);
    } else if (use->instruction()->IsCheckArrayBound()) {
      ConstrainValueAfterCheckArrayBound(
          defn,
          use->instruction()->AsCheckArrayBound());
    }
  }
}


Definition* RangeAnalysis::LoadArrayLength(CheckArrayBoundInstr* check) {
  Definition* array = check->array()->definition();

  Definition* length = array_lengths_.Lookup(array);
  if (length != NULL) return length;

  StaticCallInstr* allocation = array->AsStaticCall();
  if ((allocation != NULL) &&
      allocation->is_known_constructor() &&
      (allocation->ResultCid() == kArrayCid)) {
    // For fixed length arrays check if array is the result of a constructor
    // call. In this case we can use the length passed to the constructor
    // instead of loading it from array itself.
    length = allocation->ArgumentAt(1)->value()->definition();
  } else {
    // Load length from the array. Do not insert instruction into the graph.
    // It will only be used in range boundaries.
    LoadFieldInstr* length_load = new LoadFieldInstr(
        check->array()->Copy(),
        CheckArrayBoundInstr::LengthOffsetFor(check->array_type()),
        Type::ZoneHandle(Type::SmiType()),
        true);  // Immutable.
    length_load->set_recognized_kind(MethodRecognizer::kObjectArrayLength);
    length_load->set_result_cid(kSmiCid);
    length_load->set_ssa_temp_index(flow_graph_->alloc_ssa_temp_index());
    length = length_load;
  }

  ASSERT(length != NULL);
  array_lengths_.Insert(ArrayLengthData(array, length));
  return length;
}


void RangeAnalysis::ConstrainValueAfterCheckArrayBound(
    Definition* defn, CheckArrayBoundInstr* check) {
  if (!CheckArrayBoundInstr::IsFixedLengthArrayType(check->array_type())) {
    return;
  }

  Definition* length = LoadArrayLength(check);

  Range* constraint_range = new Range(
      RangeBoundary::FromConstant(0),
      RangeBoundary::FromDefinition(length, -1));
  InsertConstraintFor(defn, constraint_range, check);
}


void RangeAnalysis::InsertConstraints() {
  for (intptr_t i = 0; i < smi_checks_.length(); i++) {
    CheckSmiInstr* check = smi_checks_[i];
    ConstraintInstr* constraint =
        InsertConstraintFor(check->value()->definition(),
                            Range::Unknown(),
                            check);
    if (constraint != NULL) {
      InsertConstraintsFor(constraint);  // Constrain uses further.
    }
  }

  for (intptr_t i = 0; i < smi_values_.length(); i++) {
    InsertConstraintsFor(smi_values_[i]);
  }
}


void RangeAnalysis::ResetWorklist() {
  if (marked_defns_ == NULL) {
    marked_defns_ = new BitVector(flow_graph_->current_ssa_temp_index());
  } else {
    marked_defns_->Clear();
  }
  worklist_.Clear();
}


void RangeAnalysis::MarkDefinition(Definition* defn) {
  // Unwrap constrained value.
  while (defn->IsConstraint()) {
    defn = defn->AsConstraint()->value()->definition();
  }

  if (!marked_defns_->Contains(defn->ssa_temp_index())) {
    worklist_.Add(defn);
    marked_defns_->Add(defn->ssa_temp_index());
  }
}


RangeAnalysis::Direction RangeAnalysis::ToDirection(Value* val) {
  if (val->BindsToConstant()) {
    return (Smi::Cast(val->BoundConstant()).Value() >= 0) ? kPositive
                                                          : kNegative;
  } else if (val->definition()->range() != NULL) {
    Range* range = val->definition()->range();
    if (Range::ConstantMin(range).value() >= 0) {
      return kPositive;
    } else if (Range::ConstantMax(range).value() <= 0) {
      return kNegative;
    }
  }
  return kUnknown;
}


Range* RangeAnalysis::InferInductionVariableRange(JoinEntryInstr* loop_header,
                                                  PhiInstr* var) {
  BitVector* loop_info = loop_header->loop_info();

  Definition* initial_value = NULL;
  Direction direction = kUnknown;

  ResetWorklist();
  MarkDefinition(var);
  while (!worklist_.is_empty()) {
    Definition* defn = worklist_.RemoveLast();

    if (defn->IsPhi()) {
      PhiInstr* phi = defn->AsPhi();
      for (intptr_t i = 0; i < phi->InputCount(); i++) {
        Definition* defn = phi->InputAt(i)->definition();

        if (!loop_info->Contains(defn->GetBlock()->preorder_number())) {
          // The value is coming from outside of the loop.
          if (initial_value == NULL) {
            initial_value = defn;
            continue;
          } else if (initial_value == defn) {
            continue;
          } else {
            return NULL;
          }
        }

        MarkDefinition(defn);
      }
    } else if (defn->IsBinarySmiOp()) {
      BinarySmiOpInstr* binary_op = defn->AsBinarySmiOp();

      switch (binary_op->op_kind()) {
        case Token::kADD: {
          const Direction growth_right =
              ToDirection(binary_op->right());
          if (growth_right != kUnknown) {
            UpdateDirection(&direction, growth_right);
            MarkDefinition(binary_op->left()->definition());
            break;
          }

          const Direction growth_left =
              ToDirection(binary_op->left());
          if (growth_left != kUnknown) {
            UpdateDirection(&direction, growth_left);
            MarkDefinition(binary_op->right()->definition());
            break;
          }

          return NULL;
        }

        case Token::kSUB: {
          const Direction growth_right =
              ToDirection(binary_op->right());
          if (growth_right != kUnknown) {
            UpdateDirection(&direction, Invert(growth_right));
            MarkDefinition(binary_op->left()->definition());
            break;
          }
          return NULL;
        }

        default:
          return NULL;
      }
    } else {
      return NULL;
    }
  }


  // We transitively discovered all dependencies of the given phi
  // and confirmed that it depends on a single value coming from outside of
  // the loop and some linear combinations of itself.
  // Compute the range based on initial value and the direction of the growth.
  switch (direction) {
    case kPositive:
      return new Range(RangeBoundary::FromDefinition(initial_value),
                       RangeBoundary::MaxSmi());

    case kNegative:
      return new Range(RangeBoundary::MinSmi(),
                       RangeBoundary::FromDefinition(initial_value));

    case kUnknown:
    case kBoth:
      return Range::Unknown();
  }

  UNREACHABLE();
  return NULL;
}


void RangeAnalysis::InferRangesRecursive(BlockEntryInstr* block) {
  JoinEntryInstr* join = block->AsJoinEntry();
  if (join != NULL) {
    const bool is_loop_header = (join->loop_info() != NULL);
    for (PhiIterator it(join); !it.Done(); it.Advance()) {
      PhiInstr* phi = it.Current();
      if (smi_definitions_->Contains(phi->ssa_temp_index())) {
        if (is_loop_header) {
          // Try recognizing simple induction variables.
          Range* range = InferInductionVariableRange(join, phi);
          if (range != NULL) {
            phi->range_ = range;
            continue;
          }
        }

        phi->InferRange();
      }
    }
  }

  for (ForwardInstructionIterator it(block); !it.Done(); it.Advance()) {
    Instruction* current = it.Current();

    Definition* defn = current->AsDefinition();
    if ((defn != NULL) &&
        (defn->ssa_temp_index() != -1) &&
        smi_definitions_->Contains(defn->ssa_temp_index())) {
      defn->InferRange();
    } else if (FLAG_array_bounds_check_elimination &&
               current->IsCheckArrayBound()) {
      CheckArrayBoundInstr* check = current->AsCheckArrayBound();
      RangeBoundary array_length =
          RangeBoundary::FromDefinition(LoadArrayLength(check));
      if (check->IsRedundant(array_length)) it.RemoveCurrentFromGraph();
    }
  }

  for (intptr_t i = 0; i < block->dominated_blocks().length(); ++i) {
    InferRangesRecursive(block->dominated_blocks()[i]);
  }
}


void RangeAnalysis::InferRanges() {
  // Initialize bitvector for quick filtering of smi values.
  smi_definitions_ = new BitVector(flow_graph_->current_ssa_temp_index());
  for (intptr_t i = 0; i < smi_values_.length(); i++) {
    smi_definitions_->Add(smi_values_[i]->ssa_temp_index());
  }
  for (intptr_t i = 0; i < constraints_.length(); i++) {
    smi_definitions_->Add(constraints_[i]->ssa_temp_index());
  }

  // Infer initial values of ranges.
  InferRangesRecursive(flow_graph_->graph_entry());

  if (FLAG_trace_range_analysis) {
    OS::Print("---- after range analysis -------\n");
    FlowGraphPrinter printer(*flow_graph_);
    printer.PrintBlocks();
  }
}


void RangeAnalysis::RemoveConstraints() {
  for (intptr_t i = 0; i < constraints_.length(); i++) {
    Definition* def = constraints_[i]->value()->definition();
    // Some constraints might be constraining constraints. Unwind the chain of
    // constraints until we reach the actual definition.
    while (def->IsConstraint()) {
      def = def->AsConstraint()->value()->definition();
    }
    constraints_[i]->ReplaceUsesWith(def);
    constraints_[i]->RemoveDependency();
    constraints_[i]->RemoveFromGraph();
  }
}


void FlowGraphOptimizer::InferSmiRanges() {
  RangeAnalysis range_analysis(flow_graph_);
  range_analysis.Analyze();
}


void FlowGraphTypePropagator::VisitBlocks() {
  ASSERT(current_iterator_ == NULL);
  for (intptr_t i = 0; i < block_order_.length(); ++i) {
    BlockEntryInstr* entry = block_order_[i];
    entry->Accept(this);
    ForwardInstructionIterator it(entry);
    current_iterator_ = &it;
    for (; !it.Done(); it.Advance()) {
      Instruction* current = it.Current();
      // No need to propagate the input types of the instruction, as long as
      // PhiInstr's are handled as part of JoinEntryInstr.

      // Visit the instruction and possibly eliminate type checks.
      current->Accept(this);
      // The instruction may have been removed from the graph.
      Definition* defn = current->AsDefinition();
      if ((defn != NULL) &&
          !defn->IsPushArgument() &&
          (defn->previous() != NULL)) {
        // Cache the propagated computation type.
        AbstractType& type = AbstractType::Handle(defn->CompileType());
        still_changing_ = defn->SetPropagatedType(type) || still_changing_;

        // Propagate class ids.
        const intptr_t cid = defn->ResultCid();
        still_changing_ = defn->SetPropagatedCid(cid) || still_changing_;
      }
    }
    current_iterator_ = NULL;
  }
}


void FlowGraphTypePropagator::VisitAssertAssignable(
    AssertAssignableInstr* instr) {
  bool is_null, is_instance;
  if (FLAG_eliminate_type_checks &&
      !instr->is_eliminated() &&
      ((instr->value()->CanComputeIsNull(&is_null) && is_null) ||
       (instr->value()->CanComputeIsInstanceOf(instr->dst_type(), &is_instance)
        && is_instance))) {
    // TODO(regis): Remove is_eliminated_ field and support.
    instr->eliminate();

    Value* use = instr->value();
    ASSERT(use != NULL);
    Definition* result = use->definition();
    ASSERT(result != NULL);
    // Replace uses and remove the current instruction via the iterator.
    instr->ReplaceUsesWith(result);
    ASSERT(current_iterator()->Current() == instr);
    current_iterator()->RemoveCurrentFromGraph();
    if (FLAG_trace_optimization) {
      OS::Print("Replacing v%"Pd" with v%"Pd"\n",
                instr->ssa_temp_index(),
                result->ssa_temp_index());
    }

    if (FLAG_trace_type_check_elimination) {
      FlowGraphPrinter::PrintTypeCheck(parsed_function(),
                                       instr->token_pos(),
                                       instr->value(),
                                       instr->dst_type(),
                                       instr->dst_name(),
                                       instr->is_eliminated());
    }
  }
}


void FlowGraphTypePropagator::VisitAssertBoolean(AssertBooleanInstr* instr) {
  bool is_null, is_bool;
  if (FLAG_eliminate_type_checks &&
      !instr->is_eliminated() &&
      instr->value()->CanComputeIsNull(&is_null) &&
      !is_null &&
      instr->value()->CanComputeIsInstanceOf(Type::Handle(Type::BoolType()),
                                             &is_bool) &&
      is_bool) {
    // TODO(regis): Remove is_eliminated_ field and support.
    instr->eliminate();
    Value* use = instr->value();
    Definition* result = use->definition();
    ASSERT(result != NULL);
    // Replace uses and remove the current instruction via the iterator.
    instr->ReplaceUsesWith(result);
    ASSERT(current_iterator()->Current() == instr);
    current_iterator()->RemoveCurrentFromGraph();
    if (FLAG_trace_optimization) {
      OS::Print("Replacing v%"Pd" with v%"Pd"\n",
                instr->ssa_temp_index(),
                result->ssa_temp_index());
    }

    if (FLAG_trace_type_check_elimination) {
      FlowGraphPrinter::PrintTypeCheck(parsed_function(),
                                       instr->token_pos(),
                                       instr->value(),
                                       Type::Handle(Type::BoolType()),
                                       Symbols::BooleanExpression(),
                                       instr->is_eliminated());
    }
  }
}


void FlowGraphTypePropagator::VisitInstanceOf(InstanceOfInstr* instr) {
  bool is_null;
  bool is_instance = false;
  if (FLAG_eliminate_type_checks &&
      instr->value()->CanComputeIsNull(&is_null) &&
      (is_null ||
       instr->value()->CanComputeIsInstanceOf(instr->type(), &is_instance))) {
    bool val = instr->negate_result() ? !is_instance : is_instance;
    Definition* result = new ConstantInstr(val ? Bool::True() : Bool::False());
    result->set_ssa_temp_index(flow_graph_->alloc_ssa_temp_index());
    result->InsertBefore(instr);
    // Replace uses and remove the current instruction via the iterator.
    instr->ReplaceUsesWith(result);
    ASSERT(current_iterator()->Current() == instr);
    current_iterator()->RemoveCurrentFromGraph();
    if (FLAG_trace_optimization) {
      OS::Print("Replacing v%"Pd" with v%"Pd"\n",
                instr->ssa_temp_index(),
                result->ssa_temp_index());
    }

    if (FLAG_trace_type_check_elimination) {
      FlowGraphPrinter::PrintTypeCheck(parsed_function(),
                                       instr->token_pos(),
                                       instr->value(),
                                       instr->type(),
                                       Symbols::InstanceOf(),
                                       /* eliminated = */ true);
    }
  }
}


void FlowGraphTypePropagator::VisitGraphEntry(GraphEntryInstr* graph_entry) {
  // Visit incoming parameters.
  for (intptr_t i = 0; i < graph_entry->initial_definitions()->length(); i++) {
    ParameterInstr* param =
        (*graph_entry->initial_definitions())[i]->AsParameter();
    if (param != NULL) VisitParameter(param);
  }
}


void FlowGraphTypePropagator::VisitJoinEntry(JoinEntryInstr* join_entry) {
  if (join_entry->phis() != NULL) {
    for (intptr_t i = 0; i < join_entry->phis()->length(); ++i) {
      PhiInstr* phi = (*join_entry->phis())[i];
      if (phi != NULL) {
        VisitPhi(phi);
      }
    }
  }
}


// TODO(srdjan): Investigate if the propagated cid should be more specific.
void FlowGraphTypePropagator::VisitPushArgument(PushArgumentInstr* push) {
  if (!push->has_propagated_cid()) push->SetPropagatedCid(kDynamicCid);
}


void FlowGraphTypePropagator::VisitPhi(PhiInstr* phi) {
  // We could set the propagated type of the phi to the least upper bound of its
  // input propagated types. However, keeping all propagated types allows us to
  // optimize method dispatch.
  // TODO(regis): Support a set of propagated types. For now, we compute the
  // least specific of the input propagated types.
  AbstractType& type = AbstractType::Handle(phi->LeastSpecificInputType());
  bool changed = phi->SetPropagatedType(type);
  if (changed) {
    still_changing_ = true;
  }

  // Merge class ids: if any two inputs have different class ids then result
  // is kDynamicCid.
  intptr_t merged_cid = kIllegalCid;
  for (intptr_t i = 0; i < phi->InputCount(); i++) {
    // Result cid of UseVal can be kIllegalCid if the referred definition
    // has not been visited yet.
    intptr_t cid = phi->InputAt(i)->ResultCid();
    if (cid == kIllegalCid) {
      still_changing_ = true;
      continue;
    }
    if (merged_cid == kIllegalCid) {
      // First time set.
      merged_cid = cid;
    } else if (merged_cid != cid) {
      merged_cid = kDynamicCid;
    }
  }
  if (merged_cid == kIllegalCid) {
    merged_cid = kDynamicCid;
  }
  changed = phi->SetPropagatedCid(merged_cid);
  if (changed) {
    still_changing_ = true;
  }
}


void FlowGraphTypePropagator::VisitParameter(ParameterInstr* param) {
  // TODO(regis): Once we inline functions, the propagated type of the formal
  // parameter will reflect the compile type of the passed-in argument.
  // For now, we do not know anything about the argument type and therefore set
  // it to the DynamicType, unless the argument is a compiler generated value,
  // i.e. the receiver argument or the constructor phase argument.
  AbstractType& param_type = AbstractType::Handle(Type::DynamicType());
  param->SetPropagatedCid(kDynamicCid);
  bool param_type_is_known = false;
  if (param->index() == 0) {
    const Function& function = parsed_function().function();
    if ((function.IsDynamicFunction() || function.IsConstructor())) {
      // Parameter is the receiver .
      param_type_is_known = true;
    }
  } else if ((param->index() == 1) &&
      parsed_function().function().IsConstructor()) {
    // Parameter is the constructor phase.
    param_type_is_known = true;
  }
  if (param_type_is_known) {
    LocalScope* scope = parsed_function().node_sequence()->scope();
    param_type = scope->VariableAt(param->index())->type().raw();
    if (FLAG_use_cha) {
      const intptr_t cid = Class::Handle(param_type.type_class()).id();
      if (!CHA::HasSubclasses(cid)) {
        // Receiver's class has no subclasses.
        param->SetPropagatedCid(cid);
      }
    }
  }
  bool changed = param->SetPropagatedType(param_type);
  if (changed) {
    still_changing_ = true;
  }
}


void FlowGraphTypePropagator::PropagateTypes() {
  // TODO(regis): Is there a way to make this more efficient, e.g. by visiting
  // only blocks depending on blocks that have changed and not the whole graph.
  do {
    still_changing_ = false;
    VisitBlocks();
  } while (still_changing_);
}


static BlockEntryInstr* FindPreHeader(BlockEntryInstr* header) {
  for (intptr_t j = 0; j < header->PredecessorCount(); ++j) {
    BlockEntryInstr* candidate = header->PredecessorAt(j);
    if (header->dominator() == candidate) {
      return candidate;
    }
  }
  return NULL;
}


void LICM::Hoist(ForwardInstructionIterator* it,
                 BlockEntryInstr* pre_header,
                 Instruction* current) {
  // TODO(fschneider): Avoid repeated deoptimization when
  // speculatively hoisting checks.
  if (FLAG_trace_optimization) {
    OS::Print("Hoisting instruction %s:%"Pd" from B%"Pd" to B%"Pd"\n",
              current->DebugName(),
              current->GetDeoptId(),
              current->GetBlock()->block_id(),
              pre_header->block_id());
  }
  // Move the instruction out of the loop.
  it->RemoveCurrentFromGraph();
  GotoInstr* last = pre_header->last_instruction()->AsGoto();
  current->InsertBefore(last);
  // Attach the environment of the Goto instruction to the hoisted
  // instruction and set the correct deopt_id.
  ASSERT(last->env() != NULL);
  last->env()->DeepCopyTo(current);
  current->deopt_id_ = last->GetDeoptId();
}


void LICM::TryHoistCheckSmiThroughPhi(ForwardInstructionIterator* it,
                                      BlockEntryInstr* header,
                                      BlockEntryInstr* pre_header,
                                      CheckSmiInstr* current) {
  PhiInstr* phi = current->InputAt(0)->definition()->AsPhi();
  if (!header->loop_info()->Contains(phi->block()->preorder_number())) {
    return;
  }

  if (phi->GetPropagatedCid() == kSmiCid) {
    it->RemoveCurrentFromGraph();
    return;
  }

  // Check if there is only a single kDynamicCid input to the phi that
  // comes from the pre-header.
  const intptr_t kNotFound = -1;
  intptr_t non_smi_input = kNotFound;
  for (intptr_t i = 0; i < phi->InputCount(); ++i) {
    Value* input = phi->InputAt(i);
    if (input->ResultCid() != kSmiCid) {
      if ((non_smi_input != kNotFound) || (input->ResultCid() != kDynamicCid)) {
        // There are multiple kDynamicCid inputs or there is an input that is
        // known to be non-smi.
        return;
      } else {
        non_smi_input = i;
      }
    }
  }

  if ((non_smi_input == kNotFound) ||
      (phi->block()->PredecessorAt(non_smi_input) != pre_header)) {
    return;
  }

  // Host CheckSmi instruction and make this phi smi one.
  Hoist(it, pre_header, current);

  // Replace value we are checking with phi's input. Maintain use lists.
  Definition* non_smi_input_defn = phi->InputAt(non_smi_input)->definition();
  current->value()->RemoveFromInputUseList();
  current->value()->set_definition(non_smi_input_defn);
  current->value()->AddToInputUseList();

  phi->SetPropagatedCid(kSmiCid);
}


void LICM::Optimize(FlowGraph* flow_graph) {
  GrowableArray<BlockEntryInstr*> loop_headers;
  flow_graph->ComputeLoops(&loop_headers);

  for (intptr_t i = 0; i < loop_headers.length(); ++i) {
    BlockEntryInstr* header = loop_headers[i];
    // Skip loop that don't have a pre-header block.
    BlockEntryInstr* pre_header = FindPreHeader(header);
    if (pre_header == NULL) continue;

    for (BitVector::Iterator loop_it(header->loop_info());
         !loop_it.Done();
         loop_it.Advance()) {
      BlockEntryInstr* block = flow_graph->preorder()[loop_it.Current()];
      for (ForwardInstructionIterator it(block);
           !it.Done();
           it.Advance()) {
        Instruction* current = it.Current();
        if (!current->IsPushArgument() && !current->AffectedBySideEffect()) {
          bool inputs_loop_invariant = true;
          for (int i = 0; i < current->InputCount(); ++i) {
            Definition* input_def = current->InputAt(i)->definition();
            if (!input_def->GetBlock()->Dominates(pre_header)) {
              inputs_loop_invariant = false;
              break;
            }
          }
          if (inputs_loop_invariant &&
              !current->IsAssertAssignable() &&
              !current->IsAssertBoolean()) {
            // TODO(fschneider): Enable hoisting of Assert-instructions
            // if it safe to do.
            Hoist(&it, pre_header, current);
          } else if (current->IsCheckSmi() &&
                     current->InputAt(0)->definition()->IsPhi()) {
            TryHoistCheckSmiThroughPhi(
                &it, header, pre_header, current->AsCheckSmi());
          }
        }
      }
    }
  }
}


static bool IsLoadEliminationCandidate(Definition* def) {
  // Immutable loads (not affected by side effects) are handled
  // in the DominatorBasedCSE pass.
  // TODO(fschneider): Extend to other load instructions.
  return (def->IsLoadField() && def->AffectedBySideEffect())
      || def->IsLoadIndexed();
}


static intptr_t ComputeLoadOffsetInWords(Definition* defn) {
  if (defn->IsLoadIndexed()) {
    // We are assuming that LoadField is never used to load the first word.
    return 0;
  }

  LoadFieldInstr* load_field = defn->AsLoadField();
  if (load_field != NULL) {
    const intptr_t idx = load_field->offset_in_bytes() / kWordSize;
    ASSERT(idx > 0);
    return idx;
  }

  UNREACHABLE();
  return 0;
}


static bool IsInterferingStore(Instruction* instr,
                               intptr_t* offset_in_words) {
  if (instr->IsStoreIndexed()) {
    // We are assuming that LoadField is never used to load the first word.
    *offset_in_words = 0;
    return true;
  }

  StoreInstanceFieldInstr* store_instance_field = instr->AsStoreInstanceField();
  if (store_instance_field != NULL) {
    ASSERT(store_instance_field->field().Offset() != 0);
    *offset_in_words = store_instance_field->field().Offset() / kWordSize;
    return true;
  }

  StoreVMFieldInstr* store_vm_field = instr->AsStoreVMField();
  if (store_vm_field != NULL) {
    ASSERT(store_vm_field->offset_in_bytes() != 0);
    *offset_in_words = store_vm_field->offset_in_bytes() / kWordSize;
    return true;
  }

  return false;
}


static Definition* GetStoredValue(Instruction* instr) {
  if (instr->IsStoreIndexed()) {
    return instr->AsStoreIndexed()->value()->definition();
  }

  StoreInstanceFieldInstr* store_instance_field = instr->AsStoreInstanceField();
  if (store_instance_field != NULL) {
    return store_instance_field->value()->definition();
  }

  StoreVMFieldInstr* store_vm_field = instr->AsStoreVMField();
  if (store_vm_field != NULL) {
    return store_vm_field->value()->definition();
  }

  UNREACHABLE();  // Should only be called for supported store instructions.
  return NULL;
}


// KeyValueTrait used for numbering of loads. Allows to lookup loads
// corresponding to stores.
class LoadKeyValueTrait {
 public:
  typedef Definition* Value;
  typedef Definition* Key;
  typedef Definition* Pair;

  static Key KeyOf(Pair kv) {
    return kv;
  }

  static Value ValueOf(Pair kv) {
    return kv;
  }

  static inline intptr_t Hashcode(Key key) {
    intptr_t object = 0;
    intptr_t location = 0;

    if (key->IsLoadIndexed()) {
      LoadIndexedInstr* load_indexed = key->AsLoadIndexed();
      object = load_indexed->array()->definition()->ssa_temp_index();
      location = load_indexed->index()->definition()->ssa_temp_index();
    } else if (key->IsStoreIndexed()) {
      StoreIndexedInstr* store_indexed = key->AsStoreIndexed();
      object = store_indexed->array()->definition()->ssa_temp_index();
      location = store_indexed->index()->definition()->ssa_temp_index();
    } else if (key->IsLoadField()) {
      LoadFieldInstr* load_field = key->AsLoadField();
      object = load_field->value()->definition()->ssa_temp_index();
      location = load_field->offset_in_bytes();
    } else if (key->IsStoreInstanceField()) {
      StoreInstanceFieldInstr* store_field = key->AsStoreInstanceField();
      object = store_field->instance()->definition()->ssa_temp_index();
      location = store_field->field().Offset();
    } else if (key->IsStoreVMField()) {
      StoreVMFieldInstr* store_field = key->AsStoreVMField();
      object = store_field->dest()->definition()->ssa_temp_index();
      location = store_field->offset_in_bytes();
    }

    return object * 31 + location;
  }

  static inline bool IsKeyEqual(Pair kv, Key key) {
    if (kv->Equals(key)) return true;

    if (kv->IsLoadIndexed()) {
      if (key->IsStoreIndexed()) {
        LoadIndexedInstr* load_indexed = kv->AsLoadIndexed();
        StoreIndexedInstr* store_indexed = key->AsStoreIndexed();
        return load_indexed->array()->Equals(store_indexed->array()) &&
               load_indexed->index()->Equals(store_indexed->index());
      }
      return false;
    }

    ASSERT(kv->IsLoadField());
    LoadFieldInstr* load_field = kv->AsLoadField();
    if (key->IsStoreVMField()) {
      StoreVMFieldInstr* store_field = key->AsStoreVMField();
      return load_field->value()->Equals(store_field->dest()) &&
             (load_field->offset_in_bytes() == store_field->offset_in_bytes());
    } else if (key->IsStoreInstanceField()) {
      StoreInstanceFieldInstr* store_field = key->AsStoreInstanceField();
      return load_field->value()->Equals(store_field->instance()) &&
             (load_field->offset_in_bytes() == store_field->field().Offset());
    }

    return false;
  }
};


static intptr_t NumberLoadExpressions(
    FlowGraph* graph,
    DirectChainedHashMap<LoadKeyValueTrait>* map,
    GrowableArray<BitVector*>* kill_by_offs) {
  intptr_t expr_id = 0;

  // Loads representing different expression ids will be collected and
  // used to build per offset kill sets.
  GrowableArray<Definition*> loads(10);

  for (BlockIterator it = graph->reverse_postorder_iterator();
       !it.Done();
       it.Advance()) {
    BlockEntryInstr* block = it.Current();
    for (ForwardInstructionIterator instr_it(block);
         !instr_it.Done();
         instr_it.Advance()) {
      Definition* defn = instr_it.Current()->AsDefinition();
      if ((defn == NULL) || !IsLoadEliminationCandidate(defn)) {
        continue;
      }
      Definition* result = map->Lookup(defn);
      if (result == NULL) {
        map->Insert(defn);
        defn->set_expr_id(expr_id++);
        loads.Add(defn);
      } else {
        defn->set_expr_id(result->expr_id());
      }
    }
  }

  // Build per offset kill sets. Any store interferes only with loads from
  // the same offset.
  for (intptr_t i = 0; i < loads.length(); i++) {
    Definition* defn = loads[i];

    const intptr_t offset_in_words = ComputeLoadOffsetInWords(defn);
    while (kill_by_offs->length() <= offset_in_words) {
      kill_by_offs->Add(NULL);
    }
    if ((*kill_by_offs)[offset_in_words] == NULL) {
      (*kill_by_offs)[offset_in_words] = new BitVector(expr_id);
    }
    (*kill_by_offs)[offset_in_words]->Add(defn->expr_id());
  }

  return expr_id;
}


class LoadOptimizer : public ValueObject {
 public:
  LoadOptimizer(FlowGraph* graph,
                intptr_t max_expr_id,
                DirectChainedHashMap<LoadKeyValueTrait>* map,
                const GrowableArray<BitVector*>& kill_by_offset)
      : graph_(graph),
        map_(map),
        max_expr_id_(max_expr_id),
        kill_by_offset_(kill_by_offset),
        in_(graph_->preorder().length()),
        out_(graph_->preorder().length()),
        gen_(graph_->preorder().length()),
        kill_(graph_->preorder().length()),
        exposed_values_(graph_->preorder().length()),
        out_values_(graph_->preorder().length()),
        phis_(5),
        worklist_(5),
        in_worklist_(NULL) {
    const intptr_t num_blocks = graph_->preorder().length();
    for (intptr_t i = 0; i < num_blocks; i++) {
      out_.Add(new BitVector(max_expr_id_));
      gen_.Add(new BitVector(max_expr_id_));
      kill_.Add(new BitVector(max_expr_id_));
      in_.Add(new BitVector(max_expr_id_));

      exposed_values_.Add(NULL);
      out_values_.Add(NULL);
    }
  }

  void Optimize() {
    ComputeInitialSets();
    ComputeOutValues();
    ForwardLoads();
    EmitPhis();
  }

 private:
  // Compute sets of loads generated and killed by each block.
  // Additionally compute upwards exposed and generated loads for each block.
  // Exposed loads are those that can be replaced if a corresponding
  // reaching load will be found.
  // Loads that are locally redundant will be replaced as we go through
  // instructions.
  void ComputeInitialSets() {
    for (BlockIterator block_it = graph_->reverse_postorder_iterator();
         !block_it.Done();
         block_it.Advance()) {
      BlockEntryInstr* block = block_it.Current();
      const intptr_t preorder_number = block->preorder_number();

      BitVector* kill = kill_[preorder_number];
      BitVector* gen = gen_[preorder_number];

      ZoneGrowableArray<Definition*>* exposed_values = NULL;
      ZoneGrowableArray<Definition*>* out_values = NULL;

      for (ForwardInstructionIterator instr_it(block);
           !instr_it.Done();
           instr_it.Advance()) {
        Instruction* instr = instr_it.Current();

        intptr_t offset_in_words = 0;
        if (IsInterferingStore(instr, &offset_in_words)) {
          // Interfering stores kill only loads from the same offset.
          if ((offset_in_words < kill_by_offset_.length()) &&
              (kill_by_offset_[offset_in_words] != NULL)) {
            kill->AddAll(kill_by_offset_[offset_in_words]);
            // There is no need to clear out_values when clearing GEN set
            // because only those values that are in the GEN set
            // will ever be used.
            gen->RemoveAll(kill_by_offset_[offset_in_words]);

            // Only forward stores to normal arrays and float64 arrays
            // to loads because other array stores (intXX/uintXX/float32)
            // may implicitly convert the value stored.
            StoreIndexedInstr* array_store = instr->AsStoreIndexed();
            if (array_store == NULL ||
                array_store->class_id() == kArrayCid ||
                array_store->class_id() == kFloat64ArrayCid) {
              Definition* load = map_->Lookup(instr->AsDefinition());
              if (load != NULL) {
                // Store has a corresponding numbered load. Try forwarding
                // stored value to it.
                gen->Add(load->expr_id());
                if (out_values == NULL) out_values = CreateBlockOutValues();
                (*out_values)[load->expr_id()] = GetStoredValue(instr);
              }
            }
          }
          ASSERT(instr->IsDefinition() &&
                 !IsLoadEliminationCandidate(instr->AsDefinition()));
          continue;
        }

        // Other instructions with side effects kill all loads.
        if (instr->HasSideEffect()) {
          kill->SetAll();
          // There is no need to clear out_values when clearing GEN set
          // because only those values that are in the GEN set
          // will ever be used.
          gen->Clear();
          continue;
        }

        Definition* defn = instr->AsDefinition();
        if ((defn == NULL) || !IsLoadEliminationCandidate(defn)) {
          continue;
        }

        const intptr_t expr_id = defn->expr_id();
        if (gen->Contains(expr_id)) {
          // This is a locally redundant load.
          ASSERT((out_values != NULL) && ((*out_values)[expr_id] != NULL));

          Definition* replacement = (*out_values)[expr_id];
          EnsureSSATempIndex(graph_, defn, replacement);
          if (FLAG_trace_optimization) {
            OS::Print("Replacing load v%"Pd" with v%"Pd"\n",
                      defn->ssa_temp_index(),
                      replacement->ssa_temp_index());
          }

          defn->ReplaceUsesWith(replacement);
          instr_it.RemoveCurrentFromGraph();
          continue;
        } else if (!kill->Contains(expr_id)) {
          // This is an exposed load: it is the first representative of a
          // given expression id and it is not killed on the path from
          // the block entry.
          if (exposed_values == NULL) {
            static const intptr_t kMaxExposedValuesInitialSize = 5;
            exposed_values = new ZoneGrowableArray<Definition*>(
                Utils::Minimum(kMaxExposedValuesInitialSize, max_expr_id_));
          }

          exposed_values->Add(defn);
        }

        gen->Add(expr_id);

        if (out_values == NULL) out_values = CreateBlockOutValues();
        (*out_values)[expr_id] = defn;
      }

      out_[preorder_number]->CopyFrom(gen);
      exposed_values_[preorder_number] = exposed_values;
      out_values_[preorder_number] = out_values;
    }
  }

  // Compute OUT sets and corresponding out_values mappings by propagating them
  // iteratively until fix point is reached.
  // No replacement is done at this point and thus any out_value[expr_id] is
  // changed at most once: from NULL to an actual value.
  // When merging incoming loads we might need to create a phi.
  // These phis are not inserted at the graph immediately because some of them
  // might become redundant after load forwarding is done.
  void ComputeOutValues() {
    BitVector* temp = new BitVector(max_expr_id_);

    bool changed = true;
    while (changed) {
      changed = false;

      for (BlockIterator block_it = graph_->reverse_postorder_iterator();
           !block_it.Done();
           block_it.Advance()) {
        BlockEntryInstr* block = block_it.Current();

        const intptr_t preorder_number = block->preorder_number();

        BitVector* block_in = in_[preorder_number];
        BitVector* block_out = out_[preorder_number];
        BitVector* block_kill = kill_[preorder_number];
        BitVector* block_gen = gen_[preorder_number];

        if (FLAG_trace_optimization) {
          OS::Print("B%"Pd"", block->block_id());
          block_in->Print();
          block_out->Print();
          block_kill->Print();
          block_gen->Print();
          OS::Print("\n");
        }

        ZoneGrowableArray<Definition*>* block_out_values =
            out_values_[preorder_number];

        // Compute block_in as the intersection of all out(p) where p
        // is a predecessor of the current block.
        if (block->IsGraphEntry()) {
          temp->Clear();
        } else {
          // TODO(vegorov): this can be optimized for the case of a single
          // predecessor.
          // TODO(vegorov): this can be reordered to reduce amount of operations
          // temp->CopyFrom(first_predecessor)
          temp->SetAll();
          ASSERT(block->PredecessorCount() > 0);
          for (intptr_t i = 0; i < block->PredecessorCount(); i++) {
            BlockEntryInstr* pred = block->PredecessorAt(i);
            BitVector* pred_out = out_[pred->preorder_number()];
            temp->Intersect(*pred_out);
          }
        }

        if (!temp->Equals(*block_in)) {
          // If IN set has changed propagate the change to OUT set.
          block_in->CopyFrom(temp);
          if (block_out->KillAndAdd(block_kill, block_in)) {
            // If OUT set has changed then we have new values available out of
            // the block. Compute these values creating phi where necessary.
            for (BitVector::Iterator it(block_out);
                 !it.Done();
                 it.Advance()) {
              const intptr_t expr_id = it.Current();

              if (block_out_values == NULL) {
                out_values_[preorder_number] = block_out_values =
                    CreateBlockOutValues();
              }

              if ((*block_out_values)[expr_id] == NULL) {
                ASSERT(block->PredecessorCount() > 0);
                (*block_out_values)[expr_id] =
                    MergeIncomingValues(block, expr_id);
              }
            }
            changed = true;
          }
        }

        if (FLAG_trace_optimization) {
          OS::Print("after B%"Pd"", block->block_id());
          block_in->Print();
          block_out->Print();
          block_kill->Print();
          block_gen->Print();
          OS::Print("\n");
        }
      }
    }
  }

  // Compute incoming value for the given expression id.
  // Will create a phi if different values are incoming from multiple
  // predecessors.
  Definition* MergeIncomingValues(BlockEntryInstr* block, intptr_t expr_id) {
    // First check if the same value is coming in from all predecessors.
    Definition* incoming = NULL;
    for (intptr_t i = 0; i < block->PredecessorCount(); i++) {
      BlockEntryInstr* pred = block->PredecessorAt(i);
      ZoneGrowableArray<Definition*>* pred_out_values =
          out_values_[pred->preorder_number()];
      if (incoming == NULL) {
        incoming = (*pred_out_values)[expr_id];
      } else if (incoming != (*pred_out_values)[expr_id]) {
        incoming = NULL;
        break;
      }
    }

    if (incoming != NULL) {
      return incoming;
    }

    // Incoming values are different. Phi is required to merge.
    PhiInstr* phi = new PhiInstr(
        block->AsJoinEntry(), block->PredecessorCount());

    for (intptr_t i = 0; i < block->PredecessorCount(); i++) {
      BlockEntryInstr* pred = block->PredecessorAt(i);
      ZoneGrowableArray<Definition*>* pred_out_values =
          out_values_[pred->preorder_number()];
      ASSERT((*pred_out_values)[expr_id] != NULL);

      // Sets of outgoing values are not linked into use lists so
      // they might contain values that were replaced and removed
      // from the graph by this iteration.
      // To prevent using them we additionally mark definitions themselves
      // as replaced and store a pointer to the replacement.
      Value* input = new Value((*pred_out_values)[expr_id]->Replacement());
      phi->SetInputAt(i, input);

      // TODO(vegorov): add a helper function to handle input insertion.
      input->set_instruction(phi);
      input->set_use_index(i);
      input->AddToInputUseList();
    }

    phi->set_ssa_temp_index(graph_->alloc_ssa_temp_index());
    phis_.Add(phi);  // Postpone phi insertion until after load forwarding.

    return phi;
  }

  // Iterate over basic blocks and replace exposed loads with incoming
  // values.
  void ForwardLoads() {
    for (BlockIterator block_it = graph_->reverse_postorder_iterator();
         !block_it.Done();
         block_it.Advance()) {
      BlockEntryInstr* block = block_it.Current();

      ZoneGrowableArray<Definition*>* loads =
          exposed_values_[block->preorder_number()];
      if (loads == NULL) continue;  // No exposed loads.

      BitVector* in = in_[block->preorder_number()];

      for (intptr_t i = 0; i < loads->length(); i++) {
        Definition* load = (*loads)[i];
        if (!in->Contains(load->expr_id())) continue;  // No incoming value.

        Definition* replacement = MergeIncomingValues(block, load->expr_id());

        // Sets of outgoing values are not linked into use lists so
        // they might contain values that were replace and removed
        // from the graph by this iteration.
        // To prevent using them we additionally mark definitions themselves
        // as replaced and store a pointer to the replacement.
        replacement = replacement->Replacement();

        if (load != replacement) {
          EnsureSSATempIndex(graph_, load, replacement);

          if (FLAG_trace_optimization) {
            OS::Print("Replacing load v%"Pd" with v%"Pd"\n",
                      load->ssa_temp_index(),
                      replacement->ssa_temp_index());
          }

          load->ReplaceUsesWith(replacement);
          load->RemoveFromGraph();
          load->SetReplacement(replacement);
        }
      }
    }
  }

  // Check if the given phi take the same value on all code paths.
  // Eliminate it as redundant if this is the case.
  // When analyzing phi operands assumes that only generated during
  // this load phase can be redundant. They can be distinguished because
  // they are not marked alive.
  // TODO(vegorov): move this into a separate phase over all phis.
  bool EliminateRedundantPhi(PhiInstr* phi) {
    Definition* value = NULL;  // Possible value of this phi.

    worklist_.Clear();
    if (in_worklist_ == NULL) {
      in_worklist_ = new BitVector(graph_->current_ssa_temp_index());
    } else {
      in_worklist_->Clear();
    }

    worklist_.Add(phi);
    in_worklist_->Add(phi->ssa_temp_index());

    for (intptr_t i = 0; i < worklist_.length(); i++) {
      PhiInstr* phi = worklist_[i];

      for (intptr_t i = 0; i < phi->InputCount(); i++) {
        Definition* input = phi->InputAt(i)->definition();
        if (input == phi) continue;

        PhiInstr* phi_input = input->AsPhi();
        if ((phi_input != NULL) && !phi_input->is_alive()) {
          if (!in_worklist_->Contains(phi_input->ssa_temp_index())) {
            worklist_.Add(phi_input);
            in_worklist_->Add(phi_input->ssa_temp_index());
          }
          continue;
        }

        if (value == NULL) {
          value = input;
        } else if (value != input) {
          return false;  // This phi is not redundant.
        }
      }
    }

    // All phis in the worklist are redundant and have the same computed
    // value on all code paths.
    ASSERT(value != NULL);
    for (intptr_t i = 0; i < worklist_.length(); i++) {
      worklist_[i]->ReplaceUsesWith(value);
    }

    return true;
  }

  // Emit non-redundant phis created during ComputeOutValues and ForwardLoads.
  void EmitPhis() {
    for (intptr_t i = 0; i < phis_.length(); i++) {
      PhiInstr* phi = phis_[i];
      if ((phi->input_use_list() != NULL) && !EliminateRedundantPhi(phi)) {
        phi->mark_alive();
        phi->block()->InsertPhi(phi);
      }
    }
  }

  ZoneGrowableArray<Definition*>* CreateBlockOutValues() {
    ZoneGrowableArray<Definition*>* out =
        new ZoneGrowableArray<Definition*>(max_expr_id_);
    for (intptr_t i = 0; i < max_expr_id_; i++) {
      out->Add(NULL);
    }
    return out;
  }

  FlowGraph* graph_;
  DirectChainedHashMap<LoadKeyValueTrait>* map_;
  const intptr_t max_expr_id_;

  // Mapping between field offsets in words and expression ids of loads from
  // that offset.
  const GrowableArray<BitVector*>& kill_by_offset_;

  // Per block sets of expression ids for loads that are: incoming (available
  // on the entry), outgoing (available on the exit), generated and killed.
  GrowableArray<BitVector*> in_;
  GrowableArray<BitVector*> out_;
  GrowableArray<BitVector*> gen_;
  GrowableArray<BitVector*> kill_;

  // Per block list of upwards exposed loads.
  GrowableArray<ZoneGrowableArray<Definition*>*> exposed_values_;

  // Per block mappings between expression ids and outgoing definitions that
  // represent those ids.
  GrowableArray<ZoneGrowableArray<Definition*>*> out_values_;

  // List of phis generated during ComputeOutValues and ForwardLoads.
  // Some of these phis might be redundant and thus a separate pass is
  // needed to emit only non-redundant ones.
  GrowableArray<PhiInstr*> phis_;

  // Auxiliary worklist used by redundant phi elimination.
  GrowableArray<PhiInstr*> worklist_;
  BitVector* in_worklist_;

  DISALLOW_COPY_AND_ASSIGN(LoadOptimizer);
};


bool DominatorBasedCSE::Optimize(FlowGraph* graph) {
  bool changed = false;
  if (FLAG_load_cse) {
    GrowableArray<BitVector*> kill_by_offs(10);
    DirectChainedHashMap<LoadKeyValueTrait> map;
    const intptr_t max_expr_id =
        NumberLoadExpressions(graph, &map, &kill_by_offs);
    if (max_expr_id > 0) {
      LoadOptimizer load_optimizer(graph, max_expr_id, &map, kill_by_offs);
      load_optimizer.Optimize();
    }
  }

  DirectChainedHashMap<PointerKeyValueTrait<Instruction> > map;
  changed = OptimizeRecursive(graph, graph->graph_entry(), &map) || changed;

  return changed;
}


bool DominatorBasedCSE::OptimizeRecursive(
    FlowGraph* graph,
    BlockEntryInstr* block,
    DirectChainedHashMap<PointerKeyValueTrait<Instruction> >* map) {
  bool changed = false;
  for (ForwardInstructionIterator it(block); !it.Done(); it.Advance()) {
    Instruction* current = it.Current();
    if (current->AffectedBySideEffect()) continue;
    Instruction* replacement = map->Lookup(current);
    if (replacement == NULL) {
      map->Insert(current);
      continue;
    }
    // Replace current with lookup result.
    ReplaceCurrentInstruction(&it, current, replacement, graph);
    changed = true;
  }

  // Process children in the dominator tree recursively.
  intptr_t num_children = block->dominated_blocks().length();
  for (intptr_t i = 0; i < num_children; ++i) {
    BlockEntryInstr* child = block->dominated_blocks()[i];
    if (i  < num_children - 1) {
      // Copy map.
      DirectChainedHashMap<PointerKeyValueTrait<Instruction> > child_map(*map);
      changed = OptimizeRecursive(graph, child, &child_map) || changed;
    } else {
      // Reuse map for the last child.
      changed = OptimizeRecursive(graph, child, map) || changed;
    }
  }
  return changed;
}


ConstantPropagator::ConstantPropagator(
    FlowGraph* graph,
    const GrowableArray<BlockEntryInstr*>& ignored)
    : FlowGraphVisitor(ignored),
      graph_(graph),
      unknown_(Object::transition_sentinel()),
      non_constant_(Object::sentinel()),
      reachable_(new BitVector(graph->preorder().length())),
      definition_marks_(new BitVector(graph->max_virtual_register_number())),
      block_worklist_(),
      definition_worklist_() {}


void ConstantPropagator::Optimize(FlowGraph* graph) {
  GrowableArray<BlockEntryInstr*> ignored;
  ConstantPropagator cp(graph, ignored);
  cp.Analyze();
  cp.Transform();
}


void ConstantPropagator::SetReachable(BlockEntryInstr* block) {
  if (!reachable_->Contains(block->preorder_number())) {
    reachable_->Add(block->preorder_number());
    block_worklist_.Add(block);
  }
}


void ConstantPropagator::SetValue(Definition* definition, const Object& value) {
  // We would like to assert we only go up (toward non-constant) in the lattice.
  //
  // ASSERT(IsUnknown(definition->constant_value()) ||
  //        IsNonConstant(value) ||
  //        (definition->constant_value().raw() == value.raw()));
  //
  // But the final disjunct is not true (e.g., mint or double constants are
  // heap-allocated and so not necessarily pointer-equal on each iteration).
  if (definition->constant_value().raw() != value.raw()) {
    definition->constant_value() = value.raw();
    if (definition->input_use_list() != NULL) {
      ASSERT(definition->HasSSATemp());
      if (!definition_marks_->Contains(definition->ssa_temp_index())) {
        definition_worklist_.Add(definition);
        definition_marks_->Add(definition->ssa_temp_index());
      }
    }
  }
}


// Compute the join of two values in the lattice, assign it to the first.
void ConstantPropagator::Join(Object* left, const Object& right) {
  // Join(non-constant, X) = non-constant
  // Join(X, unknown)      = X
  if (IsNonConstant(*left) || IsUnknown(right)) return;

  // Join(unknown, X)      = X
  // Join(X, non-constant) = non-constant
  if (IsUnknown(*left) || IsNonConstant(right)) {
    *left = right.raw();
    return;
  }

  // Join(X, X) = X
  // TODO(kmillikin): support equality for doubles, mints, etc.
  if (left->raw() == right.raw()) return;

  // Join(X, Y) = non-constant
  *left = non_constant_.raw();
}


// --------------------------------------------------------------------------
// Analysis of blocks.  Called at most once per block.  The block is already
// marked as reachable.  All instructions in the block are analyzed.
void ConstantPropagator::VisitGraphEntry(GraphEntryInstr* block) {
  const GrowableArray<Definition*>& defs = *block->initial_definitions();
  for (intptr_t i = 0; i < defs.length(); ++i) {
    defs[i]->Accept(this);
  }
  ASSERT(ForwardInstructionIterator(block).Done());

  SetReachable(block->normal_entry());
}


void ConstantPropagator::VisitJoinEntry(JoinEntryInstr* block) {
  // Phis are visited when visiting Goto at a predecessor. See VisitGoto.
  for (ForwardInstructionIterator it(block); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}


void ConstantPropagator::VisitTargetEntry(TargetEntryInstr* block) {
  for (ForwardInstructionIterator it(block); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}


void ConstantPropagator::VisitParallelMove(ParallelMoveInstr* instr) {
  // Parallel moves have not yet been inserted in the graph.
  UNREACHABLE();
}


// --------------------------------------------------------------------------
// Analysis of control instructions.  Unconditional successors are
// reachable.  Conditional successors are reachable depending on the
// constant value of the condition.
void ConstantPropagator::VisitReturn(ReturnInstr* instr) {
  // Nothing to do.
}


void ConstantPropagator::VisitThrow(ThrowInstr* instr) {
  // Nothing to do.
}


void ConstantPropagator::VisitReThrow(ReThrowInstr* instr) {
  // Nothing to do.
}


void ConstantPropagator::VisitGoto(GotoInstr* instr) {
  SetReachable(instr->successor());

  // Phi value depends on the reachability of a predecessor. We have
  // to revisit phis every time a predecessor becomes reachable.
  for (PhiIterator it(instr->successor()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}


void ConstantPropagator::VisitBranch(BranchInstr* instr) {
  instr->comparison()->Accept(this);

  // The successors may be reachable, but only if this instruction is.  (We
  // might be analyzing it because the constant value of one of its inputs
  // has changed.)
  if (reachable_->Contains(instr->GetBlock()->preorder_number())) {
    const Object& value = instr->comparison()->constant_value();
    if (IsNonConstant(value)) {
      SetReachable(instr->true_successor());
      SetReachable(instr->false_successor());
    } else if (value.raw() == Bool::True().raw()) {
      SetReachable(instr->true_successor());
    } else if (!IsUnknown(value)) {  // Any other constant.
      SetReachable(instr->false_successor());
    }
  }
}


// --------------------------------------------------------------------------
// Analysis of non-definition instructions.  They do not have values so they
// cannot have constant values.
void ConstantPropagator::VisitStoreContext(StoreContextInstr* instr) { }


void ConstantPropagator::VisitChainContext(ChainContextInstr* instr) { }


void ConstantPropagator::VisitCatchEntry(CatchEntryInstr* instr) { }


void ConstantPropagator::VisitCheckStackOverflow(
    CheckStackOverflowInstr* instr) { }


void ConstantPropagator::VisitCheckClass(CheckClassInstr* instr) { }


void ConstantPropagator::VisitCheckSmi(CheckSmiInstr* instr) { }


void ConstantPropagator::VisitCheckEitherNonSmi(
    CheckEitherNonSmiInstr* instr) { }


void ConstantPropagator::VisitCheckArrayBound(CheckArrayBoundInstr* instr) { }


// --------------------------------------------------------------------------
// Analysis of definitions.  Compute the constant value.  If it has changed
// and the definition has input uses, add the definition to the definition
// worklist so that the used can be processed.
void ConstantPropagator::VisitPhi(PhiInstr* instr) {
  // Compute the join over all the reachable predecessor values.
  JoinEntryInstr* block = instr->block();
  Object& value = Object::ZoneHandle(Unknown());
  for (intptr_t pred_idx = 0; pred_idx < instr->InputCount(); ++pred_idx) {
    if (reachable_->Contains(
            block->PredecessorAt(pred_idx)->preorder_number())) {
      Join(&value,
           instr->InputAt(pred_idx)->definition()->constant_value());
    }
  }
  SetValue(instr, value);
}


void ConstantPropagator::VisitParameter(ParameterInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitPushArgument(PushArgumentInstr* instr) {
  SetValue(instr, instr->value()->definition()->constant_value());
}


void ConstantPropagator::VisitAssertAssignable(AssertAssignableInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    // We are ignoring the instantiator and instantiator_type_arguments, but
    // still monotonic and safe.
    // TODO(kmillikin): Handle constants.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitAssertBoolean(AssertBooleanInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    // TODO(kmillikin): Handle assertion.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitArgumentDefinitionTest(
    ArgumentDefinitionTestInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitCurrentContext(CurrentContextInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitClosureCall(ClosureCallInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitInstanceCall(InstanceCallInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitPolymorphicInstanceCall(
    PolymorphicInstanceCallInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitStaticCall(StaticCallInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitLoadLocal(LoadLocalInstr* instr) {
  // Instruction is eliminated when translating to SSA.
  UNREACHABLE();
}


void ConstantPropagator::VisitStoreLocal(StoreLocalInstr* instr) {
  // Instruction is eliminated when translating to SSA.
  UNREACHABLE();
}


void ConstantPropagator::VisitStrictCompare(StrictCompareInstr* instr) {
  const Object& left = instr->left()->definition()->constant_value();
  const Object& right = instr->right()->definition()->constant_value();

  if (IsNonConstant(left) || IsNonConstant(right)) {
    // TODO(vegorov): incorporate nullability information into the lattice.
    if ((left.IsNull() && (instr->right()->ResultCid() != kDynamicCid)) ||
        (right.IsNull() && (instr->left()->ResultCid() != kDynamicCid))) {
      bool result = left.IsNull() ? (instr->right()->ResultCid() == kNullCid)
                                  : (instr->left()->ResultCid() == kNullCid);
      if (instr->kind() == Token::kNE_STRICT) result = !result;
      SetValue(instr, result ? Bool::True() : Bool::False());
    } else {
      SetValue(instr, non_constant_);
    }
  } else if (IsConstant(left) && IsConstant(right)) {
    bool result = (left.raw() == right.raw());
    if (instr->kind() == Token::kNE_STRICT) result = !result;
    SetValue(instr, result ? Bool::True() : Bool::False());
  }
}


static bool CompareIntegers(Token::Kind kind,
                            const Integer& left,
                            const Integer& right) {
  const int result = left.CompareWith(right);
  switch (kind) {
    case Token::kEQ: return (result == 0);
    case Token::kNE: return (result != 0);
    case Token::kLT: return (result < 0);
    case Token::kGT: return (result > 0);
    case Token::kLTE: return (result <= 0);
    case Token::kGTE: return (result >= 0);
    default:
      UNREACHABLE();
      return false;
  }
}


void ConstantPropagator::VisitEqualityCompare(EqualityCompareInstr* instr) {
  const Object& left = instr->left()->definition()->constant_value();
  const Object& right = instr->right()->definition()->constant_value();
  if (IsNonConstant(left) || IsNonConstant(right)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(left) && IsConstant(right)) {
    if (left.IsInteger() && right.IsInteger()) {
      const bool result = CompareIntegers(instr->kind(),
                                          Integer::Cast(left),
                                          Integer::Cast(right));
      SetValue(instr, result ? Bool::True() : Bool::False());
    } else {
      SetValue(instr, non_constant_);
    }
  }
}


void ConstantPropagator::VisitRelationalOp(RelationalOpInstr* instr) {
  const Object& left = instr->left()->definition()->constant_value();
  const Object& right = instr->right()->definition()->constant_value();
  if (IsNonConstant(left) || IsNonConstant(right)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(left) && IsConstant(right)) {
    if (left.IsInteger() && right.IsInteger()) {
      const bool result = CompareIntegers(instr->kind(),
                                          Integer::Cast(left),
                                          Integer::Cast(right));
      SetValue(instr, result ? Bool::True() : Bool::False());
    } else {
      SetValue(instr, non_constant_);
    }
  }
}


void ConstantPropagator::VisitNativeCall(NativeCallInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitStringFromCharCode(
    StringFromCharCodeInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitLoadIndexed(LoadIndexedInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitStoreIndexed(StoreIndexedInstr* instr) {
  SetValue(instr, instr->value()->definition()->constant_value());
}


void ConstantPropagator::VisitStoreInstanceField(
    StoreInstanceFieldInstr* instr) {
  SetValue(instr, instr->value()->definition()->constant_value());
}


void ConstantPropagator::VisitLoadStaticField(LoadStaticFieldInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitStoreStaticField(StoreStaticFieldInstr* instr) {
  SetValue(instr, instr->value()->definition()->constant_value());
}


void ConstantPropagator::VisitBooleanNegate(BooleanNegateInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    bool val = value.raw() != Bool::True().raw();
    SetValue(instr, val ? Bool::True() : Bool::False());
  }
}


void ConstantPropagator::VisitInstanceOf(InstanceOfInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    // TODO(kmillikin): Handle instanceof on constants.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitCreateArray(CreateArrayInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitCreateClosure(CreateClosureInstr* instr) {
  // TODO(kmillikin): Treat closures as constants.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitAllocateObject(AllocateObjectInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitAllocateObjectWithBoundsCheck(
    AllocateObjectWithBoundsCheckInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitLoadField(LoadFieldInstr* instr) {
  if ((instr->recognized_kind() == MethodRecognizer::kObjectArrayLength) &&
      (instr->value()->definition()->IsCreateArray())) {
    const intptr_t length =
        instr->value()->definition()->AsCreateArray()->ArgumentCount();
    const Object& result = Smi::ZoneHandle(Smi::New(length));
    SetValue(instr, result);
  } else {
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitStoreVMField(StoreVMFieldInstr* instr) {
  SetValue(instr, instr->value()->definition()->constant_value());
}


void ConstantPropagator::VisitInstantiateTypeArguments(
    InstantiateTypeArgumentsInstr* instr) {
  const Object& object =
      instr->instantiator()->definition()->constant_value();
  if (IsNonConstant(object)) {
    SetValue(instr, non_constant_);
    return;
  }
  if (IsConstant(object)) {
    const intptr_t len = instr->type_arguments().Length();
    if (instr->type_arguments().IsRawInstantiatedRaw(len) &&
        object.IsNull()) {
      SetValue(instr, object);
      return;
    }
    if (instr->type_arguments().IsUninstantiatedIdentity() &&
        !object.IsNull() &&
        object.IsTypeArguments() &&
        (TypeArguments::Cast(object).Length() == len)) {
      SetValue(instr, object);
      return;
    }
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitExtractConstructorTypeArguments(
    ExtractConstructorTypeArgumentsInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitExtractConstructorInstantiator(
    ExtractConstructorInstantiatorInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitAllocateContext(AllocateContextInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitCloneContext(CloneContextInstr* instr) {
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitBinarySmiOp(BinarySmiOpInstr* instr) {
  const Object& left = instr->left()->definition()->constant_value();
  const Object& right = instr->right()->definition()->constant_value();
  if (IsNonConstant(left) || IsNonConstant(right)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(left) && IsConstant(right)) {
    if (left.IsSmi() && right.IsSmi()) {
      const Smi& left_smi = Smi::Cast(left);
      const Smi& right_smi = Smi::Cast(right);
      switch (instr->op_kind()) {
        case Token::kADD:
        case Token::kSUB:
        case Token::kMUL:
        case Token::kTRUNCDIV:
        case Token::kMOD: {
          const Object& result = Integer::ZoneHandle(
              left_smi.ArithmeticOp(instr->op_kind(), right_smi));
          SetValue(instr, result);
          break;
        }
        case Token::kSHL:
        case Token::kSHR: {
          const Object& result = Integer::ZoneHandle(
              left_smi.ShiftOp(instr->op_kind(), right_smi));
          SetValue(instr, result);
          break;
        }
        case Token::kBIT_AND:
        case Token::kBIT_OR:
        case Token::kBIT_XOR: {
          const Object& result = Integer::ZoneHandle(
              left_smi.BitOp(instr->op_kind(), right_smi));
          SetValue(instr, result);
          break;
        }
        default:
          // TODO(kmillikin): support other smi operations.
          SetValue(instr, non_constant_);
      }
    } else {
      // TODO(kmillikin): support other types.
      SetValue(instr, non_constant_);
    }
  }
}


void ConstantPropagator::VisitBoxInteger(BoxIntegerInstr* instr) {
  // TODO(kmillikin): Handle box operation.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitUnboxInteger(UnboxIntegerInstr* instr) {
  // TODO(kmillikin): Handle unbox operation.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitBinaryMintOp(
    BinaryMintOpInstr* instr) {
  // TODO(kmillikin): Handle binary operations.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitShiftMintOp(
    ShiftMintOpInstr* instr) {
  // TODO(kmillikin): Handle shift operations.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitUnaryMintOp(
    UnaryMintOpInstr* instr) {
  // TODO(kmillikin): Handle unary operations.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitUnarySmiOp(UnarySmiOpInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    // TODO(kmillikin): Handle unary operations.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitSmiToDouble(SmiToDoubleInstr* instr) {
  // TODO(kmillikin): Handle conversion.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitDoubleToInteger(DoubleToIntegerInstr* instr) {
  // TODO(kmillikin): Handle conversion.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitDoubleToSmi(DoubleToSmiInstr* instr) {
  // TODO(kmillikin): Handle conversion.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitDoubleToDouble(DoubleToDoubleInstr* instr) {
  // TODO(kmillikin): Handle conversion.
  SetValue(instr, non_constant_);
}


void ConstantPropagator::VisitInvokeMathCFunction(
    InvokeMathCFunctionInstr* instr) {
  // TODO(kmillikin): Handle conversion.
  SetValue(instr, non_constant_);
}

void ConstantPropagator::VisitConstant(ConstantInstr* instr) {
  SetValue(instr, instr->value());
}


void ConstantPropagator::VisitConstraint(ConstraintInstr* instr) {
  // Should not be used outside of range analysis.
  UNREACHABLE();
}


void ConstantPropagator::VisitBinaryDoubleOp(
    BinaryDoubleOpInstr* instr) {
  const Object& left = instr->left()->definition()->constant_value();
  const Object& right = instr->right()->definition()->constant_value();
  if (IsNonConstant(left) || IsNonConstant(right)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(left) && IsConstant(right)) {
    // TODO(kmillikin): Handle binary operation.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitMathSqrt(MathSqrtInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    // TODO(kmillikin): Handle sqrt.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitUnboxDouble(UnboxDoubleInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    // TODO(kmillikin): Handle conversion.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::VisitBoxDouble(BoxDoubleInstr* instr) {
  const Object& value = instr->value()->definition()->constant_value();
  if (IsNonConstant(value)) {
    SetValue(instr, non_constant_);
  } else if (IsConstant(value)) {
    // TODO(kmillikin): Handle conversion.
    SetValue(instr, non_constant_);
  }
}


void ConstantPropagator::Analyze() {
  GraphEntryInstr* entry = graph_->graph_entry();
  reachable_->Add(entry->preorder_number());
  block_worklist_.Add(entry);

  while (true) {
    if (block_worklist_.is_empty()) {
      if (definition_worklist_.is_empty()) break;
      Definition* definition = definition_worklist_.RemoveLast();
      definition_marks_->Remove(definition->ssa_temp_index());
      Value* use = definition->input_use_list();
      while (use != NULL) {
        use->instruction()->Accept(this);
        use = use->next_use();
      }
    } else {
      BlockEntryInstr* block = block_worklist_.RemoveLast();
      block->Accept(this);
    }
  }
}


void ConstantPropagator::Transform() {
  if (FLAG_trace_constant_propagation) {
    OS::Print("\n==== Before constant propagation ====\n");
    FlowGraphPrinter printer(*graph_);
    printer.PrintBlocks();
  }

  GrowableArray<PhiInstr*> redundant_phis(10);

  // We will recompute dominators, block ordering, block ids, block last
  // instructions, previous pointers, predecessors, etc. after eliminating
  // unreachable code.  We do not maintain those properties during the
  // transformation.
  for (BlockIterator b = graph_->reverse_postorder_iterator();
       !b.Done();
       b.Advance()) {
    BlockEntryInstr* block = b.Current();
    if (!reachable_->Contains(block->preorder_number())) {
      if (FLAG_trace_constant_propagation) {
        OS::Print("Unreachable B%"Pd"\n", block->block_id());
      }
      continue;
    }

    JoinEntryInstr* join = block->AsJoinEntry();
    if (join != NULL) {
      // Remove phi inputs corresponding to unreachable predecessor blocks.
      // Predecessors will be recomputed (in block id order) after removing
      // unreachable code so we merely have to keep the phi inputs in order.
      ZoneGrowableArray<PhiInstr*>* phis = join->phis();
      if (phis != NULL) {
        intptr_t pred_count = join->PredecessorCount();
        intptr_t live_count = 0;
        for (intptr_t pred_idx = 0; pred_idx < pred_count; ++pred_idx) {
          if (reachable_->Contains(
                  join->PredecessorAt(pred_idx)->preorder_number())) {
            if (live_count < pred_idx) {
              for (intptr_t phi_idx = 0; phi_idx < phis->length(); ++phi_idx) {
                PhiInstr* phi = (*phis)[phi_idx];
                if (phi == NULL) continue;
                phi->inputs_[live_count] = phi->inputs_[pred_idx];
              }
            }
            ++live_count;
          }
        }
        if (live_count < pred_count) {
          for (intptr_t phi_idx = 0; phi_idx < phis->length(); ++phi_idx) {
            PhiInstr* phi = (*phis)[phi_idx];
            if (phi == NULL) continue;
            phi->inputs_.TruncateTo(live_count);
            if (live_count == 1) redundant_phis.Add(phi);
          }
        }
      }
    }

    for (ForwardInstructionIterator i(block); !i.Done(); i.Advance()) {
      Definition* defn = i.Current()->AsDefinition();
      // Replace constant-valued instructions without observable side
      // effects.  Do this for smis only to avoid having to copy other
      // objects into the heap's old generation.
      //
      // TODO(kmillikin): Extend this to handle booleans, other number
      // types, etc.
      if ((defn != NULL) &&
          (defn->constant_value().IsSmi() ||
           defn->constant_value().IsNull() ||
           defn->constant_value().IsTypeArguments()) &&
          !defn->IsConstant() &&
          !defn->IsPushArgument() &&
          !defn->IsStoreIndexed() &&
          !defn->IsStoreInstanceField() &&
          !defn->IsStoreStaticField() &&
          !defn->IsStoreVMField()) {
        if (FLAG_trace_constant_propagation) {
          OS::Print("Constant v%"Pd" = %s\n",
                    defn->ssa_temp_index(),
                    defn->constant_value().ToCString());
        }
        i.ReplaceCurrentWith(new ConstantInstr(defn->constant_value()));
      }
    }

    // Replace branches where one target is unreachable with jumps.
    BranchInstr* branch = block->last_instruction()->AsBranch();
    if (branch != NULL) {
      TargetEntryInstr* if_true = branch->true_successor();
      TargetEntryInstr* if_false = branch->false_successor();
      JoinEntryInstr* join = NULL;
      Instruction* next = NULL;

      if (!reachable_->Contains(if_true->preorder_number())) {
        ASSERT(reachable_->Contains(if_false->preorder_number()));
        ASSERT(if_false->parallel_move() == NULL);
        ASSERT(if_false->loop_info() == NULL);
        join = new JoinEntryInstr(if_false->block_id(), if_false->try_index());
        next = if_false->next();
      } else if (!reachable_->Contains(if_false->preorder_number())) {
        ASSERT(if_true->parallel_move() == NULL);
        ASSERT(if_true->loop_info() == NULL);
        join = new JoinEntryInstr(if_true->block_id(), if_true->try_index());
        next = if_true->next();
      }

      if (join != NULL) {
        // Replace the branch with a jump to the reachable successor.
        // Drop the comparison, which does not have side effects as long
        // as it is a strict compare (the only one we can determine is
        // constant with the current analysis).
        GotoInstr* jump = new GotoInstr(join);
        Instruction* previous = branch->previous();
        branch->set_previous(NULL);
        previous->LinkTo(jump);
        // Replace the false target entry with the new join entry. We will
        // recompute the dominators after this pass.
        join->LinkTo(next);
      }
    }
  }

  graph_->DiscoverBlocks();
  GrowableArray<BitVector*> dominance_frontier;
  graph_->ComputeDominators(&dominance_frontier);
  graph_->ComputeUseLists();

  if (FLAG_remove_redundant_phis) {
    for (intptr_t i = 0; i < redundant_phis.length(); i++) {
      PhiInstr* phi = redundant_phis[i];
      phi->ReplaceUsesWith(phi->InputAt(0)->definition());
      phi->mark_dead();
    }
  }

  if (FLAG_trace_constant_propagation) {
    OS::Print("\n==== After constant propagation ====\n");
    FlowGraphPrinter printer(*graph_);
    printer.PrintBlocks();
  }
}


}  // namespace dart
