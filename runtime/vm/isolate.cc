// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/isolate.h"

#include "include/dart_api.h"
#include "platform/assert.h"
#include "lib/mirrors.h"
#include "vm/code_observers.h"
#include "vm/compiler_stats.h"
#include "vm/dart_api_state.h"
#include "vm/dart_entry.h"
#include "vm/debugger.h"
#include "vm/heap.h"
#include "vm/message_handler.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/port.h"
#include "vm/simulator.h"
#include "vm/stack_frame.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"
#include "vm/thread.h"
#include "vm/timer.h"
#include "vm/visitor.h"

namespace dart {

DEFINE_FLAG(bool, report_usage_count, false,
            "Track function usage and report.");
DEFINE_FLAG(bool, trace_isolates, false,
            "Trace isolate creation and shut down.");


class IsolateMessageHandler : public MessageHandler {
 public:
  explicit IsolateMessageHandler(Isolate* isolate);
  ~IsolateMessageHandler();

  const char* name() const;
  void MessageNotify(Message::Priority priority);
  bool HandleMessage(Message* message);

#if defined(DEBUG)
  // Check that it is safe to access this handler.
  void CheckAccess();
#endif
  bool IsCurrentIsolate() const;
  virtual Isolate* GetIsolate() const { return isolate_; }
  bool UnhandledExceptionCallbackHandler(const Object& message,
                                         const UnhandledException& error);

 private:
  bool ProcessUnhandledException(const Object& message, const Error& result);
  RawFunction* ResolveCallbackFunction();
  Isolate* isolate_;
};


IsolateMessageHandler::IsolateMessageHandler(Isolate* isolate)
    : isolate_(isolate) {
}


IsolateMessageHandler::~IsolateMessageHandler() {
}

const char* IsolateMessageHandler::name() const {
  return isolate_->name();
}


void IsolateMessageHandler::MessageNotify(Message::Priority priority) {
  if (priority >= Message::kOOBPriority) {
    // Handle out of band messages even if the isolate is busy.
    isolate_->ScheduleInterrupts(Isolate::kMessageInterrupt);
  }
  Dart_MessageNotifyCallback callback = isolate_->message_notify_callback();
  if (callback) {
    // Allow the embedder to handle message notification.
    (*callback)(Api::CastIsolate(isolate_));
  }
}


bool IsolateMessageHandler::HandleMessage(Message* message) {
  StartIsolateScope start_scope(isolate_);
  StackZone zone(isolate_);
  HandleScope handle_scope(isolate_);

  // If the message is in band we lookup the receive port to dispatch to.  If
  // the receive port is closed, we drop the message without deserializing it.
  Object& receive_port = Object::Handle();
  if (!message->IsOOB()) {
    receive_port = DartLibraryCalls::LookupReceivePort(message->dest_port());
    if (receive_port.IsError()) {
      return ProcessUnhandledException(Instance::Handle(),
                                       Error::Cast(receive_port));
    }
    if (receive_port.IsNull()) {
      delete message;
      return true;
    }
  }

  // Parse the message.
  SnapshotReader reader(message->data(), message->len(),
                        Snapshot::kMessage, Isolate::Current());
  const Object& msg_obj = Object::Handle(reader.ReadObject());
  if (msg_obj.IsError()) {
    // An error occurred while reading the message.
    return ProcessUnhandledException(Instance::Handle(), Error::Cast(msg_obj));
  }
  if (!msg_obj.IsNull() && !msg_obj.IsInstance()) {
    // TODO(turnidge): We need to decide what an isolate does with
    // malformed messages.  If they (eventually) come from a remote
    // machine, then it might make sense to drop the message entirely.
    // In the case that the message originated locally, which is
    // always true for now, then this should never occur.
    UNREACHABLE();
  }

  Instance& msg = Instance::Handle();
  msg ^= msg_obj.raw();  // Can't use Instance::Cast because may be null.

  bool success = true;
  if (message->IsOOB()) {
    // For now the only OOB messages are Mirrors messages.
    HandleMirrorsMessage(isolate_, message->reply_port(), msg);
  } else {
    const Object& result = Object::Handle(
        DartLibraryCalls::HandleMessage(
            receive_port, message->reply_port(), msg));
    if (result.IsError()) {
      success = ProcessUnhandledException(msg, Error::Cast(result));
    } else {
      ASSERT(result.IsNull());
    }
  }
  delete message;
  return success;
}

RawFunction* IsolateMessageHandler::ResolveCallbackFunction() {
  ASSERT(isolate_->object_store()->unhandled_exception_handler() != NULL);
  String& callback_name = String::Handle(isolate_);
  if (isolate_->object_store()->unhandled_exception_handler() !=
      String::null()) {
    callback_name = isolate_->object_store()->unhandled_exception_handler();
  } else {
    callback_name = String::New("_unhandledExceptionCallback");
  }
  Library& lib =
      Library::Handle(isolate_, isolate_->object_store()->isolate_library());
  Function& func =
      Function::Handle(isolate_, lib.LookupLocalFunction(callback_name));
  if (func.IsNull()) {
    lib = isolate_->object_store()->root_library();
    func = lib.LookupLocalFunction(callback_name);
  }
  return func.raw();
}


bool IsolateMessageHandler::UnhandledExceptionCallbackHandler(
    const Object& message, const UnhandledException& error) {
  const Instance& cause = Instance::Handle(isolate_, error.exception());
  const Instance& stacktrace =
      Instance::Handle(isolate_, error.stacktrace());

  // Wrap these args into an IsolateUncaughtException object.
  const Array& exception_args = Array::Handle(Array::New(3));
  exception_args.SetAt(0, message);
  exception_args.SetAt(1, cause);
  exception_args.SetAt(2, stacktrace);
  const Object& exception =
      Object::Handle(isolate_,
                     Exceptions::Create(Exceptions::kIsolateUnhandledException,
                                        exception_args));
  if (exception.IsError()) {
    return false;
  }
  ASSERT(exception.IsInstance());

  // Invoke script's callback function.
  Object& function = Object::Handle(isolate_, ResolveCallbackFunction());
  if (function.IsNull() || function.IsError()) {
    return false;
  }
  const Array& callback_args = Array::Handle(Array::New(1));
  callback_args.SetAt(0, exception);
  const Object& result =
      Object::Handle(DartEntry::InvokeStatic(Function::Cast(function),
                                             callback_args));
  if (result.IsError()) {
    const Error& err = Error::Cast(result);
    OS::PrintErr("failed calling unhandled exception callback: %s\n",
                 err.ToErrorCString());
    return false;
  }

  ASSERT(result.IsBool());
  bool continue_from_exception = Bool::Cast(result).value();
  if (continue_from_exception) {
    isolate_->object_store()->clear_sticky_error();
  }
  return continue_from_exception;
}

#if defined(DEBUG)
void IsolateMessageHandler::CheckAccess() {
  ASSERT(IsCurrentIsolate());
}
#endif


bool IsolateMessageHandler::IsCurrentIsolate() const {
  return (isolate_ == Isolate::Current());
}


bool IsolateMessageHandler::ProcessUnhandledException(
    const Object& message, const Error& result) {
  if (result.IsUnhandledException()) {
    // Invoke the isolate's uncaught exception handler, if it exists.
    const UnhandledException& error = UnhandledException::Cast(result);
    RawInstance* exception = error.exception();
    if ((exception != isolate_->object_store()->out_of_memory()) &&
        (exception != isolate_->object_store()->stack_overflow())) {
      if (UnhandledExceptionCallbackHandler(message, error)) {
        return true;
      }
    }
  }

  // Invoke the isolate's unhandled exception callback if there is one.
  if (Isolate::UnhandledExceptionCallback() != NULL) {
    Dart_EnterScope();
    Dart_Handle error = Api::NewHandle(isolate_, result.raw());
    (Isolate::UnhandledExceptionCallback())(error);
    Dart_ExitScope();
  }

  isolate_->object_store()->set_sticky_error(result);
  return false;
}


#if defined(DEBUG)
// static
void BaseIsolate::AssertCurrent(BaseIsolate* isolate) {
  ASSERT(isolate == Isolate::Current());
}
#endif


Isolate::Isolate()
    : store_buffer_block_(),
      store_buffer_(),
      message_notify_callback_(NULL),
      name_(NULL),
      start_time_(OS::GetCurrentTimeMicros()),
      main_port_(0),
      heap_(NULL),
      object_store_(NULL),
      top_context_(Context::null()),
      top_exit_frame_info_(0),
      init_callback_data_(NULL),
      library_tag_handler_(NULL),
      api_state_(NULL),
      stub_code_(NULL),
      debugger_(NULL),
      simulator_(NULL),
      long_jump_base_(NULL),
      timer_list_(),
      deopt_id_(0),
      ic_data_array_(Array::null()),
      mutex_(new Mutex()),
      stack_limit_(0),
      saved_stack_limit_(0),
      message_handler_(NULL),
      spawn_data_(0),
      gc_prologue_callbacks_(),
      gc_epilogue_callbacks_(),
      deopt_cpu_registers_copy_(NULL),
      deopt_fpu_registers_copy_(NULL),
      deopt_frame_copy_(NULL),
      deopt_frame_copy_size_(0),
      deferred_doubles_(NULL),
      deferred_mints_(NULL) {
}


Isolate::~Isolate() {
  delete [] name_;
  delete heap_;
  delete object_store_;
  delete api_state_;
  delete stub_code_;
  delete debugger_;
#if defined(USING_SIMULATOR)
  delete simulator_;
#endif
  delete mutex_;
  mutex_ = NULL;  // Fail fast if interrupts are scheduled on a dead isolate.
  delete message_handler_;
  message_handler_ = NULL;  // Fail fast if we send messages to a dead isolate.
}

void Isolate::SetCurrent(Isolate* current) {
  Thread::SetThreadLocal(isolate_key, reinterpret_cast<uword>(current));
}


// The single thread local key which stores all the thread local data
// for a thread. Since an Isolate is the central repository for
// storing all isolate specific information a single thread local key
// is sufficient.
ThreadLocalKey Isolate::isolate_key = Thread::kUnsetThreadLocalKey;


void Isolate::InitOnce() {
  ASSERT(isolate_key == Thread::kUnsetThreadLocalKey);
  isolate_key = Thread::CreateThreadLocal();
  ASSERT(isolate_key != Thread::kUnsetThreadLocalKey);
  create_callback_ = NULL;
}


Isolate* Isolate::Init(const char* name_prefix) {
  Isolate* result = new Isolate();
  ASSERT(result != NULL);

  // TODO(5411455): For now just set the recently created isolate as
  // the current isolate.
  SetCurrent(result);

  // Setup the isolate message handler.
  MessageHandler* handler = new IsolateMessageHandler(result);
  ASSERT(handler != NULL);
  result->set_message_handler(handler);

  // Setup the Dart API state.
  ApiState* state = new ApiState();
  ASSERT(state != NULL);
  result->set_api_state(state);

  // Initialize stack top and limit in case we are running the isolate in the
  // main thread.
  // TODO(5411455): Need to figure out how to set the stack limit for the
  // main thread.
  result->SetStackLimitFromCurrentTOS(reinterpret_cast<uword>(&result));
  result->set_main_port(PortMap::CreatePort(result->message_handler()));
  result->BuildName(name_prefix);

  result->debugger_ = new Debugger();
  result->debugger_->Initialize(result);
  if (FLAG_trace_isolates) {
    if (name_prefix == NULL || strcmp(name_prefix, "vm-isolate") != 0) {
      OS::Print("[+] Starting isolate:\n"
                "\tisolate:    %s\n", result->name());
    }
  }
  return result;
}


void Isolate::BuildName(const char* name_prefix) {
  ASSERT(name_ == NULL);
  if (name_prefix == NULL) {
    name_prefix = "isolate";
  }
  const char* kFormat = "%s-%lld";
  intptr_t len = OS::SNPrint(NULL, 0, kFormat, name_prefix, main_port()) + 1;
  name_ = new char[len];
  OS::SNPrint(name_, len, kFormat, name_prefix, main_port());
}


// TODO(5411455): Use flag to override default value and Validate the
// stack size by querying OS.
uword Isolate::GetSpecifiedStackSize() {
  ASSERT(Isolate::kStackSizeBuffer < Thread::GetMaxStackSize());
  uword stack_size = Thread::GetMaxStackSize() - Isolate::kStackSizeBuffer;
  return stack_size;
}


void Isolate::SetStackLimitFromCurrentTOS(uword stack_top_value) {
  SetStackLimit(stack_top_value - GetSpecifiedStackSize());
}


void Isolate::SetStackLimit(uword limit) {
  MutexLocker ml(mutex_);
  if (stack_limit_ == saved_stack_limit_) {
    // No interrupt pending, set stack_limit_ too.
    stack_limit_ = limit;
  }
  saved_stack_limit_ = limit;
}


void Isolate::ScheduleInterrupts(uword interrupt_bits) {
  // TODO(turnidge): Can't use MutexLocker here because MutexLocker is
  // a StackResource, which requires a current isolate.  Should
  // MutexLocker really be a StackResource?
  mutex_->Lock();
  ASSERT((interrupt_bits & ~kInterruptsMask) == 0);  // Must fit in mask.
  if (stack_limit_ == saved_stack_limit_) {
    stack_limit_ = (~static_cast<uword>(0)) & ~kInterruptsMask;
  }
  stack_limit_ |= interrupt_bits;
  mutex_->Unlock();
}


uword Isolate::GetAndClearInterrupts() {
  MutexLocker ml(mutex_);
  if (stack_limit_ == saved_stack_limit_) {
    return 0;  // No interrupt was requested.
  }
  uword interrupt_bits = stack_limit_ & kInterruptsMask;
  stack_limit_ = saved_stack_limit_;
  return interrupt_bits;
}


ICData* Isolate::GetICDataForDeoptId(intptr_t deopt_id) const {
  if (ic_data_array() == Array::null()) {
    return &ICData::ZoneHandle();
  }
  const Array& array_handle = Array::Handle(ic_data_array());
  if (deopt_id >= array_handle.Length()) {
    // For computations being added in the optimizing compiler.
    return &ICData::ZoneHandle();
  }
  ICData& ic_data_handle = ICData::ZoneHandle();
  ic_data_handle ^= array_handle.At(deopt_id);
  return &ic_data_handle;
}


static int MostUsedFunctionFirst(const Function* const* a,
                                 const Function* const* b) {
  if ((*a)->usage_counter() > (*b)->usage_counter()) {
    return -1;
  } else if ((*a)->usage_counter() < (*b)->usage_counter()) {
    return 1;
  } else {
    return 0;
  }
}


static void AddFunctionsFromClass(const Class& cls,
                                  GrowableArray<const Function*>* functions) {
  const Array& class_functions = Array::Handle(cls.functions());
  // Class 'dynamic' is allocated/initialized in a special way, leaving
  // the functions field NULL instead of empty.
  const int func_len = class_functions.IsNull() ? 0 : class_functions.Length();
  for (int j = 0; j < func_len; j++) {
    Function& function = Function::Handle();
    function ^= class_functions.At(j);
    if (function.usage_counter() > 0) {
      functions->Add(&function);
    }
  }
}


void Isolate::PrintInvokedFunctions() {
  ASSERT(this == Isolate::Current());
  StackZone zone(this);
  HandleScope handle_scope(this);
  const GrowableObjectArray& libraries =
      GrowableObjectArray::Handle(object_store()->libraries());
  Library& library = Library::Handle();
  GrowableArray<const Function*> invoked_functions;
  for (int i = 0; i < libraries.Length(); i++) {
    library ^= libraries.At(i);
    Class& cls = Class::Handle();
    ClassDictionaryIterator iter(library);
    while (iter.HasNext()) {
      cls = iter.GetNextClass();
      AddFunctionsFromClass(cls, &invoked_functions);
    }
    Array& anon_classes = Array::Handle(library.raw_ptr()->anonymous_classes_);
    for (int i = 0; i < library.raw_ptr()->num_anonymous_; i++) {
      cls ^= anon_classes.At(i);
      AddFunctionsFromClass(cls, &invoked_functions);
    }
  }
  invoked_functions.Sort(MostUsedFunctionFirst);
  for (int i = 0; i < invoked_functions.length(); i++) {
    OS::Print("%10"Pd" x %s\n",
        invoked_functions[i]->usage_counter(),
        invoked_functions[i]->ToFullyQualifiedCString());
  }
}


class FinalizeWeakPersistentHandlesVisitor : public HandleVisitor {
 public:
  FinalizeWeakPersistentHandlesVisitor() {
  }

  void VisitHandle(uword addr) {
    FinalizablePersistentHandle* handle =
        reinterpret_cast<FinalizablePersistentHandle*>(addr);
    FinalizablePersistentHandle::Finalize(handle);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FinalizeWeakPersistentHandlesVisitor);
};


void Isolate::Shutdown() {
  ASSERT(this == Isolate::Current());
  ASSERT(top_resource() == NULL);
  ASSERT((heap_ == NULL) || heap_->Verify());

  // Clean up debugger resources. Shutting down the debugger
  // requires a handle zone. We must set up a temporary zone because
  // Isolate::Shutdown is called without a zone.
  {
    StackZone zone(this);
    HandleScope handle_scope(this);
    debugger_->Shutdown();
  }

  // Close all the ports owned by this isolate.
  PortMap::ClosePorts(message_handler());

  // Fail fast if anybody tries to post any more messsages to this isolate.
  delete message_handler();
  set_message_handler(NULL);

  // Finalize any weak persistent handles with a non-null referent.
  FinalizeWeakPersistentHandlesVisitor visitor;
  api_state()->weak_persistent_handles().VisitHandles(&visitor);

  // Dump all accumalated timer data for the isolate.
  timer_list_.ReportTimers();
  if (FLAG_report_usage_count) {
    PrintInvokedFunctions();
  }
  CompilerStats::Print();
  // TODO(asiva): Move this code to Dart::Cleanup when we have that method
  // as the cleanup for Dart::InitOnce.
  CodeObservers::DeleteAll();
  if (FLAG_trace_isolates) {
    StackZone zone(this);
    HandleScope handle_scope(this);
    heap()->PrintSizes();
    megamorphic_cache_table()->PrintSizes();
    Symbols::DumpStats();
    OS::Print("[-] Stopping isolate:\n"
              "\tisolate:    %s\n", name());
  }
  // TODO(5411455): For now just make sure there are no current isolates
  // as we are shutting down the isolate.
  SetCurrent(NULL);
}


Dart_IsolateCreateCallback Isolate::create_callback_ = NULL;
Dart_IsolateInterruptCallback Isolate::interrupt_callback_ = NULL;
Dart_IsolateUnhandledExceptionCallback
    Isolate::unhandled_exception_callback_ = NULL;
Dart_IsolateShutdownCallback Isolate::shutdown_callback_ = NULL;
Dart_FileOpenCallback Isolate::file_open_callback_ = NULL;
Dart_FileWriteCallback Isolate::file_write_callback_ = NULL;
Dart_FileCloseCallback Isolate::file_close_callback_ = NULL;


void Isolate::VisitObjectPointers(ObjectPointerVisitor* visitor,
                                  bool visit_prologue_weak_handles,
                                  bool validate_frames) {
  ASSERT(visitor != NULL);

  // Visit objects in the object store.
  object_store()->VisitObjectPointers(visitor);

  // Visit objects in the class table.
  class_table()->VisitObjectPointers(visitor);

  // Visit objects in the megamorphic cache.
  megamorphic_cache_table()->VisitObjectPointers(visitor);

  // Visit objects in per isolate stubs.
  StubCode::VisitObjectPointers(visitor);

  // Visit objects in zones.
  current_zone()->VisitObjectPointers(visitor);

  // Iterate over all the stack frames and visit objects on the stack.
  StackFrameIterator frames_iterator(validate_frames);
  StackFrame* frame = frames_iterator.NextFrame();
  while (frame != NULL) {
    frame->VisitObjectPointers(visitor);
    frame = frames_iterator.NextFrame();
  }

  // Visit the dart api state for all local and persistent handles.
  if (api_state() != NULL) {
    api_state()->VisitObjectPointers(visitor, visit_prologue_weak_handles);
  }

  // Visit the top context which is stored in the isolate.
  visitor->VisitPointer(reinterpret_cast<RawObject**>(&top_context_));

  // Visit the currently active IC data array.
  visitor->VisitPointer(reinterpret_cast<RawObject**>(&ic_data_array_));

  // Visit objects in the debugger.
  debugger()->VisitObjectPointers(visitor);
}


void Isolate::VisitWeakPersistentHandles(HandleVisitor* visitor,
                                         bool visit_prologue_weak_handles) {
  if (api_state() != NULL) {
    api_state()->VisitWeakHandles(visitor, visit_prologue_weak_handles);
  }
}

}  // namespace dart
