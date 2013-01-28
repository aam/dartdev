// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
//
// The intrinsic code below is executed before a method has built its frame.
// The return address is on the stack and the arguments below it.
// Registers EDX (arguments descriptor) and ECX (function) must be preserved.
// Each intrinsification method returns true if the corresponding
// Dart method was intrinsified.

#include "vm/globals.h"  // Needed here to get TARGET_ARCH_IA32.
#if defined(TARGET_ARCH_IA32)

#include "vm/intrinsifier.h"

#include "vm/assembler.h"
#include "vm/assembler_macros.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/os.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"

namespace dart {

DECLARE_FLAG(bool, enable_type_checks);


#define __ assembler->

bool Intrinsifier::ObjectArray_Allocate(Assembler* assembler) {
  // This snippet of inlined code uses the following registers:
  // EAX, EBX, EDI
  // and the newly allocated object is returned in EAX.
  const intptr_t kTypeArgumentsOffset = 2 * kWordSize;
  const intptr_t kArrayLengthOffset = 1 * kWordSize;
  Label fall_through;

  // Compute the size to be allocated, it is based on the array length
  // and is computed as:
  // RoundedAllocationSize((array_length * kwordSize) + sizeof(RawArray)).
  __ movl(EDI, Address(ESP, kArrayLengthOffset));  // Array length.
  // Check that length is a positive Smi.
  __ testl(EDI, Immediate(kSmiTagSize));
  __ j(NOT_ZERO, &fall_through);
  __ cmpl(EDI, Immediate(0));
  __ j(LESS, &fall_through);
  // Check for maximum allowed length.
  const Immediate& max_len =
      Immediate(reinterpret_cast<int32_t>(Smi::New(Array::kMaxElements)));
  __ cmpl(EDI, max_len);
  __ j(GREATER, &fall_through);
  const intptr_t fixed_size = sizeof(RawArray) + kObjectAlignment - 1;
  __ leal(EDI, Address(EDI, TIMES_2, fixed_size));  // EDI is a Smi.
  ASSERT(kSmiTagShift == 1);
  __ andl(EDI, Immediate(-kObjectAlignment));

  Isolate* isolate = Isolate::Current();
  Heap* heap = isolate->heap();

  __ movl(EAX, Address::Absolute(heap->TopAddress()));
  __ movl(EBX, EAX);

  // EDI: allocation size.
  __ addl(EBX, EDI);
  __ j(CARRY, &fall_through);

  // Check if the allocation fits into the remaining space.
  // EAX: potential new object start.
  // EBX: potential next object start.
  // EDI: allocation size.
  __ cmpl(EBX, Address::Absolute(heap->EndAddress()));
  __ j(ABOVE_EQUAL, &fall_through);

  // Successfully allocated the object(s), now update top to point to
  // next object start and initialize the object.
  __ movl(Address::Absolute(heap->TopAddress()), EBX);
  __ addl(EAX, Immediate(kHeapObjectTag));

  // Initialize the tags.
  // EAX: new object start as a tagged pointer.
  // EBX: new object end address.
  // EDI: allocation size.
  {
    Label size_tag_overflow, done;
    __ cmpl(EDI, Immediate(RawObject::SizeTag::kMaxSizeTag));
    __ j(ABOVE, &size_tag_overflow, Assembler::kNearJump);
    __ shll(EDI, Immediate(RawObject::kSizeTagBit - kObjectAlignmentLog2));
    __ jmp(&done, Assembler::kNearJump);

    __ Bind(&size_tag_overflow);
    __ movl(EDI, Immediate(0));
    __ Bind(&done);

    // Get the class index and insert it into the tags.
    const Class& cls = Class::Handle(isolate->object_store()->array_class());
    __ orl(EDI, Immediate(RawObject::ClassIdTag::encode(cls.id())));
    __ movl(FieldAddress(EAX, Array::tags_offset()), EDI);  // Tags.
  }

  // EAX: new object start as a tagged pointer.
  // EBX: new object end address.
  // Store the type argument field.
  __ movl(EDI, Address(ESP, kTypeArgumentsOffset));  // type argument.
  __ StoreIntoObjectNoBarrier(EAX,
                              FieldAddress(EAX, Array::type_arguments_offset()),
                              EDI);

  // Set the length field.
  __ movl(EDI, Address(ESP, kArrayLengthOffset));  // Array Length.
  __ StoreIntoObjectNoBarrier(EAX,
                              FieldAddress(EAX, Array::length_offset()),
                              EDI);

  // Initialize all array elements to raw_null.
  // EAX: new object start as a tagged pointer.
  // EBX: new object end address.
  // EDI: iterator which initially points to the start of the variable
  // data area to be initialized.
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ leal(EDI, FieldAddress(EAX, sizeof(RawArray)));
  Label done;
  Label init_loop;
  __ Bind(&init_loop);
  __ cmpl(EDI, EBX);
  __ j(ABOVE_EQUAL, &done, Assembler::kNearJump);
  __ movl(Address(EDI, 0), raw_null);
  __ addl(EDI, Immediate(kWordSize));
  __ jmp(&init_loop, Assembler::kNearJump);
  __ Bind(&done);
  __ ret();  // returns the newly allocated object in EAX.

  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Array_getLength(Assembler* assembler) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ movl(EAX, FieldAddress(EAX, Array::length_offset()));
  __ ret();
  return true;
}


bool Intrinsifier::ImmutableArray_getLength(Assembler* assembler) {
  return Array_getLength(assembler);
}


bool Intrinsifier::Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Index.
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Array.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpl(EBX, FieldAddress(EAX, Array::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  // Note that EBX is Smi, i.e, times 2.
  ASSERT(kSmiTagShift == 1);
  __ movl(EAX, FieldAddress(EAX, EBX, TIMES_2, Array::data_offset()));
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::ImmutableArray_getIndexed(Assembler* assembler) {
  return Array_getIndexed(assembler);
}


static intptr_t ComputeObjectArrayTypeArgumentsOffset() {
  const Library& core_lib = Library::Handle(Library::CoreLibrary());
  const Class& cls =
      Class::Handle(core_lib.LookupClassAllowPrivate(Symbols::ObjectArray()));
  ASSERT(!cls.IsNull());
  ASSERT(cls.HasTypeArguments());
  ASSERT(cls.NumTypeArguments() == 1);
  const intptr_t field_offset = cls.type_arguments_field_offset();
  ASSERT(field_offset != Class::kNoTypeArguments);
  return field_offset;
}


// Intrinsify only for Smi value and index. Non-smi values need a store buffer
// update. Array length is always a Smi.
bool Intrinsifier::Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  if (FLAG_enable_type_checks) {
    const intptr_t type_args_field_offset =
        ComputeObjectArrayTypeArgumentsOffset();
    // Inline simple tests (Smi, null), fallthrough if not positive.
    const Immediate& raw_null =
        Immediate(reinterpret_cast<intptr_t>(Object::null()));
    Label checked_ok;
    __ movl(EDI, Address(ESP, + 1 * kWordSize));  // Value.
    // Null value is valid for any type.
    __ cmpl(EDI, raw_null);
    __ j(EQUAL, &checked_ok, Assembler::kNearJump);

    __ movl(EBX, Address(ESP, + 3 * kWordSize));  // Array.
    __ movl(EBX, FieldAddress(EBX, type_args_field_offset));
    // EBX: Type arguments of array.
    __ cmpl(EBX, raw_null);
    __ j(EQUAL, &checked_ok, Assembler::kNearJump);
    // Check if it's dynamic.
    // For now handle only TypeArguments and bail out if InstantiatedTypeArgs.
    __ CompareClassId(EBX, kTypeArgumentsCid, EAX);
    __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
    // Get type at index 0.
    __ movl(EAX, FieldAddress(EBX, TypeArguments::type_at_offset(0)));
    __ CompareObject(EAX, Type::ZoneHandle(Type::DynamicType()));
    __ j(EQUAL,  &checked_ok, Assembler::kNearJump);
    // Check for int and num.
    __ testl(EDI, Immediate(kSmiTagMask));  // Value is Smi?
    __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi value.
    __ CompareObject(EAX, Type::ZoneHandle(Type::IntType()));
    __ j(EQUAL,  &checked_ok, Assembler::kNearJump);
    __ CompareObject(EAX, Type::ZoneHandle(Type::Number()));
    __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
    __ Bind(&checked_ok);
  }
  __ movl(EBX, Address(ESP, + 2 * kWordSize));  // Index.
  __ testl(EBX, Immediate(kSmiTagMask));
  // Index not Smi.
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  __ movl(EAX, Address(ESP, + 3 * kWordSize));  // Array.
  // Range check.
  __ cmpl(EBX, FieldAddress(EAX, Array::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  // Note that EBX is Smi, i.e, times 2.
  ASSERT(kSmiTagShift == 1);
  // Destroy ECX as we will not continue in the function.
  __ movl(ECX, Address(ESP, + 1 * kWordSize));  // Value.
  __ StoreIntoObject(EAX,
                     FieldAddress(EAX, EBX, TIMES_2, Array::data_offset()),
                     ECX);
  // Caller is responsible of preserving the value if necessary.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Allocate a GrowableObjectArray using the backing array specified.
// On stack: type argument (+2), data (+1), return-address (+0).
bool Intrinsifier::GArray_Allocate(Assembler* assembler) {
  // This snippet of inlined code uses the following registers:
  // EAX, EBX
  // and the newly allocated object is returned in EAX.
  const intptr_t kTypeArgumentsOffset = 2 * kWordSize;
  const intptr_t kArrayOffset = 1 * kWordSize;
  Label fall_through;

  // Compute the size to be allocated, it is based on the array length
  // and is computed as:
  // RoundedAllocationSize(sizeof(RawGrowableObjectArray)) +
  intptr_t fixed_size = GrowableObjectArray::InstanceSize();

  Isolate* isolate = Isolate::Current();
  Heap* heap = isolate->heap();

  __ movl(EAX, Address::Absolute(heap->TopAddress()));
  __ leal(EBX, Address(EAX, fixed_size));

  // Check if the allocation fits into the remaining space.
  // EAX: potential new backing array object start.
  // EBX: potential next object start.
  __ cmpl(EBX, Address::Absolute(heap->EndAddress()));
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);

  // Successfully allocated the object(s), now update top to point to
  // next object start and initialize the object.
  __ movl(Address::Absolute(heap->TopAddress()), EBX);
  __ addl(EAX, Immediate(kHeapObjectTag));

  // Initialize the tags.
  // EAX: new growable array object start as a tagged pointer.
  const Class& cls = Class::Handle(
      isolate->object_store()->growable_object_array_class());
  uword tags = 0;
  tags = RawObject::SizeTag::update(fixed_size, tags);
  tags = RawObject::ClassIdTag::update(cls.id(), tags);
  __ movl(FieldAddress(EAX, GrowableObjectArray::tags_offset()),
          Immediate(tags));

  // Store backing array object in growable array object.
  __ movl(EBX, Address(ESP, kArrayOffset));  // data argument.
  __ StoreIntoObject(EAX,
                     FieldAddress(EAX, GrowableObjectArray::data_offset()),
                     EBX);

  // EAX: new growable array object start as a tagged pointer.
  // Store the type argument field in the growable array object.
  __ movl(EBX, Address(ESP, kTypeArgumentsOffset));  // type argument.
  __ StoreIntoObjectNoBarrier(
      EAX,
      FieldAddress(EAX, GrowableObjectArray::type_arguments_offset()),
      EBX);

  // Set the length field in the growable array object to 0.
  __ movl(FieldAddress(EAX, GrowableObjectArray::length_offset()),
          Immediate(0));
  __ ret();  // returns the newly allocated object in EAX.

  __ Bind(&fall_through);
  return false;
}


// Get length of growable object array.
// On stack: growable array (+1), return-address (+0).
bool Intrinsifier::GrowableArray_getLength(Assembler* assembler) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ movl(EAX, FieldAddress(EAX, GrowableObjectArray::length_offset()));
  __ ret();
  return true;
}


// Get capacity of growable object array.
// On stack: growable array (+1), return-address (+0).
bool Intrinsifier::GrowableArray_getCapacity(Assembler* assembler) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ movl(EAX, FieldAddress(EAX, GrowableObjectArray::data_offset()));
  __ movl(EAX, FieldAddress(EAX, Array::length_offset()));
  __ ret();
  return true;
}


// Access growable object array at specified index.
// On stack: growable array (+2), index (+1), return-address (+0).
bool Intrinsifier::GrowableArray_getIndexed(Assembler* assembler) {
  Label fall_through;
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Index.
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // GrowableArray.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check using _length field.
  __ cmpl(EBX, FieldAddress(EAX, GrowableObjectArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  __ movl(EAX, FieldAddress(EAX, GrowableObjectArray::data_offset()));  // data.

  // Note that EBX is Smi, i.e, times 2.
  ASSERT(kSmiTagShift == 1);
  __ movl(EAX, FieldAddress(EAX, EBX, TIMES_2, Array::data_offset()));
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Set value into growable object array at specified index.
// On stack: growable array (+3), index (+2), value (+1), return-address (+0).
bool Intrinsifier::GrowableArray_setIndexed(Assembler* assembler) {
  if (FLAG_enable_type_checks) {
    return false;
  }
  Label fall_through;
  __ movl(EBX, Address(ESP, + 2 * kWordSize));  // Index.
  __ movl(EAX, Address(ESP, + 3 * kWordSize));  // GrowableArray.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check using _length field.
  __ cmpl(EBX, FieldAddress(EAX, GrowableObjectArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  __ movl(EAX, FieldAddress(EAX, GrowableObjectArray::data_offset()));  // data.
  __ movl(EDI, Address(ESP, + 1 * kWordSize));  // Value.
  // Note that EBX is Smi, i.e, times 2.
  ASSERT(kSmiTagShift == 1);
  __ StoreIntoObject(EAX,
                     FieldAddress(EAX, EBX, TIMES_2, Array::data_offset()),
                     EDI);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Set length of growable object array.
// On stack: growable array (+2), length (+1), return-address (+0).
bool Intrinsifier::GrowableArray_setLength(Assembler* assembler) {
  Label fall_through;
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Growable array.
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Length value.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi length.
  __ movl(EDI, FieldAddress(EAX, GrowableObjectArray::data_offset()));
  __ cmpl(EBX, FieldAddress(EDI, Array::length_offset()));
  __ j(ABOVE, &fall_through, Assembler::kNearJump);
  __ movl(FieldAddress(EAX, GrowableObjectArray::length_offset()), EBX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Set data of growable object array.
// On stack: growable array (+2), data (+1), return-address (+0).
bool Intrinsifier::GrowableArray_setData(Assembler* assembler) {
  if (FLAG_enable_type_checks) {
    return false;
  }
  Label fall_through;
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Data.
  // Check that data is an ObjectArray.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(ZERO, &fall_through, Assembler::kNearJump);  // Data is Smi.
  __ CompareClassId(EBX, kArrayCid, EAX);
  __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Growable array.
  __ StoreIntoObject(EAX,
                     FieldAddress(EAX, GrowableObjectArray::data_offset()),
                     EBX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Add an element to growable array if it doesn't need to grow, otherwise
// call into regular code.
// On stack: growable array (+2), value (+1), return-address (+0).
bool Intrinsifier::GrowableArray_add(Assembler* assembler) {
  // In checked mode we need to type-check the incoming argument.
  if (FLAG_enable_type_checks) return false;
  Label fall_through;
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Array.
  __ movl(EBX, FieldAddress(EAX, GrowableObjectArray::length_offset()));
  // EBX: length.
  __ movl(EDI, FieldAddress(EAX, GrowableObjectArray::data_offset()));
  // EDI: data.
  // Compare length with capacity.
  __ cmpl(EBX, FieldAddress(EDI, Array::length_offset()));
  __ j(EQUAL, &fall_through, Assembler::kNearJump);  // Must grow data.
  const Immediate& value_one =
      Immediate(reinterpret_cast<int32_t>(Smi::New(1)));
  // len = len + 1;
  __ addl(FieldAddress(EAX, GrowableObjectArray::length_offset()), value_one);
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // Value
  ASSERT(kSmiTagShift == 1);
  __ StoreIntoObject(EDI,
                     FieldAddress(EDI, EBX, TIMES_2, Array::data_offset()),
                     EAX);
  const Immediate& raw_null =
      Immediate(reinterpret_cast<int32_t>(Object::null()));
  __ movl(EAX, raw_null);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Gets the length of a ByteArray.
bool Intrinsifier::ByteArrayBase_getLength(Assembler* assembler) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ movl(EAX, FieldAddress(EAX, ByteArray::length_offset()));
  __ ret();
  return true;
}


// Tests if index is a valid length (Smi and within valid index range),
// jumps to fall_through if it is not.
// Returns index in EBX, array in EAX.
// This should be used only on getIndexed intrinsics.
static void TestByteArrayGetIndex(Assembler* assembler, Label* fall_through) {
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Array.
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Index.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpl(EBX, FieldAddress(EAX, ByteArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, fall_through, Assembler::kNearJump);
}


// Tests if index is a valid length (Smi and within valid index range),
// jumps to fall_through if it is not.
// Returns index in EBX, array in EAX.
// This should be used only for setIndexed intrinsics.
static void TestByteArraySetIndex(Assembler* assembler, Label* fall_through) {
  __ movl(EAX, Address(ESP, + 3 * kWordSize));  // Array.
  __ movl(EBX, Address(ESP, + 2 * kWordSize));  // Index.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpl(EBX, FieldAddress(EAX, ByteArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, fall_through, Assembler::kNearJump);
}


bool Intrinsifier::Int8Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ SmiUntag(EBX);
  __ movsxb(EAX, FieldAddress(EAX,
                              EBX,
                              TIMES_1,
                              Int8Array::data_offset()));
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Int8Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ SmiUntag(EBX);
  // Free EBX for the value since we want a byte register.
  __ movl(EDI, EBX);
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Value.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  __ SmiUntag(EBX);
  // Check that the value is a byte. Add 128 to EBX to bring it into
  // the range 0..FF.
  __ addl(EBX, Immediate(128));
  __ cmpl(EBX, Immediate(0xFF));
  __ j(ABOVE, &fall_through, Assembler::kNearJump);
  // Undo addition.
  __ subl(EBX, Immediate(128));
  __ movb(FieldAddress(EAX, EDI, TIMES_1, Int8Array::data_offset()), BL);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


#define TYPED_ARRAY_ALLOCATION(type_name, scale_factor)                        \
  Label fall_through;                                                          \
  const intptr_t kArrayLengthStackOffset = 1 * kWordSize;                      \
  __ movl(EDI, Address(ESP, kArrayLengthStackOffset));  /* Array length. */    \
  /* Check that length is a positive Smi. */                                   \
  /* EDI: requested array length argument. */                                  \
  __ testl(EDI, Immediate(kSmiTagSize));                                       \
  __ j(NOT_ZERO, &fall_through);                                               \
  __ cmpl(EDI, Immediate(0));                                                  \
  __ j(LESS, &fall_through);                                                   \
  __ SmiUntag(EDI);                                                            \
  /* Check for maximum allowed length. */                                      \
  /* EDI: untagged array length. */                                            \
  __ cmpl(EDI, Immediate(type_name::kMaxElements));                            \
  __ j(GREATER, &fall_through);                                                \
  const intptr_t fixed_size = sizeof(Raw##type_name) + kObjectAlignment - 1;   \
  __ leal(EDI, Address(EDI, scale_factor, fixed_size));                        \
  __ andl(EDI, Immediate(-kObjectAlignment));                                  \
  Heap* heap = Isolate::Current()->heap();                                     \
                                                                               \
  __ movl(EAX, Address::Absolute(heap->TopAddress()));                         \
  __ movl(EBX, EAX);                                                           \
                                                                               \
  /* EDI: allocation size. */                                                  \
  __ addl(EBX, EDI);                                                           \
  __ j(CARRY, &fall_through);                                                  \
                                                                               \
  /* Check if the allocation fits into the remaining space. */                 \
  /* EAX: potential new object start. */                                       \
  /* EBX: potential next object start. */                                      \
  /* EDI: allocation size. */                                                  \
  __ cmpl(EBX, Address::Absolute(heap->EndAddress()));                         \
  __ j(ABOVE_EQUAL, &fall_through);                                            \
                                                                               \
  /* Successfully allocated the object(s), now update top to point to */       \
  /* next object start and initialize the object. */                           \
  __ movl(Address::Absolute(heap->TopAddress()), EBX);                         \
  __ addl(EAX, Immediate(kHeapObjectTag));                                     \
                                                                               \
  /* Initialize the tags. */                                                   \
  /* EAX: new object start as a tagged pointer. */                             \
  /* EBX: new object end address. */                                           \
  /* EDI: allocation size. */                                                  \
  {                                                                            \
    Label size_tag_overflow, done;                                             \
    __ cmpl(EDI, Immediate(RawObject::SizeTag::kMaxSizeTag));                  \
    __ j(ABOVE, &size_tag_overflow, Assembler::kNearJump);                     \
    __ shll(EDI, Immediate(RawObject::kSizeTagBit - kObjectAlignmentLog2));    \
    __ jmp(&done, Assembler::kNearJump);                                       \
                                                                               \
    __ Bind(&size_tag_overflow);                                               \
    __ movl(EDI, Immediate(0));                                                \
    __ Bind(&done);                                                            \
                                                                               \
    /* Get the class index and insert it into the tags. */                     \
    __ orl(EDI, Immediate(RawObject::ClassIdTag::encode(k##type_name##Cid)));  \
    __ movl(FieldAddress(EAX, type_name::tags_offset()), EDI);  /* Tags. */    \
  }                                                                            \
  /* Set the length field. */                                                  \
  /* EAX: new object start as a tagged pointer. */                             \
  /* EBX: new object end address. */                                           \
  __ movl(EDI, Address(ESP, kArrayLengthStackOffset));  /* Array length. */    \
  __ StoreIntoObjectNoBarrier(EAX,                                             \
                              FieldAddress(EAX, type_name::length_offset()),   \
                              EDI);                                            \
  /* Initialize all array elements to 0. */                                    \
  /* EAX: new object start as a tagged pointer. */                             \
  /* EBX: new object end address. */                                           \
  /* EDI: iterator which initially points to the start of the variable */      \
  /* ECX: scratch register. */                                                 \
  /* data area to be initialized. */                                           \
  __ xorl(ECX, ECX);  /* Zero. */                                              \
  __ leal(EDI, FieldAddress(EAX, sizeof(Raw##type_name)));                     \
  Label done, init_loop;                                                       \
  __ Bind(&init_loop);                                                         \
  __ cmpl(EDI, EBX);                                                           \
  __ j(ABOVE_EQUAL, &done, Assembler::kNearJump);                              \
  __ movl(Address(EDI, 0), ECX);                                               \
  __ addl(EDI, Immediate(kWordSize));                                          \
  __ jmp(&init_loop, Assembler::kNearJump);                                    \
  __ Bind(&done);                                                              \
                                                                               \
  __ ret();                                                                    \
  __ Bind(&fall_through);                                                      \


bool Intrinsifier::Int8Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Int8Array, TIMES_1);
  return false;
}


bool Intrinsifier::Uint8Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ SmiUntag(EBX);
  __ movzxb(EAX, FieldAddress(EAX,
                              EBX,
                              TIMES_1,
                              Uint8Array::data_offset()));
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint8Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ SmiUntag(EBX);
  // Free EBX for the value since we want a byte register.
  __ movl(EDI, EBX);
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Value.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  __ SmiUntag(EBX);
  // Check that the value is a byte.
  __ cmpl(EBX, Immediate(0xFF));
  __ j(ABOVE, &fall_through, Assembler::kNearJump);
  __ movb(FieldAddress(EAX, EDI, TIMES_1, Uint8Array::data_offset()), BL);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint8Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Uint8Array, TIMES_1);
  return false;
}


bool Intrinsifier::UintClamped8Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ SmiUntag(EBX);
  __ movzxb(EAX, FieldAddress(EAX,
                              EBX,
                              TIMES_1,
                              Uint8ClampedArray::data_offset()));
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint8ClampedArray_setIndexed(Assembler* assembler) {
  Label fall_through, store_value, load_0xff;
  TestByteArraySetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ SmiUntag(EBX);
  // Free EBX for the value since we need a byte register.
  __ leal(EAX, FieldAddress(EAX, EBX, TIMES_1,
      Uint8ClampedArray::data_offset()));
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Value.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);

  __ SmiUntag(EBX);
  __ cmpl(EBX, Immediate(0xFF));
  __ j(BELOW_EQUAL, &store_value, Assembler::kNearJump);
  // Clamp to 0x00 or 0xFF respectively.
  __ j(GREATER, &load_0xff,  Assembler::kNearJump);
  __ xorl(EBX, EBX);  // Zero.
  __ jmp(&store_value, Assembler::kNearJump);
  __ Bind(&load_0xff);
  __ movl(EBX, Immediate(0xFF));

  __ Bind(&store_value);
  __ movb(Address(EAX, 0), BL);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint8ClampedArray_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Uint8ClampedArray, TIMES_1);
  return false;
}


bool Intrinsifier::Int16Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ movsxw(EAX, FieldAddress(EAX,
                              EBX,
                              TIMES_1,
                              Int16Array::data_offset()));
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Int16Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Int16Array, TIMES_2);
  return false;
}


bool Intrinsifier::Uint16Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ movzxw(EAX, FieldAddress(EAX,
                              EBX,
                              TIMES_1,
                              Uint16Array::data_offset()));
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint16Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ movl(EDI, Address(ESP, + 1 * kWordSize));
  __ SmiUntag(EDI);
  // EDI: undtagged value.
  __ testl(EDI, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  __ movw(FieldAddress(EAX, EBX, TIMES_1, Uint16Array::data_offset()), EDI);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint16Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Uint16Array, TIMES_2);
  return false;
}


bool Intrinsifier::Int32Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ movl(EAX, FieldAddress(EAX,
                            EBX,
                            TIMES_2,
                            Int32Array::data_offset()));
  // Verify that the signed value in EAX can fit inside a Smi.
  __ cmpl(EAX, Immediate(0xC0000000));
  __ j(NEGATIVE, &fall_through, Assembler::kNearJump);  // Won't fit Smi.
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Int32Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Int32Array, TIMES_4);
  return false;
}


bool Intrinsifier::Uint32Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ movl(EAX, FieldAddress(EAX,
                            EBX,
                            TIMES_2,
                            Uint32Array::data_offset()));
  // Verify that the unsigned value in EAX can be stored in a Smi.
  __ testl(EAX,  Immediate(0xC0000000));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Won't fit Smi.
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint32Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Uint32Array, TIMES_4);
  return false;
}


bool Intrinsifier::Int64Array_getIndexed(Assembler* assembler) {
  return false;
}


bool Intrinsifier::Int64Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Int64Array, TIMES_8);
  return false;
}


bool Intrinsifier::Uint64Array_getIndexed(Assembler* assembler) {
  return false;
}


bool Intrinsifier::Uint64Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Uint64Array, TIMES_8);
  return false;
}


bool Intrinsifier::Float32Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  // Load single precision float into XMM7.
  __ movss(XMM7, FieldAddress(EAX, EBX, TIMES_2,
                              Float32Array::data_offset()));
  // Convert into a double precision float.
  __ cvtss2sd(XMM7, XMM7);
  // Allocate a double instance.
  const Class& double_class = Class::Handle(
                          Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump, EAX);
  // Store XMM7 into double instance.
  __ movsd(FieldAddress(EAX, Double::value_offset()), XMM7);
  __ ret();
  __ Bind(&fall_through);

  return false;
}


bool Intrinsifier::Float32Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // Value.
  // If EAX is not an instance of double, jump to fall through.
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(ZERO, &fall_through);
  __ CompareClassId(EAX, kDoubleCid, EDI);
  __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
  // Load double value into XMM7.
  __ movsd(XMM7, FieldAddress(EAX, Double::value_offset()));
  TestByteArraySetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  // Convert from double precision float to single precision float.
  __ cvtsd2ss(XMM7, XMM7);
  // Store into array.
  __ movss(FieldAddress(EAX, EBX, TIMES_2, Float32Array::data_offset()), XMM7);
  // End fast path.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Float32Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Float32Array, TIMES_4);
  return false;
}


bool Intrinsifier::Float64Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  // Load double precision float into XMM7.
  __ movsd(XMM7, FieldAddress(EAX, EBX, TIMES_4,
                              Float64Array::data_offset()));
  // Allocate a double instance.
  const Class& double_class = Class::Handle(
    Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump, EAX);
  // Store XMM7 into double instance.
  __ movsd(FieldAddress(EAX, Double::value_offset()), XMM7);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Float64Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // Value.
  // If EAX is not an instance of double, jump to fall through.
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(ZERO, &fall_through, Assembler::kNearJump);
  __ CompareClassId(EAX, kDoubleCid, EDI);
  __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
  // Load double value into XMM7.
  __ movsd(XMM7, FieldAddress(EAX, Double::value_offset()));
  TestByteArraySetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ movsd(FieldAddress(EAX, EBX, TIMES_4, Float64Array::data_offset()), XMM7);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Float64Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Float64Array, TIMES_8);
  return false;
}


bool Intrinsifier::ExternalUint8Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // EBX: index as Smi.
  // EAX: array.
  __ SmiUntag(EBX);
  __ movl(EAX, FieldAddress(EAX, ExternalUint8Array::external_data_offset()));
  __ movl(EAX, Address(EAX, ExternalByteArrayData<uint8_t>::data_offset()));
  __ movzxb(EAX, Address(EAX, EBX, TIMES_1, 0));
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Tests if two top most arguments are smis, jumps to label not_smi if not.
// Topmost argument is in EAX.
static void TestBothArgumentsSmis(Assembler* assembler, Label* not_smi) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ movl(EBX, Address(ESP, + 2 * kWordSize));
  __ orl(EBX, EAX);
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, not_smi, Assembler::kNearJump);
}


bool Intrinsifier::Integer_addFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  __ addl(EAX, Address(ESP, + 2 * kWordSize));
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_add(Assembler* assembler) {
  return Integer_addFromInteger(assembler);
}


bool Intrinsifier::Integer_subFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  __ subl(EAX, Address(ESP, + 2 * kWordSize));
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_sub(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  __ movl(EBX, EAX);
  __ movl(EAX, Address(ESP, + 2 * kWordSize));
  __ subl(EAX, EBX);
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}



bool Intrinsifier::Integer_mulFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  ASSERT(kSmiTag == 0);  // Adjust code below if not the case.
  __ SmiUntag(EAX);
  __ imull(EAX, Address(ESP, + 2 * kWordSize));
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_mul(Assembler* assembler) {
  return Integer_mulFromInteger(assembler);
}


bool Intrinsifier::Integer_modulo(Assembler* assembler) {
  Label fall_through, return_zero, try_modulo;
  TestBothArgumentsSmis(assembler, &fall_through);
  // EAX: right argument (divisor)
  // Check if modulo by zero -> exception thrown in main function.
  __ cmpl(EAX, Immediate(0));
  __ j(EQUAL, &fall_through,  Assembler::kNearJump);
  __ movl(EBX, Address(ESP, + 2 * kWordSize));  // Left argument (dividend).
  __ cmpl(EBX, Immediate(0));
  __ j(LESS, &fall_through, Assembler::kNearJump);
  __ cmpl(EBX, EAX);
  __ j(EQUAL, &return_zero, Assembler::kNearJump);
  __ j(GREATER, &try_modulo, Assembler::kNearJump);
  __ movl(EAX, EBX);  // Return dividend as it is smaller than divisor.
  __ ret();
  __ Bind(&return_zero);
  __ xorl(EAX, EAX);  // Return zero.
  __ ret();
  __ Bind(&try_modulo);
  // EAX: right (non-null divisor).
  __ movl(EBX, EAX);
  __ SmiUntag(EBX);
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Left argument (dividend).
  __ SmiUntag(EAX);
  __ cdq();
  __ idivl(EBX);
  __ movl(EAX, EDX);
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_truncDivide(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  // EAX: right argument (divisor)
  __ cmpl(EAX, Immediate(0));
  __ j(EQUAL, &fall_through, Assembler::kNearJump);
  __ movl(EBX, EAX);
  __ SmiUntag(EBX);
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Left argument (dividend).
  __ SmiUntag(EAX);
  __ pushl(EDX);  // Preserve EDX in case of 'fall_through'.
  __ cdq();
  __ idivl(EBX);
  __ popl(EDX);
  // Check the corner case of dividing the 'MIN_SMI' with -1, in which case we
  // cannot tag the result.
  __ cmpl(EAX, Immediate(0x40000000));
  __ j(EQUAL, &fall_through);
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_negate(Assembler* assembler) {
  Label fall_through;
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi value.
  __ negl(EAX);
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_bitAndFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  __ movl(EBX, Address(ESP, + 2 * kWordSize));
  __ andl(EAX, EBX);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_bitAnd(Assembler* assembler) {
  return Integer_bitAndFromInteger(assembler);
}


bool Intrinsifier::Integer_bitOrFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  __ movl(EBX, Address(ESP, + 2 * kWordSize));
  __ orl(EAX, EBX);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_bitOr(Assembler* assembler) {
  return Integer_bitOrFromInteger(assembler);
}


bool Intrinsifier::Integer_bitXorFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  __ movl(EBX, Address(ESP, + 2 * kWordSize));
  __ xorl(EAX, EBX);
  // Result is in EAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_bitXor(Assembler* assembler) {
  return Integer_bitXorFromInteger(assembler);
}


bool Intrinsifier::Integer_shl(Assembler* assembler) {
  ASSERT(kSmiTagShift == 1);
  ASSERT(kSmiTag == 0);
  Label fall_through, overflow;
  TestBothArgumentsSmis(assembler, &fall_through);
  // Shift value is in EAX. Compare with tagged Smi.
  __ cmpl(EAX, Immediate(Smi::RawValue(Smi::kBits)));
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);

  __ SmiUntag(EAX);
  __ movl(ECX, EAX);  // Shift amount must be in ECX.
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Value.

  // Overflow test - all the shifted-out bits must be same as the sign bit.
  __ movl(EBX, EAX);
  __ shll(EAX, ECX);
  __ sarl(EAX, ECX);
  __ cmpl(EAX, EBX);
  __ j(NOT_EQUAL, &overflow, Assembler::kNearJump);

  __ shll(EAX, ECX);  // Shift for result now we know there is no overflow.

  // EAX is a correctly tagged Smi.
  __ ret();

  __ Bind(&overflow);
  // Arguments are Smi but the shift produced an overflow to Mint.
  __ cmpl(EBX, Immediate(0));
  // TODO(srdjan): Implement negative values, for now fall through.
  __ j(LESS, &fall_through, Assembler::kNearJump);
  __ SmiUntag(EBX);
  __ movl(EAX, EBX);
  __ shll(EBX, ECX);
  __ xorl(EDI, EDI);
  __ shld(EDI, EAX);
  // Result in EDI (high) and EBX (low).
  const Class& mint_class = Class::Handle(
      Isolate::Current()->object_store()->mint_class());
  AssemblerMacros::TryAllocate(assembler,
                               mint_class,
                               &fall_through,
                               Assembler::kNearJump,
                               EAX);  // Result register.
  // EBX and EDI are not objects but integer values.
  __ movl(FieldAddress(EAX, Mint::value_offset()), EBX);
  __ movl(FieldAddress(EAX, Mint::value_offset() + kWordSize), EDI);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


static void Push64SmiOrMint(Assembler* assembler,
                            Register reg,
                            Register tmp,
                            Label* not_smi_or_mint) {
  Label not_smi, done;
  __ testl(reg, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &not_smi, Assembler::kNearJump);
  __ SmiUntag(reg);
  // Sign extend to 64 bit
  __ movl(tmp, reg);
  __ sarl(tmp, Immediate(31));
  __ pushl(tmp);
  __ pushl(reg);
  __ jmp(&done);
  __ Bind(&not_smi);
  __ CompareClassId(reg, kMintCid, tmp);
  __ j(NOT_EQUAL, not_smi_or_mint);
  // Mint.
  __ pushl(FieldAddress(reg, Mint::value_offset() + kWordSize));
  __ pushl(FieldAddress(reg, Mint::value_offset()));
  __ Bind(&done);
}


static bool CompareIntegers(Assembler* assembler, Condition true_condition) {
  Label try_mint_smi, is_true, is_false, drop_two_fall_through, fall_through;
  TestBothArgumentsSmis(assembler, &try_mint_smi);
  // EAX contains the right argument.
  __ cmpl(Address(ESP, + 2 * kWordSize), EAX);
  __ j(true_condition, &is_true, Assembler::kNearJump);
  __ Bind(&is_false);
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(EAX, Bool::True());
  __ ret();

  // 64-bit comparison
  Condition hi_true_cond, hi_false_cond, lo_false_cond;
  switch (true_condition) {
    case LESS:
    case LESS_EQUAL:
      hi_true_cond = LESS;
      hi_false_cond = GREATER;
      lo_false_cond = (true_condition == LESS) ? ABOVE_EQUAL : ABOVE;
      break;
    case GREATER:
    case GREATER_EQUAL:
      hi_true_cond = GREATER;
      hi_false_cond = LESS;
      lo_false_cond = (true_condition == GREATER) ? BELOW_EQUAL : BELOW;
      break;
    default:
      UNREACHABLE();
      hi_true_cond = hi_false_cond = lo_false_cond = OVERFLOW;
  }
  __ Bind(&try_mint_smi);
  // Note that EDX and ECX must be preserved in case we fall through to main
  // method.
  // EAX contains the right argument.
  __ movl(EBX, Address(ESP, + 2 * kWordSize));  // Left argument.
  // Push left as 64 bit integer.
  Push64SmiOrMint(assembler, EBX, EDI, &fall_through);
  // Push right as 64 bit integer.
  Push64SmiOrMint(assembler, EAX, EDI, &drop_two_fall_through);
  __ popl(EBX);  // Right.LO.
  __ popl(ECX);  // Right.HI.
  __ popl(EAX);  // Left.LO.
  __ popl(EDX);  // Left.HI.
  __ cmpl(EDX, ECX);  // cmpl left.HI, right.HI.
  __ j(hi_false_cond, &is_false, Assembler::kNearJump);
  __ j(hi_true_cond, &is_true, Assembler::kNearJump);
  __ cmpl(EAX, EBX);  // cmpl left.LO, right.LO.
  __ j(lo_false_cond, &is_false, Assembler::kNearJump);
  // Else is true.
  __ jmp(&is_true);

  __ Bind(&drop_two_fall_through);
  __ Drop(2);
  __ Bind(&fall_through);
  return false;
}



bool Intrinsifier::Integer_greaterThanFromInt(Assembler* assembler) {
  return CompareIntegers(assembler, LESS);
}


bool Intrinsifier::Integer_lessThan(Assembler* assembler) {
  return Integer_greaterThanFromInt(assembler);
}


bool Intrinsifier::Integer_greaterThan(Assembler* assembler) {
  return CompareIntegers(assembler, GREATER);
}


bool Intrinsifier::Integer_lessEqualThan(Assembler* assembler) {
  return CompareIntegers(assembler, LESS_EQUAL);
}


bool Intrinsifier::Integer_greaterEqualThan(Assembler* assembler) {
  return CompareIntegers(assembler, GREATER_EQUAL);
}


// This is called for Smi, Mint and Bigint receivers. The right argument
// can be Smi, Mint, Bigint or double.
bool Intrinsifier::Integer_equalToInteger(Assembler* assembler) {
  Label fall_through, true_label, check_for_mint;
  // For integer receiver '===' check first.
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ cmpl(EAX, Address(ESP, + 2 * kWordSize));
  __ j(EQUAL, &true_label, Assembler::kNearJump);
  __ movl(EBX, Address(ESP, + 2 * kWordSize));
  __ orl(EAX, EBX);
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &check_for_mint, Assembler::kNearJump);
  // Both arguments are smi, '===' is good enough.
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&true_label);
  __ LoadObject(EAX, Bool::True());
  __ ret();

  // At least one of the arguments was not Smi.
  Label receiver_not_smi;
  __ Bind(&check_for_mint);
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Receiver.
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &receiver_not_smi);

  // Left (receiver) is Smi, return false if right is not Double.
  // Note that an instance of Mint or Bigint never contains a value that can be
  // represented by Smi.
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // Right argument.
  __ CompareClassId(EAX, kDoubleCid, EDI);
  __ j(EQUAL, &fall_through);
  __ LoadObject(EAX, Bool::False());  // Smi == Mint -> false.
  __ ret();

  __ Bind(&receiver_not_smi);
  // EAX:: receiver.
  __ CompareClassId(EAX, kMintCid, EDI);
  __ j(NOT_EQUAL, &fall_through);
  // Receiver is Mint, return false if right is Smi.
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // Right argument.
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through);
  __ LoadObject(EAX, Bool::False());
  __ ret();
  // TODO(srdjan): Implement Mint == Mint comparison.

  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_equal(Assembler* assembler) {
  return Integer_equalToInteger(assembler);
}


bool Intrinsifier::Integer_sar(Assembler* assembler) {
  Label fall_through, shift_count_ok;
  TestBothArgumentsSmis(assembler, &fall_through);
  // Can destroy ECX since we are not falling through.
  const Immediate& count_limit = Immediate(0x1F);
  // Check that the count is not larger than what the hardware can handle.
  // For shifting right a Smi the result is the same for all numbers
  // >= count_limit.
  __ SmiUntag(EAX);
  // Negative counts throw exception.
  __ cmpl(EAX, Immediate(0));
  __ j(LESS, &fall_through, Assembler::kNearJump);
  __ cmpl(EAX, count_limit);
  __ j(LESS_EQUAL, &shift_count_ok, Assembler::kNearJump);
  __ movl(EAX, count_limit);
  __ Bind(&shift_count_ok);
  __ movl(ECX, EAX);  // Shift amount must be in ECX.
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Value.
  __ SmiUntag(EAX);  // Value.
  __ sarl(EAX, ECX);
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Argument is Smi (receiver).
bool Intrinsifier::Smi_bitNegate(Assembler* assembler) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // Index.
  __ notl(EAX);
  __ andl(EAX, Immediate(~kSmiTagMask));  // Remove inverted smi-tag.
  __ ret();
  return true;
}


// Check if the last argument is a double, jump to label 'is_smi' if smi
// (easy to convert to double), otherwise jump to label 'not_double_smi',
// Returns the last argument in EAX.
static void TestLastArgumentIsDouble(Assembler* assembler,
                                     Label* is_smi,
                                     Label* not_double_smi) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(ZERO, is_smi, Assembler::kNearJump);  // Jump if Smi.
  __ CompareClassId(EAX, kDoubleCid, EBX);
  __ j(NOT_EQUAL, not_double_smi, Assembler::kNearJump);
  // Fall through if double.
}


// Both arguments on stack, arg0 (left) is a double, arg1 (right) is of unknown
// type. Return true or false object in the register EAX. Any NaN argument
// returns false. Any non-double arg1 causes control flow to fall through to the
// slow case (compiled method body).
static bool CompareDoubles(Assembler* assembler, Condition true_condition) {
  Label fall_through, is_false, is_true, is_smi, double_op;
  TestLastArgumentIsDouble(assembler, &is_smi, &fall_through);
  // Both arguments are double, right operand is in EAX.
  __ movsd(XMM1, FieldAddress(EAX, Double::value_offset()));
  __ Bind(&double_op);
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Left argument.
  __ movsd(XMM0, FieldAddress(EAX, Double::value_offset()));
  __ comisd(XMM0, XMM1);
  __ j(PARITY_EVEN, &is_false, Assembler::kNearJump);  // NaN -> false;
  __ j(true_condition, &is_true, Assembler::kNearJump);
  // Fall through false.
  __ Bind(&is_false);
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(EAX, Bool::True());
  __ ret();
  __ Bind(&is_smi);
  __ SmiUntag(EAX);
  __ cvtsi2sd(XMM1, EAX);
  __ jmp(&double_op);
  __ Bind(&fall_through);
  return false;
}


// arg0 is Double, arg1 is unknown.
bool Intrinsifier::Double_greaterThan(Assembler* assembler) {
  return CompareDoubles(assembler, ABOVE);
}


// arg0 is Double, arg1 is unknown.
bool Intrinsifier::Double_greaterEqualThan(Assembler* assembler) {
  return CompareDoubles(assembler, ABOVE_EQUAL);
}


// arg0 is Double, arg1 is unknown.
bool Intrinsifier::Double_lessThan(Assembler* assembler) {
  return CompareDoubles(assembler, BELOW);
}


// arg0 is Double, arg1 is unknown.
bool Intrinsifier::Double_equal(Assembler* assembler) {
  return CompareDoubles(assembler, EQUAL);
}


// arg0 is Double, arg1 is unknown.
bool Intrinsifier::Double_lessEqualThan(Assembler* assembler) {
  return CompareDoubles(assembler, BELOW_EQUAL);
}


// Expects left argument to be double (receiver). Right argument is unknown.
// Both arguments are on stack.
static bool DoubleArithmeticOperations(Assembler* assembler, Token::Kind kind) {
  Label fall_through;
  TestLastArgumentIsDouble(assembler, &fall_through, &fall_through);
  // Both arguments are double, right operand is in EAX.
  __ movsd(XMM1, FieldAddress(EAX, Double::value_offset()));
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // Left argument.
  __ movsd(XMM0, FieldAddress(EAX, Double::value_offset()));
  switch (kind) {
    case Token::kADD: __ addsd(XMM0, XMM1); break;
    case Token::kSUB: __ subsd(XMM0, XMM1); break;
    case Token::kMUL: __ mulsd(XMM0, XMM1); break;
    case Token::kDIV: __ divsd(XMM0, XMM1); break;
    default: UNREACHABLE();
  }
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump,
                               EAX);  // Result register.
  __ movsd(FieldAddress(EAX, Double::value_offset()), XMM0);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Double_add(Assembler* assembler) {
  return DoubleArithmeticOperations(assembler, Token::kADD);
}


bool Intrinsifier::Double_mul(Assembler* assembler) {
  return DoubleArithmeticOperations(assembler, Token::kMUL);
}


bool Intrinsifier::Double_sub(Assembler* assembler) {
  return DoubleArithmeticOperations(assembler, Token::kSUB);
}


bool Intrinsifier::Double_div(Assembler* assembler) {
  return DoubleArithmeticOperations(assembler, Token::kDIV);
}


// Left is double right is integer (Bigint, Mint or Smi)
bool Intrinsifier::Double_mulFromInteger(Assembler* assembler) {
  Label fall_through;
  // Only Smi-s allowed.
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  // Is Smi.
  __ SmiUntag(EAX);
  __ cvtsi2sd(XMM1, EAX);
  __ movl(EAX, Address(ESP, + 2 * kWordSize));
  __ movsd(XMM0, FieldAddress(EAX, Double::value_offset()));
  __ mulsd(XMM0, XMM1);
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump,
                               EAX);  // Result register.
  __ movsd(FieldAddress(EAX, Double::value_offset()), XMM0);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Double_fromInteger(Assembler* assembler) {
  Label fall_through;
  __ movl(EAX, Address(ESP, +1 * kWordSize));
  __ testl(EAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  // Is Smi.
  __ SmiUntag(EAX);
  __ cvtsi2sd(XMM0, EAX);
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump,
                               EAX);  // Result register.
  __ movsd(FieldAddress(EAX, Double::value_offset()), XMM0);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Double_getIsNaN(Assembler* assembler) {
  Label is_true;
  __ movl(EAX, Address(ESP, +1 * kWordSize));
  __ movsd(XMM0, FieldAddress(EAX, Double::value_offset()));
  __ comisd(XMM0, XMM0);
  __ j(PARITY_EVEN, &is_true, Assembler::kNearJump);  // NaN -> true;
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(EAX, Bool::True());
  __ ret();
  return true;  // Method is complete, no slow case.
}


bool Intrinsifier::Double_getIsNegative(Assembler* assembler) {
  Label is_false, is_true, is_zero;
  __ movl(EAX, Address(ESP, +1 * kWordSize));
  __ movsd(XMM0, FieldAddress(EAX, Double::value_offset()));
  __ xorpd(XMM1, XMM1);  // 0.0 -> XMM1.
  __ comisd(XMM0, XMM1);
  __ j(PARITY_EVEN, &is_false, Assembler::kNearJump);  // NaN -> false.
  __ j(EQUAL, &is_zero, Assembler::kNearJump);  // Check for negative zero.
  __ j(ABOVE_EQUAL, &is_false, Assembler::kNearJump);  // >= 0 -> false.
  __ Bind(&is_true);
  __ LoadObject(EAX, Bool::True());
  __ ret();
  __ Bind(&is_false);
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&is_zero);
  // Check for negative zero (get the sign bit).
  __ movmskpd(EAX, XMM0);
  __ testl(EAX, Immediate(1));
  __ j(NOT_ZERO, &is_true, Assembler::kNearJump);
  __ jmp(&is_false, Assembler::kNearJump);
  return true;  // Method is complete, no slow case.
}


bool Intrinsifier::Double_toInt(Assembler* assembler) {
  __ movl(EAX, Address(ESP, +1 * kWordSize));
  __ movsd(XMM0, FieldAddress(EAX, Double::value_offset()));
  __ cvttsd2si(EAX, XMM0);
  // Overflow is signalled with minint.
  Label fall_through;
  // Check for overflow and that it fits into Smi.
  __ cmpl(EAX, Immediate(0xC0000000));
  __ j(NEGATIVE, &fall_through, Assembler::kNearJump);
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Argument type is not known
bool Intrinsifier::Math_sqrt(Assembler* assembler) {
  Label fall_through, is_smi, double_op;
  TestLastArgumentIsDouble(assembler, &is_smi, &fall_through);
  // Argument is double and is in EAX.
  __ movsd(XMM1, FieldAddress(EAX, Double::value_offset()));
  __ Bind(&double_op);
  __ sqrtsd(XMM0, XMM1);
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump,
                               EAX);  // Result register.
  __ movsd(FieldAddress(EAX, Double::value_offset()), XMM0);
  __ ret();
  __ Bind(&is_smi);
  __ SmiUntag(EAX);
  __ cvtsi2sd(XMM1, EAX);
  __ jmp(&double_op);
  __ Bind(&fall_through);
  return false;
}


enum TrigonometricFunctions {
  kSine,
  kCosine,
};


static void EmitTrigonometric(Assembler* assembler,
                              TrigonometricFunctions kind) {
  Label fall_through, is_smi, double_op;
  TestLastArgumentIsDouble(assembler, &is_smi, &fall_through);
  // Argument is double and is in EAX.
  __ fldl(FieldAddress(EAX, Double::value_offset()));
  __ Bind(&double_op);
  switch (kind) {
    case kSine:   __ fsin(); break;
    case kCosine: __ fcos(); break;
    default:
      UNREACHABLE();
  }
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  Label alloc_failed;
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &alloc_failed,
                               Assembler::kNearJump,
                               EAX);  // Result register.
  __ fstpl(FieldAddress(EAX, Double::value_offset()));
  __ ret();

  __ Bind(&is_smi);  // smi -> double.
  __ SmiUntag(EAX);
  __ pushl(EAX);
  __ filds(Address(ESP, 0));
  __ popl(EAX);
  __ jmp(&double_op);

  __ Bind(&alloc_failed);
  __ ffree(0);
  __ fincstp();

  __ Bind(&fall_through);
}


bool Intrinsifier::Math_sin(Assembler* assembler) {
  EmitTrigonometric(assembler, kSine);
  return false;  // Compile method for slow case.
}


bool Intrinsifier::Math_cos(Assembler* assembler) {
  EmitTrigonometric(assembler, kCosine);
  return false;  // Compile method for slow case.
}


// Identity comparison.
bool Intrinsifier::Object_equal(Assembler* assembler) {
  Label is_true;
  __ movl(EAX, Address(ESP, + 1 * kWordSize));
  __ cmpl(EAX, Address(ESP, + 2 * kWordSize));
  __ j(EQUAL, &is_true, Assembler::kNearJump);
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(EAX, Bool::True());
  __ ret();
  return true;
}


bool Intrinsifier::String_getHashCode(Assembler* assembler) {
  Label fall_through;
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // String object.
  __ movl(EAX, FieldAddress(EAX, String::hash_offset()));
  __ cmpl(EAX, Immediate(0));
  __ j(EQUAL, &fall_through, Assembler::kNearJump);
  __ ret();
  __ Bind(&fall_through);
  // Hash not yet computed.
  return false;
}


bool Intrinsifier::String_getLength(Assembler* assembler) {
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // String object.
  __ movl(EAX, FieldAddress(EAX, String::length_offset()));
  __ ret();
  return true;
}


// TODO(srdjan): Implement for two and four byte strings as well.
bool Intrinsifier::String_charCodeAt(Assembler* assembler) {
  Label fall_through;
  __ movl(EBX, Address(ESP, + 1 * kWordSize));  // Index.
  __ movl(EAX, Address(ESP, + 2 * kWordSize));  // String.
  __ testl(EBX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpl(EBX, FieldAddress(EAX, String::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  __ CompareClassId(EAX, kOneByteStringCid, EDI);
  __ j(NOT_EQUAL, &fall_through);
  __ SmiUntag(EBX);
  __ movzxb(EAX, FieldAddress(EAX, EBX, TIMES_1, OneByteString::data_offset()));
  __ SmiTag(EAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::String_getIsEmpty(Assembler* assembler) {
  Label is_true;
  // Get length.
  __ movl(EAX, Address(ESP, + 1 * kWordSize));  // String object.
  __ movl(EAX, FieldAddress(EAX, String::length_offset()));
  __ cmpl(EAX, Immediate(Smi::RawValue(0)));
  __ j(EQUAL, &is_true, Assembler::kNearJump);
  __ LoadObject(EAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(EAX, Bool::True());
  __ ret();
  return true;
}

#undef __
}  // namespace dart

#endif  // defined TARGET_ARCH_IA32
