// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"  // Needed here to get TARGET_ARCH_X64.
#if defined(TARGET_ARCH_X64)

#include "vm/intrinsifier.h"

#include "vm/assembler.h"
#include "vm/assembler_macros.h"
#include "vm/instructions.h"
#include "vm/object_store.h"
#include "vm/symbols.h"

namespace dart {

DECLARE_FLAG(bool, enable_type_checks);

// When entering intrinsics code:
// RBX: IC Data
// R10: Arguments descriptor
// TOS: Return address
// The RBX, R10 registers can be destroyed only if there is no slow-path (i.e.,
// the methods returns true).

#define __ assembler->


bool Intrinsifier::ObjectArray_Allocate(Assembler* assembler) {
  // This snippet of inlined code uses the following registers:
  // RAX, RCX, RDI, R13
  // and the newly allocated object is returned in RAX.
  const intptr_t kTypeArgumentsOffset = 2 * kWordSize;
  const intptr_t kArrayLengthOffset = 1 * kWordSize;
  Label fall_through;

  // Compute the size to be allocated, it is based on the array length
  // and is computed as:
  // RoundedAllocationSize((array_length * kwordSize) + sizeof(RawArray)).
  __ movq(RDI, Address(RSP, kArrayLengthOffset));  // Array Length.
  // Check that length is a positive Smi.
  __ testq(RDI, Immediate(kSmiTagSize));
  __ j(NOT_ZERO, &fall_through);
  __ cmpq(RDI, Immediate(0));
  __ j(LESS, &fall_through);
  // Check for maximum allowed length.
  const Immediate& max_len =
      Immediate(reinterpret_cast<int64_t>(Smi::New(Array::kMaxElements)));
  __ cmpq(RDI, max_len);
  __ j(GREATER, &fall_through);
  const intptr_t fixed_size = sizeof(RawArray) + kObjectAlignment - 1;
  __ leaq(RDI, Address(RDI, TIMES_4, fixed_size));  // RDI is a Smi.
  ASSERT(kSmiTagShift == 1);
  __ andq(RDI, Immediate(-kObjectAlignment));

  Isolate* isolate = Isolate::Current();
  Heap* heap = isolate->heap();

  __ movq(RAX, Immediate(heap->TopAddress()));
  __ movq(RAX, Address(RAX, 0));

  // RDI: allocation size.
  __ movq(RCX, RAX);
  __ addq(RCX, RDI);
  __ j(CARRY, &fall_through);

  // Check if the allocation fits into the remaining space.
  // RAX: potential new object start.
  // RCX: potential next object start.
  // RDI: allocation size.
  __ movq(R13, Immediate(heap->EndAddress()));
  __ cmpq(RCX, Address(R13, 0));
  __ j(ABOVE_EQUAL, &fall_through);

  // Successfully allocated the object(s), now update top to point to
  // next object start and initialize the object.
  __ movq(R13, Immediate(heap->TopAddress()));
  __ movq(Address(R13, 0), RCX);
  __ addq(RAX, Immediate(kHeapObjectTag));

  // Initialize the tags.
  // RAX: new object start as a tagged pointer.
  // RDI: allocation size.
  {
    Label size_tag_overflow, done;
    __ cmpq(RDI, Immediate(RawObject::SizeTag::kMaxSizeTag));
    __ j(ABOVE, &size_tag_overflow, Assembler::kNearJump);
    __ shlq(RDI, Immediate(RawObject::kSizeTagBit - kObjectAlignmentLog2));
    __ jmp(&done, Assembler::kNearJump);

    __ Bind(&size_tag_overflow);
    __ movq(RDI, Immediate(0));
    __ Bind(&done);

    // Get the class index and insert it into the tags.
    const Class& cls = Class::Handle(isolate->object_store()->array_class());
    __ orq(RDI, Immediate(RawObject::ClassIdTag::encode(cls.id())));
    __ movq(FieldAddress(RAX, Array::tags_offset()), RDI);  // Tags.
  }

  // RAX: new object start as a tagged pointer.
  // Store the type argument field.
  __ movq(RDI, Address(RSP, kTypeArgumentsOffset));  // type argument.
  __ StoreIntoObjectNoBarrier(RAX,
                              FieldAddress(RAX, Array::type_arguments_offset()),
                              RDI);

  // Set the length field.
  __ movq(RDI, Address(RSP, kArrayLengthOffset));  // Array Length.
  __ StoreIntoObjectNoBarrier(RAX,
                              FieldAddress(RAX, Array::length_offset()),
                              RDI);

  // Initialize all array elements to raw_null.
  // RAX: new object start as a tagged pointer.
  // RCX: new object end address.
  // RDI: iterator which initially points to the start of the variable
  // data area to be initialized.
  const Immediate& raw_null =
      Immediate(reinterpret_cast<intptr_t>(Object::null()));
  __ leaq(RDI, FieldAddress(RAX, sizeof(RawArray)));
  Label done;
  Label init_loop;
  __ Bind(&init_loop);
  __ cmpq(RDI, RCX);
  __ j(ABOVE_EQUAL, &done, Assembler::kNearJump);
  __ movq(Address(RDI, 0), raw_null);
  __ addq(RDI, Immediate(kWordSize));
  __ jmp(&init_loop, Assembler::kNearJump);
  __ Bind(&done);
  __ ret();  // returns the newly allocated object in RAX.

  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Array_getLength(Assembler* assembler) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ movq(RAX, FieldAddress(RAX, Array::length_offset()));
  __ ret();
  return true;
}


bool Intrinsifier::ImmutableArray_getLength(Assembler* assembler) {
  return Array_getLength(assembler);
}


bool Intrinsifier::Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  __ movq(RCX, Address(RSP, + 1 * kWordSize));  // Index.
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Array.
  __ testq(RCX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpq(RCX, FieldAddress(RAX, Array::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  // Note that RBX is Smi, i.e, times 2.
  ASSERT(kSmiTagShift == 1);
  __ movq(RAX, FieldAddress(RAX, RCX, TIMES_4, Array::data_offset()));
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::ImmutableArray_getIndexed(Assembler* assembler) {
  return Array_getIndexed(assembler);
}


bool Intrinsifier::Array_setIndexed(Assembler* assembler) {
  if (FLAG_enable_type_checks) {
    return false;
  }
  __ movq(RDX, Address(RSP, + 1 * kWordSize));  // Value.
  __ movq(RCX, Address(RSP, + 2 * kWordSize));  // Index.
  __ movq(RAX, Address(RSP, + 3 * kWordSize));  // Array.
  Label fall_through;
  __ testq(RCX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  // Range check.
  __ cmpq(RCX, FieldAddress(RAX, Array::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  // Note that RBX is Smi, i.e, times 2.
  ASSERT(kSmiTagShift == 1);
  // Destroy RCX as we will not continue in the function.
  __ StoreIntoObject(RAX,
                     FieldAddress(RAX, RCX, TIMES_4, Array::data_offset()),
                     RDX);
  // Caller is responsible of preserving the value if necessary.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Allocate a GrowableObjectArray using the backing array specified.
// On stack: type argument (+2), data (+1), return-address (+0).
bool Intrinsifier::GArray_Allocate(Assembler* assembler) {
  // This snippet of inlined code uses the following registers:
  // RAX, RCX, R13
  // and the newly allocated object is returned in RAX.
  const intptr_t kTypeArgumentsOffset = 2 * kWordSize;
  const intptr_t kArrayOffset = 1 * kWordSize;
  Label fall_through;

  // Compute the size to be allocated, it is based on the array length
  // and is computed as:
  // RoundedAllocationSize(sizeof(RawGrowableObjectArray)) +
  intptr_t fixed_size = GrowableObjectArray::InstanceSize();

  Isolate* isolate = Isolate::Current();
  Heap* heap = isolate->heap();

  __ movq(RAX, Immediate(heap->TopAddress()));
  __ movq(RAX, Address(RAX, 0));
  __ leaq(RCX, Address(RAX, fixed_size));

  // Check if the allocation fits into the remaining space.
  // RAX: potential new backing array object start.
  // RCX: potential next object start.
  __ movq(R13, Immediate(heap->EndAddress()));
  __ cmpq(RCX, Address(R13, 0));
  __ j(ABOVE_EQUAL, &fall_through);

  // Successfully allocated the object(s), now update top to point to
  // next object start and initialize the object.
  __ movq(R13, Immediate(heap->TopAddress()));
  __ movq(Address(R13, 0), RCX);
  __ addq(RAX, Immediate(kHeapObjectTag));

  // Initialize the tags.
  // EAX: new growable array object start as a tagged pointer.
  const Class& cls = Class::Handle(
      isolate->object_store()->growable_object_array_class());
  uword tags = 0;
  tags = RawObject::SizeTag::update(fixed_size, tags);
  tags = RawObject::ClassIdTag::update(cls.id(), tags);
  __ movq(FieldAddress(RAX, GrowableObjectArray::tags_offset()),
          Immediate(tags));

  // Store backing array object in growable array object.
  __ movq(RCX, Address(RSP, kArrayOffset));  // data argument.
  __ StoreIntoObject(RAX,
                     FieldAddress(RAX, GrowableObjectArray::data_offset()),
                     RCX);

  // RAX: new growable array object start as a tagged pointer.
  // Store the type argument field in the growable array object.
  __ movq(RCX, Address(RSP, kTypeArgumentsOffset));  // type argument.
  __ StoreIntoObjectNoBarrier(
      RAX,
      FieldAddress(RAX, GrowableObjectArray::type_arguments_offset()),
      RCX);

  // Set the length field in the growable array object to 0.
  __ movq(FieldAddress(RAX, GrowableObjectArray::length_offset()),
          Immediate(0));
  __ ret();  // returns the newly allocated object in RAX.

  __ Bind(&fall_through);
  return false;
}


// Get length of growable object array.
// On stack: growable array (+1), return-address (+0).
bool Intrinsifier::GrowableArray_getLength(Assembler* assembler) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ movq(RAX, FieldAddress(RAX, GrowableObjectArray::length_offset()));
  __ ret();
  return true;
}


bool Intrinsifier::GrowableArray_getCapacity(Assembler* assembler) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ movq(RAX, FieldAddress(RAX, GrowableObjectArray::data_offset()));
  __ movq(RAX, FieldAddress(RAX, Array::length_offset()));
  __ ret();
  return true;
}


// Access growable object array at specified index.
// On stack: growable array (+2), index (+1), return-address (+0).
bool Intrinsifier::GrowableArray_getIndexed(Assembler* assembler) {
  Label fall_through;
  __ movq(RCX, Address(RSP, + 1 * kWordSize));  // Index.
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // GrowableArray.
  __ testq(RCX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check using _length field.
  __ cmpq(RCX, FieldAddress(RAX, GrowableObjectArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  __ movq(RAX, FieldAddress(RAX, GrowableObjectArray::data_offset()));  // data.

  // Note that RCX is Smi, i.e, times 4.
  ASSERT(kSmiTagShift == 1);
  __ movq(RAX, FieldAddress(RAX, RCX, TIMES_4, Array::data_offset()));
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
  __ movq(RDX, Address(RSP, + 1 * kWordSize));  // Value.
  __ movq(RCX, Address(RSP, + 2 * kWordSize));  // Index.
  __ movq(RAX, Address(RSP, + 3 * kWordSize));  // GrowableArray.
  Label fall_through;
  __ testq(RCX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check using _length field.
  __ cmpq(RCX, FieldAddress(RAX, GrowableObjectArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  __ movq(RAX, FieldAddress(RAX, GrowableObjectArray::data_offset()));  // data.
  // Note that RCX is Smi, i.e, times 4.
  ASSERT(kSmiTagShift == 1);
  __ StoreIntoObject(RAX,
                     FieldAddress(RAX, RCX, TIMES_4, Array::data_offset()),
                     RDX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Set length of growable object array.
// On stack: growable array (+2), length (+1), return-address (+0).
bool Intrinsifier::GrowableArray_setLength(Assembler* assembler) {
  Label fall_through;
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Growable array.
  __ movq(RCX, Address(RSP, + 1 * kWordSize));  // Length value.
  __ testq(RCX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi length.
  __ movq(RDX, FieldAddress(RAX, GrowableObjectArray::data_offset()));
  __ cmpq(RCX, FieldAddress(RDX, Array::length_offset()));
  __ j(ABOVE, &fall_through, Assembler::kNearJump);
  __ movq(FieldAddress(RAX, GrowableObjectArray::length_offset()), RCX);
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
  __ movq(RBX, Address(RSP, + 1 * kWordSize));  /// Data.
  __ testq(RBX, Immediate(kSmiTagMask));
  __ j(ZERO, &fall_through, Assembler::kNearJump);  // Data is Smi.
  __ CompareClassId(RBX, kArrayCid);
  __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Growable array.
  __ StoreIntoObject(RAX,
                     FieldAddress(RAX, GrowableObjectArray::data_offset()),
                     RBX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Add an element to growable array if it doesn't need to grow, otherwise
// call into regular code.
// On stack: growable array (+2), value (+1), return-address (+0).
bool Intrinsifier::GrowableArray_add(Assembler* assembler) {
  // In checked mode we need to check the incoming argument.
  if (FLAG_enable_type_checks) return false;
  Label fall_through;
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Array.
  __ movq(RCX, FieldAddress(RAX, GrowableObjectArray::length_offset()));
  // RCX: length.
  __ movq(RDX, FieldAddress(RAX, GrowableObjectArray::data_offset()));
  // RDX: data.
  // Compare length with capacity.
  __ cmpq(RCX, FieldAddress(RDX, Array::length_offset()));
  __ j(EQUAL, &fall_through, Assembler::kNearJump);  // Must grow data.
  const Immediate& value_one =
      Immediate(reinterpret_cast<int64_t>(Smi::New(1)));
  // len = len + 1;
  __ addq(FieldAddress(RAX, GrowableObjectArray::length_offset()), value_one);
  __ movq(RAX, Address(RSP, + 1 * kWordSize));  // Value
  ASSERT(kSmiTagShift == 1);
  __ StoreIntoObject(RDX,
                     FieldAddress(RDX, RCX, TIMES_4, Array::data_offset()),
                     RAX);
  const Immediate& raw_null =
      Immediate(reinterpret_cast<int64_t>(Object::null()));
  __ movq(RAX, raw_null);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::ByteArrayBase_getLength(Assembler* assembler) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ movq(RAX, FieldAddress(RAX, ByteArray::length_offset()));
  __ ret();
  // Generate enough code to satisfy patchability constraint.
  intptr_t offset = __ CodeSize();
  __ nop(JumpPattern::InstructionLength() - offset);
  return true;
}



// Tests if index is a valid length (Smi and within valid index range),
// jumps to fall_through if it is not.
// Returns index in R12, array in RAX.
// This should be used only on getIndexed intrinsics.
void TestByteArrayGetIndex(Assembler* assembler, Label* fall_through) {
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Array.
  __ movq(R12, Address(RSP, + 1 * kWordSize));  // Index.
  __ testq(R12, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpq(R12, FieldAddress(RAX, ByteArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, fall_through, Assembler::kNearJump);
}


// Tests if index is a valid length (Smi and within valid index range),
// jumps to fall_through if it is not.
// Returns index in R12, array in RAX.
// This should be used only for setIndexed intrinsics.
static void TestByteArraySetIndex(Assembler* assembler, Label* fall_through) {
  __ movq(RAX, Address(RSP, + 3 * kWordSize));  // Array.
  __ movq(R12, Address(RSP, + 2 * kWordSize));  // Index.
  __ testq(R12, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpq(R12, FieldAddress(RAX, ByteArray::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, fall_through, Assembler::kNearJump);
}


bool Intrinsifier::Int8Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ SmiUntag(R12);
  __ movsxb(RAX, FieldAddress(RAX,
                              R12,
                              TIMES_1,
                              Int8Array::data_offset()));
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Int8Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ SmiUntag(R12);
  __ movq(RDI, Address(RSP, + 1 * kWordSize));  // Value.
  __ testq(RDI, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  __ SmiUntag(RDI);
  // Check that the value is a byte. Add 128 to the value to bring it into
  // the range 0..FF.
  __ addq(RDI, Immediate(128));
  __ cmpq(RDI, Immediate(0xFF));
  __ j(ABOVE, &fall_through, Assembler::kNearJump);
  // Undo addition.
  __ subq(RDI, Immediate(128));
  __ movb(FieldAddress(RAX, R12, TIMES_1, Uint8Array::data_offset()), RDI);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


#define TYPED_ARRAY_ALLOCATION(type_name, scale_factor)                        \
  Label fall_through;                                                          \
  const intptr_t kArrayLengthStackOffset = 1 * kWordSize;                      \
  __ movq(RDI, Address(RSP, kArrayLengthStackOffset));  /* Array length. */    \
  /* Check that length is a positive Smi. */                                   \
  /* RDI: requested array length argument. */                                  \
  __ testq(RDI, Immediate(kSmiTagSize));                                       \
  __ j(NOT_ZERO, &fall_through);                                               \
  __ cmpq(RDI, Immediate(0));                                                  \
  __ j(LESS, &fall_through);                                                   \
  __ SmiUntag(RDI);                                                            \
  /* Check for maximum allowed length. */                                      \
  /* RDI: untagged array length. */                                            \
  __ cmpq(RDI, Immediate(type_name::kMaxElements));                            \
  __ j(GREATER, &fall_through);                                                \
  const intptr_t fixed_size = sizeof(Raw##type_name) + kObjectAlignment - 1;   \
  __ leaq(RDI, Address(RDI, scale_factor, fixed_size));                        \
  __ andq(RDI, Immediate(-kObjectAlignment));                                  \
  Heap* heap = Isolate::Current()->heap();                                     \
                                                                               \
  __ movq(RAX, Immediate(heap->TopAddress()));                                 \
  __ movq(RAX, Address(RAX, 0));                                               \
  __ movq(RCX, RAX);                                                           \
                                                                               \
  /* RDI: allocation size. */                                                  \
  __ addq(RCX, RDI);                                                           \
  __ j(CARRY, &fall_through);                                                  \
                                                                               \
  /* Check if the allocation fits into the remaining space. */                 \
  /* RAX: potential new object start. */                                       \
  /* RCX: potential next object start. */                                      \
  /* RDI: allocation size. */                                                  \
  /* R13: scratch register. */                                                 \
  __ movq(R13, Immediate(heap->EndAddress()));                                 \
  __ cmpq(RCX, Address(R13, 0));                                               \
  __ j(ABOVE_EQUAL, &fall_through);                                            \
                                                                               \
  /* Successfully allocated the object(s), now update top to point to */       \
  /* next object start and initialize the object. */                           \
  __ movq(R13, Immediate(heap->TopAddress()));                                 \
  __ movq(Address(R13, 0), RCX);                                               \
  __ addq(RAX, Immediate(kHeapObjectTag));                                     \
                                                                               \
  /* Initialize the tags. */                                                   \
  /* RAX: new object start as a tagged pointer. */                             \
  /* RCX: new object end address. */                                           \
  /* RDI: allocation size. */                                                  \
  /* R13: scratch register. */                                                 \
  {                                                                            \
    Label size_tag_overflow, done;                                             \
    __ cmpq(RDI, Immediate(RawObject::SizeTag::kMaxSizeTag));                  \
    __ j(ABOVE, &size_tag_overflow, Assembler::kNearJump);                     \
    __ shlq(RDI, Immediate(RawObject::kSizeTagBit - kObjectAlignmentLog2));    \
    __ jmp(&done, Assembler::kNearJump);                                       \
                                                                               \
    __ Bind(&size_tag_overflow);                                               \
    __ movq(RDI, Immediate(0));                                                \
    __ Bind(&done);                                                            \
                                                                               \
    /* Get the class index and insert it into the tags. */                     \
    __ orq(RDI, Immediate(RawObject::ClassIdTag::encode(k##type_name##Cid)));  \
    __ movq(FieldAddress(RAX, type_name::tags_offset()), RDI);  /* Tags. */    \
  }                                                                            \
  /* Set the length field. */                                                  \
  /* RAX: new object start as a tagged pointer. */                             \
  /* RCX: new object end address. */                                           \
  __ movq(RDI, Address(RSP, kArrayLengthStackOffset));  /* Array length. */    \
  __ StoreIntoObjectNoBarrier(RAX,                                             \
                              FieldAddress(RAX, type_name::length_offset()),   \
                              RDI);                                            \
  /* Initialize all array elements to 0. */                                    \
  /* RAX: new object start as a tagged pointer. */                             \
  /* RCX: new object end address. */                                           \
  /* RDI: iterator which initially points to the start of the variable */      \
  /* RBX: scratch register. */                                                 \
  /* data area to be initialized. */                                           \
  __ xorq(RBX, RBX);  /* Zero. */                                              \
  __ leaq(RDI, FieldAddress(RAX, sizeof(Raw##type_name)));                     \
  Label done, init_loop;                                                       \
  __ Bind(&init_loop);                                                         \
  __ cmpq(RDI, RCX);                                                           \
  __ j(ABOVE_EQUAL, &done, Assembler::kNearJump);                              \
  __ movq(Address(RDI, 0), RBX);                                               \
  __ addq(RDI, Immediate(kWordSize));                                          \
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
  // R12: index as Smi.
  // RAX: array.
  __ SmiUntag(R12);
  __ movzxb(RAX, FieldAddress(RAX,
                              R12,
                              TIMES_1,
                              Uint8Array::data_offset()));
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint8Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ SmiUntag(R12);
  __ movq(RDI, Address(RSP, + 1 * kWordSize));  // Value.
  __ testq(RDI, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  __ SmiUntag(RDI);
  // Check that value is a byte.
  __ cmpq(RDI, Immediate(0xFF));
  __ j(ABOVE, &fall_through, Assembler::kNearJump);
  __ movb(FieldAddress(RAX, R12, TIMES_1, Uint8Array::data_offset()), RDI);
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
  // R12: index as Smi.
  // RAX: array.
  __ SmiUntag(R12);
  __ movzxb(RAX, FieldAddress(RAX,
                              R12,
                              TIMES_1,
                              Uint8ClampedArray::data_offset()));
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint8ClampedArray_setIndexed(Assembler* assembler) {
  Label fall_through, store_value, load_0xff;
  TestByteArraySetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ SmiUntag(R12);
  __ movq(RDI, Address(RSP, + 1 * kWordSize));  // Value.
  __ testq(RDI, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);

  __ SmiUntag(RDI);
  __ cmpq(RDI, Immediate(0xFF));
  __ j(BELOW_EQUAL, &store_value, Assembler::kNearJump);
  __ j(GREATER, &load_0xff,  Assembler::kNearJump);
  __ xorq(RDI, RDI);  // Zero.
  __ jmp(&store_value, Assembler::kNearJump);
  __ Bind(&load_0xff);
  __ movq(RDI, Immediate(0xFF));

  __ Bind(&store_value);
  __ movb(
      FieldAddress(RAX, R12, TIMES_1, Uint8ClampedArray::data_offset()), RDI);
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
  // R12: index as Smi.
  // RAX: array.
  __ movsxw(RAX, FieldAddress(RAX,
                              R12,
                              TIMES_1,
                              Int16Array::data_offset()));
  __ SmiTag(RAX);
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
  // R12: index as Smi.
  // RAX: array.
  __ movzxw(RAX, FieldAddress(RAX,
                              R12,
                              TIMES_1,
                              Uint16Array::data_offset()));
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint16Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ movq(RDI, Address(RSP, + 1 * kWordSize));
  __ SmiUntag(RDI);
  // RDI: untagged value.
  __ testl(RDI, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  __ movw(FieldAddress(RAX, R12, TIMES_1, Uint16Array::data_offset()), RDI);
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
  // R12: index as Smi.
  // RAX: array.
  __ movsxl(RAX, FieldAddress(RAX,
                              R12,
                              TIMES_2,
                              Int32Array::data_offset()));
  __ SmiTag(RAX);
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
  // R12: index as Smi.
  // RAX: array.
  __ movl(RAX, FieldAddress(RAX,
                            R12,
                            TIMES_2,
                            Uint32Array::data_offset()));
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint32Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Uint32Array, TIMES_4);
  return false;
}


bool Intrinsifier::Int64Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ movq(RAX, FieldAddress(RAX,
                            R12,
                            TIMES_4,
                            Int64Array::data_offset()));
  // Copy RAX into R12.
  // We destroy R12 while testing if RAX can fit inside a Smi.
  __ movq(R12, RAX);
  // Verify that the signed value in RAX can fit inside a Smi.
  __ shlq(R12, Immediate(0x1));
  // Jump to fall_through if it can not.
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Int64Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Int64Array, TIMES_8);
  return false;
}


bool Intrinsifier::Uint64Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ movq(RAX, FieldAddress(RAX,
                            R12,
                            TIMES_4,
                            Uint64Array::data_offset()));
  // Copy RAX into R12.
  // We destroy R12 while testing if RAX can fit inside a Smi.
  __ movq(R12, RAX);
  // Verify that the unsigned value in RAX can be stored in a Smi.
  __ shrq(R12, Immediate(kSmiBits));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Won't fit Smi.
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Uint64Array_new(Assembler* assembler) {
  TYPED_ARRAY_ALLOCATION(Uint64Array, TIMES_8);
  return false;
}


bool Intrinsifier::Float32Array_getIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArrayGetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  // Load single precision float into XMM7.
  __ movss(XMM7, FieldAddress(RAX, R12, TIMES_2,
                              Float32Array::data_offset()));
  // Convert into a double precision float.
  __ cvtss2sd(XMM7, XMM7);
  // Allocate a double instance.
  const Class& double_class = Class::Handle(
                          Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump, RAX);
  // Store XMM7 into double instance.
  __ movsd(FieldAddress(RAX, Double::value_offset()), XMM7);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Float32Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ movq(RDX, Address(RSP, + 1 * kWordSize));  // Value.
  // If RDX is not an instance of double, jump to fall through.
  __ testq(RDX, Immediate(kSmiTagMask));
  __ j(ZERO, &fall_through, Assembler::kNearJump);
  __ CompareClassId(RDX, kDoubleCid);
  __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
  // Load double value into XMM7.
  __ movsd(XMM7, FieldAddress(RDX, Double::value_offset()));
  // Convert from double precision float to single precision float.
  __ cvtsd2ss(XMM7, XMM7);
  // Store into array.
  __ movss(FieldAddress(RAX, R12, TIMES_2, Float32Array::data_offset()), XMM7);
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
  // R12: index as Smi.
  // RAX: array.
  // Load double precision float into XMM7.
  __ movsd(XMM7, FieldAddress(RAX, R12, TIMES_4,
                              Float64Array::data_offset()));
  // Allocate a double instance.
  const Class& double_class = Class::Handle(
    Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump, RAX);
  // Store XMM7 into double instance.
  __ movsd(FieldAddress(RAX, Double::value_offset()), XMM7);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Float64Array_setIndexed(Assembler* assembler) {
  Label fall_through;
  TestByteArraySetIndex(assembler, &fall_through);
  // R12: index as Smi.
  // RAX: array.
  __ movq(RDX, Address(RSP, + 1 * kWordSize));  // Value.
  // If RDX is not an instance of double, jump to fall through.
  __ testq(RDX, Immediate(kSmiTagMask));
  __ j(ZERO, &fall_through, Assembler::kNearJump);
  __ CompareClassId(RDX, kDoubleCid);
  __ j(NOT_EQUAL, &fall_through, Assembler::kNearJump);
  // Load double value into XMM7.
  __ movsd(XMM7, FieldAddress(RDX, Double::value_offset()));
  // Store into array.
  __ movsd(FieldAddress(RAX, R12, TIMES_4, Float64Array::data_offset()), XMM7);
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
  // R12: index as Smi.
  // RAX: array.
  __ SmiUntag(R12);
  __ movq(RAX, FieldAddress(RAX, ExternalUint8Array::external_data_offset()));
  __ movq(RAX, Address(RAX, ExternalByteArrayData<uint8_t>::data_offset()));
  __ movzxb(RAX, Address(RAX, R12, TIMES_1, 0));
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Tests if two top most arguments are smis, jumps to label not_smi if not.
// Topmost argument is in RAX.
static void TestBothArgumentsSmis(Assembler* assembler, Label* not_smi) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ movq(RCX, Address(RSP, + 2 * kWordSize));
  __ orq(RCX, RAX);
  __ testq(RCX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, not_smi, Assembler::kNearJump);
}


bool Intrinsifier::Integer_addFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  // RAX contains right argument.
  __ addq(RAX, Address(RSP, + 2 * kWordSize));
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in RAX.
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
  // RAX contains right argument, which is the actual minuend of subtraction.
  __ subq(RAX, Address(RSP, + 2 * kWordSize));
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in RAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_sub(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  // RAX contains right argument, which is the actual subtrahend of subtraction.
  __ movq(RCX, RAX);
  __ movq(RAX, Address(RSP, + 2 * kWordSize));
  __ subq(RAX, RCX);
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in RAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}



bool Intrinsifier::Integer_mulFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  // RAX is the right argument.
  ASSERT(kSmiTag == 0);  // Adjust code below if not the case.
  __ SmiUntag(RAX);
  __ imulq(RAX, Address(RSP, + 2 * kWordSize));
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in RAX.
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
  // RAX: right argument (divisor)
  // Check if modulo by zero -> exception thrown in main function.
  __ cmpq(RAX, Immediate(0));
  __ j(EQUAL, &fall_through,  Assembler::kNearJump);
  __ movq(RCX, Address(RSP, + 2 * kWordSize));  // Left argument (dividend).
  __ cmpq(RCX, Immediate(0));
  __ j(LESS, &fall_through, Assembler::kNearJump);
  __ cmpq(RCX, RAX);
  __ j(EQUAL, &return_zero, Assembler::kNearJump);
  __ j(GREATER, &try_modulo, Assembler::kNearJump);
  __ movq(RAX, RCX);  // Return dividend as it is smaller than divisor.
  __ ret();
  __ Bind(&return_zero);
  __ xorq(RAX, RAX);  // Return zero.
  __ ret();
  __ Bind(&try_modulo);
  // RAX: right (non-null divisor).
  __ movq(RCX, RAX);
  __ SmiUntag(RCX);
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Left argument (dividend).
  __ SmiUntag(RAX);
  __ cqo();
  __ idivq(RCX);
  __ movq(RAX, RDX);
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_truncDivide(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  // RAX: right argument (divisor)
  __ cmpq(RAX, Immediate(0));
  __ j(EQUAL, &fall_through, Assembler::kNearJump);
  __ movq(RCX, RAX);
  __ SmiUntag(RCX);
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Left argument (dividend).
  __ SmiUntag(RAX);
  __ pushq(RDX);  // Preserve RDX in case of 'fall_through'.
  __ cqo();
  __ idivq(RCX);
  __ popq(RDX);
  // Check the corner case of dividing the 'MIN_SMI' with -1, in which case we
  // cannot tag the result.
  __ cmpq(RAX, Immediate(0x4000000000000000));
  __ j(EQUAL, &fall_through);
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_negate(Assembler* assembler) {
  Label fall_through;
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi value.
  __ negq(RAX);
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  // Result is in RAX.
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Integer_bitAndFromInteger(Assembler* assembler) {
  Label fall_through;
  TestBothArgumentsSmis(assembler, &fall_through);
  // RAX is the right argument.
  __ andq(RAX, Address(RSP, + 2 * kWordSize));
  // Result is in RAX.
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
  // RAX is the right argument.
  __ orq(RAX, Address(RSP, + 2 * kWordSize));
  // Result is in RAX.
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
  // RAX is the right argument.
  __ xorq(RAX, Address(RSP, + 2 * kWordSize));
  // Result is in RAX.
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
  // Shift value is in RAX. Compare with tagged Smi.
  __ cmpq(RAX, Immediate(Smi::RawValue(Smi::kBits)));
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);

  __ SmiUntag(RAX);
  __ movq(RCX, RAX);  // Shift amount must be in RCX.
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Value.

  // Overflow test - all the shifted-out bits must be same as the sign bit.
  __ movq(RDI, RAX);
  __ shlq(RAX, RCX);
  __ sarq(RAX, RCX);
  __ cmpq(RAX, RDI);
  __ j(NOT_EQUAL, &overflow, Assembler::kNearJump);

  __ shlq(RAX, RCX);  // Shift for result now we know there is no overflow.

  // RAX is a correctly tagged Smi.
  __ ret();

  __ Bind(&overflow);
  // Mint is rarely used on x64 (only for integers requiring 64 bit instead of
  // 63 bits as represented by Smi).
  __ Bind(&fall_through);
  return false;
}


static bool CompareIntegers(Assembler* assembler, Condition true_condition) {
  Label fall_through, true_label;
  TestBothArgumentsSmis(assembler, &fall_through);
  // RAX contains the right argument.
  __ cmpq(Address(RSP, + 2 * kWordSize), RAX);
  __ j(true_condition, &true_label, Assembler::kNearJump);
  __ LoadObject(RAX, Bool::False());
  __ ret();
  __ Bind(&true_label);
  __ LoadObject(RAX, Bool::True());
  __ ret();
  __ Bind(&fall_through);
  return false;
}



bool Intrinsifier::Integer_lessThan(Assembler* assembler) {
  return CompareIntegers(assembler, LESS);
}


bool Intrinsifier::Integer_greaterThanFromInt(Assembler* assembler) {
  return CompareIntegers(assembler, LESS);
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
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ movq(RCX, Address(RSP, + 2 * kWordSize));
  __ cmpq(RAX, RCX);
  __ j(EQUAL, &true_label, Assembler::kNearJump);
  __ orq(RAX, RCX);
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &check_for_mint, Assembler::kNearJump);
  // Both arguments are smi, '===' is good enough.
  __ LoadObject(RAX, Bool::False());
  __ ret();
  __ Bind(&true_label);
  __ LoadObject(RAX, Bool::True());
  __ ret();

  // At least one of the arguments was not Smi.
  Label receiver_not_smi;
  __ Bind(&check_for_mint);
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Receiver.
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &receiver_not_smi);

  // Left (receiver) is Smi, return false if right is not Double.
  // Note that an instance of Mint or Bigint never contains a value that can be
  // represented by Smi.
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ CompareClassId(RAX, kDoubleCid);
  __ j(EQUAL, &fall_through);
  __ LoadObject(RAX, Bool::False());
  __ ret();

  __ Bind(&receiver_not_smi);
  // RAX:: receiver.
  __ CompareClassId(RAX, kMintCid);
  __ j(NOT_EQUAL, &fall_through);
  // Receiver is Mint, return false if right is Smi.
  __ movq(RAX, Address(RSP, + 1 * kWordSize));  // Right argument.
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through);
  __ LoadObject(RAX, Bool::False());  // Smi == Mint -> false.
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
  const Immediate& count_limit = Immediate(0x3F);
  // Check that the count is not larger than what the hardware can handle.
  // For shifting right a Smi the result is the same for all numbers
  // >= count_limit.
  __ SmiUntag(RAX);
  // Negative counts throw exception.
  __ cmpq(RAX, Immediate(0));
  __ j(LESS, &fall_through, Assembler::kNearJump);
  __ cmpq(RAX, count_limit);
  __ j(LESS_EQUAL, &shift_count_ok, Assembler::kNearJump);
  __ movq(RAX, count_limit);
  __ Bind(&shift_count_ok);
  __ movq(RCX, RAX);  // Shift amount must be in RCX.
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Value.
  __ SmiUntag(RAX);  // Value.
  __ sarq(RAX, RCX);
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Argument is Smi (receiver).
bool Intrinsifier::Smi_bitNegate(Assembler* assembler) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));  // Index.
  __ notq(RAX);
  __ andq(RAX, Immediate(~kSmiTagMask));  // Remove inverted smi-tag.
  __ ret();
  return true;
}


// Check if the last argument is a double, jump to label 'is_smi' if smi
// (easy to convert to double), otherwise jump to label 'not_double_smi',
// Returns the last argument in RAX.
static void TestLastArgumentIsDouble(Assembler* assembler,
                                     Label* is_smi,
                                     Label* not_double_smi) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(ZERO, is_smi, Assembler::kNearJump);  // Jump if Smi.
  __ CompareClassId(RAX, kDoubleCid);
  __ j(NOT_EQUAL, not_double_smi, Assembler::kNearJump);
  // Fall through if double.
}


// Both arguments on stack, left argument is a double, right argument is of
// unknown type. Return true or false object in RAX. Any NaN argument
// returns false. Any non-double argument causes control flow to fall through
// to the slow case (compiled method body).
static bool CompareDoubles(Assembler* assembler, Condition true_condition) {
  Label fall_through, is_false, is_true, is_smi, double_op;
  TestLastArgumentIsDouble(assembler, &is_smi, &fall_through);
  // Both arguments are double, right operand is in RAX.
  __ movsd(XMM1, FieldAddress(RAX, Double::value_offset()));
  __ Bind(&double_op);
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Left argument.
  __ movsd(XMM0, FieldAddress(RAX, Double::value_offset()));
  __ comisd(XMM0, XMM1);
  __ j(PARITY_EVEN, &is_false, Assembler::kNearJump);  // NaN -> false;
  __ j(true_condition, &is_true, Assembler::kNearJump);
  // Fall through false.
  __ Bind(&is_false);
  __ LoadObject(RAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(RAX, Bool::True());
  __ ret();
  __ Bind(&is_smi);
  __ SmiUntag(RAX);
  __ cvtsi2sd(XMM1, RAX);
  __ jmp(&double_op);
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Double_greaterThan(Assembler* assembler) {
  return CompareDoubles(assembler, ABOVE);
}


bool Intrinsifier::Double_greaterEqualThan(Assembler* assembler) {
  return CompareDoubles(assembler, ABOVE_EQUAL);
}


bool Intrinsifier::Double_lessThan(Assembler* assembler) {
  return CompareDoubles(assembler, BELOW);
}


bool Intrinsifier::Double_equal(Assembler* assembler) {
  return CompareDoubles(assembler, EQUAL);
}


bool Intrinsifier::Double_lessEqualThan(Assembler* assembler) {
  return CompareDoubles(assembler, BELOW_EQUAL);
}


// Expects left argument to be double (receiver). Right argument is unknown.
// Both arguments are on stack.
static bool DoubleArithmeticOperations(Assembler* assembler, Token::Kind kind) {
  Label fall_through;
  TestLastArgumentIsDouble(assembler, &fall_through, &fall_through);
  // Both arguments are double, right operand is in RAX.
  __ movsd(XMM1, FieldAddress(RAX, Double::value_offset()));
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // Left argument.
  __ movsd(XMM0, FieldAddress(RAX, Double::value_offset()));
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
                               RAX);  // Result register.
  __ movsd(FieldAddress(RAX, Double::value_offset()), XMM0);
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


bool Intrinsifier::Double_mulFromInteger(Assembler* assembler) {
  Label fall_through;
  // Only Smi-s allowed.
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  // Is Smi.
  __ SmiUntag(RAX);
  __ cvtsi2sd(XMM1, RAX);
  __ movq(RAX, Address(RSP, + 2 * kWordSize));
  __ movsd(XMM0, FieldAddress(RAX, Double::value_offset()));
  __ mulsd(XMM0, XMM1);
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump,
                               RAX);  // Result register.
  __ movsd(FieldAddress(RAX, Double::value_offset()), XMM0);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


// Left is double right is integer (Bigint, Mint or Smi)
bool Intrinsifier::Double_fromInteger(Assembler* assembler) {
  Label fall_through;
  __ movq(RAX, Address(RSP, +1 * kWordSize));
  __ testq(RAX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);
  // Is Smi.
  __ SmiUntag(RAX);
  __ cvtsi2sd(XMM0, RAX);
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump,
                               RAX);  // Result register.
  __ movsd(FieldAddress(RAX, Double::value_offset()), XMM0);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Double_getIsNaN(Assembler* assembler) {
  Label is_true;
  __ movq(RAX, Address(RSP, +1 * kWordSize));
  __ movsd(XMM0, FieldAddress(RAX, Double::value_offset()));
  __ comisd(XMM0, XMM0);
  __ j(PARITY_EVEN, &is_true, Assembler::kNearJump);  // NaN -> true;
  __ LoadObject(RAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(RAX, Bool::True());
  __ ret();
  return true;  // Method is complete, no slow case.
}


bool Intrinsifier::Double_getIsNegative(Assembler* assembler) {
  Label is_false, is_true, is_zero;
  __ movq(RAX, Address(RSP, +1 * kWordSize));
  __ movsd(XMM0, FieldAddress(RAX, Double::value_offset()));
  __ xorpd(XMM1, XMM1);  // 0.0 -> XMM1.
  __ comisd(XMM0, XMM1);
  __ j(PARITY_EVEN, &is_false, Assembler::kNearJump);  // NaN -> false.
  __ j(EQUAL, &is_zero, Assembler::kNearJump);  // Check for negative zero.
  __ j(ABOVE_EQUAL, &is_false, Assembler::kNearJump);  // >= 0 -> false.
  __ Bind(&is_true);
  __ LoadObject(RAX, Bool::True());
  __ ret();
  __ Bind(&is_false);
  __ LoadObject(RAX, Bool::False());
  __ ret();
  __ Bind(&is_zero);
  // Check for negative zero (get the sign bit).
  __ movmskpd(RAX, XMM0);
  __ testq(RAX, Immediate(1));
  __ j(NOT_ZERO, &is_true, Assembler::kNearJump);
  __ jmp(&is_false, Assembler::kNearJump);
  return true;  // Method is complete, no slow case.
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
  __ fldl(FieldAddress(RAX, Double::value_offset()));
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
                               RAX);  // Result register.
  __ fstpl(FieldAddress(RAX, Double::value_offset()));
  __ ret();

  __ Bind(&is_smi);  // smi -> double.
  __ SmiUntag(RAX);
  __ pushq(RAX);
  __ fildl(Address(RSP, 0));
  __ popq(RAX);
  __ jmp(&double_op);

  __ Bind(&alloc_failed);
  __ ffree(0);
  __ fincstp();

  __ Bind(&fall_through);
}


bool Intrinsifier::Double_toInt(Assembler* assembler) {
  __ movq(RAX, Address(RSP, +1 * kWordSize));
  __ movsd(XMM0, FieldAddress(RAX, Double::value_offset()));
  __ cvttsd2siq(RAX, XMM0);
  // Overflow is signalled with minint.
  Label fall_through;
  // Check for overflow and that it fits into Smi.
  __ movq(RCX, RAX);
  __ shlq(RCX, Immediate(1));
  __ j(OVERFLOW, &fall_through, Assembler::kNearJump);
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::Math_sqrt(Assembler* assembler) {
  Label fall_through, is_smi, double_op;
  TestLastArgumentIsDouble(assembler, &is_smi, &fall_through);
  // Argument is double and is in RAX.
  __ movsd(XMM1, FieldAddress(RAX, Double::value_offset()));
  __ Bind(&double_op);
  __ sqrtsd(XMM0, XMM1);
  const Class& double_class = Class::Handle(
      Isolate::Current()->object_store()->double_class());
  AssemblerMacros::TryAllocate(assembler,
                               double_class,
                               &fall_through,
                               Assembler::kNearJump,
                               RAX);  // Result register.
  __ movsd(FieldAddress(RAX, Double::value_offset()), XMM0);
  __ ret();
  __ Bind(&is_smi);
  __ SmiUntag(RAX);
  __ cvtsi2sd(XMM1, RAX);
  __ jmp(&double_op);
  __ Bind(&fall_through);
  return false;
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
  __ movq(RAX, Address(RSP, + 1 * kWordSize));
  __ cmpq(RAX, Address(RSP, + 2 * kWordSize));
  __ j(EQUAL, &is_true, Assembler::kNearJump);
  __ LoadObject(RAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(RAX, Bool::True());
  __ ret();
  return true;
}


bool Intrinsifier::String_getHashCode(Assembler* assembler) {
  Label fall_through;
  __ movq(RAX, Address(RSP, + 1 * kWordSize));  // String object.
  __ movq(RAX, FieldAddress(RAX, String::hash_offset()));
  __ cmpq(RAX, Immediate(0));
  __ j(EQUAL, &fall_through, Assembler::kNearJump);
  __ ret();
  __ Bind(&fall_through);
  // Hash not yet computed.
  return false;
}


bool Intrinsifier::String_getLength(Assembler* assembler) {
  __ movq(RAX, Address(RSP, + 1 * kWordSize));  // String object.
  __ movq(RAX, FieldAddress(RAX, String::length_offset()));
  __ ret();
  return true;
}


// TODO(srdjan): Implement for two and four byte strings as well.
bool Intrinsifier::String_charCodeAt(Assembler* assembler) {
  Label fall_through;
  __ movq(RCX, Address(RSP, + 1 * kWordSize));  // Index.
  __ movq(RAX, Address(RSP, + 2 * kWordSize));  // String.
  __ testq(RCX, Immediate(kSmiTagMask));
  __ j(NOT_ZERO, &fall_through, Assembler::kNearJump);  // Non-smi index.
  // Range check.
  __ cmpq(RCX, FieldAddress(RAX, String::length_offset()));
  // Runtime throws exception.
  __ j(ABOVE_EQUAL, &fall_through, Assembler::kNearJump);
  __ CompareClassId(RAX, kOneByteStringCid);
  __ j(NOT_EQUAL, &fall_through);
  __ SmiUntag(RCX);
  __ movzxb(RAX, FieldAddress(RAX, RCX, TIMES_1, OneByteString::data_offset()));
  __ SmiTag(RAX);
  __ ret();
  __ Bind(&fall_through);
  return false;
}


bool Intrinsifier::String_getIsEmpty(Assembler* assembler) {
  Label is_true;
  // Get length.
  __ movq(RAX, Address(RSP, + 1 * kWordSize));  // String object.
  __ movq(RAX, FieldAddress(RAX, String::length_offset()));
  __ cmpq(RAX, Immediate(Smi::RawValue(0)));
  __ j(EQUAL, &is_true, Assembler::kNearJump);
  __ LoadObject(RAX, Bool::False());
  __ ret();
  __ Bind(&is_true);
  __ LoadObject(RAX, Bool::True());
  __ ret();
  return true;
}

#undef __

}  // namespace dart

#endif  // defined TARGET_ARCH_X64
