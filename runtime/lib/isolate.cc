// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/assert.h"
#include "vm/bootstrap_natives.h"
#include "vm/class_finalizer.h"
#include "vm/dart.h"
#include "vm/dart_api_impl.h"
#include "vm/dart_entry.h"
#include "vm/exceptions.h"
#include "vm/longjump.h"
#include "vm/message_handler.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/port.h"
#include "vm/resolver.h"
#include "vm/snapshot.h"
#include "vm/symbols.h"
#include "vm/thread.h"

namespace dart {

class IsolateStartData {
 public:
  IsolateStartData(char* library_url,
                   char* class_name,
                   intptr_t port_id)
      : library_url_(library_url),
        class_name_(class_name),
        port_id_(port_id) {}

  char* library_url_;
  char* class_name_;
  intptr_t port_id_;
};


static uint8_t* allocator(uint8_t* ptr, intptr_t old_size, intptr_t new_size) {
  void* new_ptr = realloc(reinterpret_cast<void*>(ptr), new_size);
  return reinterpret_cast<uint8_t*>(new_ptr);
}


static void StoreError(Isolate* isolate, const Object& obj) {
  ASSERT(obj.IsError());
  isolate->object_store()->set_sticky_error(Error::Cast(obj));
}


// TODO(turnidge): Move to DartLibraryCalls.
static RawObject* ReceivePortCreate(intptr_t port_id) {
  Isolate* isolate = Isolate::Current();
  Function& func =
      Function::Handle(isolate,
                       isolate->object_store()->receive_port_create_function());
  const int kNumArguments = 1;
  if (func.IsNull()) {
    Library& isolate_lib = Library::Handle(Library::IsolateLibrary());
    ASSERT(!isolate_lib.IsNull());
    const String& class_name =
        String::Handle(isolate_lib.PrivateName(Symbols::_ReceivePortImpl()));
    const String& function_name =
        String::Handle(isolate_lib.PrivateName(Symbols::_get_or_create()));
    func = Resolver::ResolveStatic(isolate_lib,
                                   class_name,
                                   function_name,
                                   kNumArguments,
                                   Object::empty_array(),
                                   Resolver::kIsQualified);
    ASSERT(!func.IsNull());
    isolate->object_store()->set_receive_port_create_function(func);
  }
  const Array& args = Array::Handle(isolate, Array::New(kNumArguments));
  args.SetAt(0, Integer::Handle(isolate, Integer::New(port_id)));
  const Object& result =
      Object::Handle(isolate, DartEntry::InvokeStatic(func, args));
  if (!result.IsError()) {
    PortMap::SetLive(port_id);
  }
  return result.raw();
}


static void ShutdownIsolate(uword parameter) {
  Isolate* isolate = reinterpret_cast<Isolate*>(parameter);
  {
    // Print the error if there is one.  This may execute dart code to
    // print the exception object, so we need to use a StartIsolateScope.
    StartIsolateScope start_scope(isolate);
    StackZone zone(isolate);
    HandleScope handle_scope(isolate);
    Error& error = Error::Handle();
    error = isolate->object_store()->sticky_error();
    if (!error.IsNull()) {
      OS::PrintErr("in ShutdownIsolate: %s\n", error.ToErrorCString());
    }
  }
  {
    // Shut the isolate down.
    SwitchIsolateScope switch_scope(isolate);
    Dart::ShutdownIsolate();
  }
}


static char* GetRootScriptUri(Isolate* isolate) {
  const Library& library =
      Library::Handle(isolate->object_store()->root_library());
  ASSERT(!library.IsNull());
  const String& script_name = String::Handle(library.url());
  return isolate->current_zone()->MakeCopyOfString(script_name.ToCString());
}


DEFINE_NATIVE_ENTRY(ReceivePortImpl_factory, 1) {
  ASSERT(AbstractTypeArguments::CheckedHandle(
      arguments->NativeArgAt(0)).IsNull());
  intptr_t port_id =
      PortMap::CreatePort(arguments->isolate()->message_handler());
  const Object& port = Object::Handle(ReceivePortCreate(port_id));
  if (port.IsError()) {
    Exceptions::PropagateError(Error::Cast(port));
  }
  return port.raw();
}


DEFINE_NATIVE_ENTRY(ReceivePortImpl_closeInternal, 1) {
  GET_NON_NULL_NATIVE_ARGUMENT(Smi, id, arguments->NativeArgAt(0));
  PortMap::ClosePort(id.Value());
  return Object::null();
}


DEFINE_NATIVE_ENTRY(SendPortImpl_sendInternal_, 3) {
  GET_NON_NULL_NATIVE_ARGUMENT(Smi, send_id, arguments->NativeArgAt(0));
  GET_NON_NULL_NATIVE_ARGUMENT(Smi, reply_id, arguments->NativeArgAt(1));
  // TODO(iposva): Allow for arbitrary messages to be sent.
  GET_NON_NULL_NATIVE_ARGUMENT(Instance, obj, arguments->NativeArgAt(2));

  uint8_t* data = NULL;
  MessageWriter writer(&data, &allocator);
  writer.WriteMessage(obj);

  // TODO(turnidge): Throw an exception when the return value is false?
  PortMap::PostMessage(new Message(send_id.Value(), reply_id.Value(),
                                   data, writer.BytesWritten(),
                                   Message::kNormalPriority));
  return Object::null();
}


static void ThrowIllegalArgException(const String& message) {
  const Array& args = Array::Handle(Array::New(1));
  args.SetAt(0, message);
  Exceptions::ThrowByType(Exceptions::kArgument, args);
}


static void ThrowIsolateSpawnException(const String& message) {
  const Array& args = Array::Handle(Array::New(1));
  args.SetAt(0, message);
  Exceptions::ThrowByType(Exceptions::kIsolateSpawn, args);
}


static bool CanonicalizeUri(Isolate* isolate,
                            const Library& library,
                            const String& uri,
                            char** canonical_uri,
                            char** error) {
  Zone* zone = isolate->current_zone();
  Dart_LibraryTagHandler handler = isolate->library_tag_handler();
  if (handler == NULL) {
    *error = zone->PrintToString(
        "Unable to canonicalize uri '%s': no library tag handler found.",
        uri.ToCString());
    return false;
  }
  Dart_Handle result = handler(kCanonicalizeUrl,
                               Api::NewHandle(isolate, library.raw()),
                               Api::NewHandle(isolate, uri.raw()));
  const Object& obj = Object::Handle(Api::UnwrapHandle(result));
  if (obj.IsError()) {
    Error& error_obj = Error::Handle();
    error_obj ^= obj.raw();
    *error = zone->PrintToString("Unable to canonicalize uri '%s': %s",
                                 uri.ToCString(), error_obj.ToErrorCString());
    return false;
  } else if (obj.IsString()) {
    *canonical_uri = zone->MakeCopyOfString(String::Cast(obj).ToCString());
    return true;
  } else {
    *error = zone->PrintToString("Unable to canonicalize uri '%s': "
                                 "library tag handler returned wrong type",
                                 uri.ToCString());
    return false;
  }
}


class SpawnState {
 public:
  SpawnState(const Function& func, const Function& callback_func)
      : isolate_(NULL),
        script_url_(NULL),
        library_url_(NULL),
        function_name_(NULL),
        exception_callback_name_(NULL) {
    script_url_ = strdup(GetRootScriptUri(Isolate::Current()));
    const Class& cls = Class::Handle(func.Owner());
    ASSERT(cls.IsTopLevel());
    const Library& lib = Library::Handle(cls.library());
    const String& lib_url = String::Handle(lib.url());
    library_url_ = strdup(lib_url.ToCString());

    const String& func_name = String::Handle(func.name());
    function_name_ = strdup(func_name.ToCString());
    if (!callback_func.IsNull()) {
      const String& callback_name = String::Handle(callback_func.name());
      exception_callback_name_ = strdup(callback_name.ToCString());
    } else {
      exception_callback_name_ = strdup("_unhandledExceptionCallback");
    }
  }

  explicit SpawnState(const char* script_url)
      : isolate_(NULL),
        library_url_(NULL),
        function_name_(NULL),
        exception_callback_name_(NULL) {
    script_url_ = strdup(script_url);
    library_url_ = NULL;
    function_name_ = strdup("main");
    exception_callback_name_ = strdup("_unhandledExceptionCallback");
  }

  ~SpawnState() {
    free(script_url_);
    free(library_url_);
    free(function_name_);
    free(exception_callback_name_);
  }

  Isolate* isolate() const { return isolate_; }
  void set_isolate(Isolate* value) { isolate_ = value; }
  char* script_url() const { return script_url_; }
  char* library_url() const { return library_url_; }
  char* function_name() const { return function_name_; }
  char* exception_callback_name() const { return exception_callback_name_; }

  RawObject* ResolveFunction() {
    // Resolve the library.
    Library& lib = Library::Handle();
    if (library_url()) {
      const String& lib_url = String::Handle(String::New(library_url()));
      lib = Library::LookupLibrary(lib_url);
      if (lib.IsNull() || lib.IsError()) {
        const String& msg = String::Handle(String::NewFormatted(
            "Unable to find library '%s'.", library_url()));
        return LanguageError::New(msg);
      }
    } else {
      lib = isolate()->object_store()->root_library();
    }
    ASSERT(!lib.IsNull());

    // Resolve the function.
    const String& func_name =
        String::Handle(String::New(function_name()));
    const Function& func = Function::Handle(lib.LookupLocalFunction(func_name));
    if (func.IsNull()) {
      const String& msg = String::Handle(String::NewFormatted(
          "Unable to resolve function '%s' in library '%s'.",
          function_name(), (library_url() ? library_url() : script_url())));
      return LanguageError::New(msg);
    }
    return func.raw();
  }

  void Cleanup() {
    SwitchIsolateScope switch_scope(isolate());
    Dart::ShutdownIsolate();
  }

 private:
  Isolate* isolate_;
  char* script_url_;
  char* library_url_;
  char* function_name_;
  char* exception_callback_name_;
};


static bool CreateIsolate(SpawnState* state, char** error) {
  Isolate* parent_isolate = Isolate::Current();

  Dart_IsolateCreateCallback callback = Isolate::CreateCallback();
  if (callback == NULL) {
    *error = strdup("Null callback specified for isolate creation\n");
    Isolate::SetCurrent(parent_isolate);
    return false;
  }

  void* init_data = parent_isolate->init_callback_data();
  bool retval = (callback)(state->script_url(),
                           state->function_name(),
                           init_data,
                           error);
  if (!retval) {
    Isolate::SetCurrent(parent_isolate);
    return false;
  }

  Isolate* child_isolate = Isolate::Current();
  ASSERT(child_isolate);
  state->set_isolate(child_isolate);

  // Attempt to resolve the entry function now, so that we fail fast
  // in the case that the function cannot be resolved.
  //
  // TODO(turnidge): Revisit this once we have an isolate death api.
  bool resolve_error = false;
  {
    StackZone zone(child_isolate);
    HandleScope handle_scope(child_isolate);
    const Object& result = Object::Handle(state->ResolveFunction());
    if (result.IsError()) {
      Error& errobj = Error::Handle();
      errobj ^= result.raw();
      *error = strdup(errobj.ToErrorCString());
      resolve_error = true;
    } else {
      const String& callback_name =
          String::Handle(child_isolate,
                         String::New(state->exception_callback_name()));
      child_isolate->object_store()->
          set_unhandled_exception_handler(callback_name);
    }
  }
  if (resolve_error) {
    Dart::ShutdownIsolate();
    Isolate::SetCurrent(parent_isolate);
    return false;
  }

  Isolate::SetCurrent(parent_isolate);
  return true;
}


static bool RunIsolate(uword parameter) {
  Isolate* isolate = reinterpret_cast<Isolate*>(parameter);
  SpawnState* state = reinterpret_cast<SpawnState*>(isolate->spawn_data());
  isolate->set_spawn_data(0);
  {
    StartIsolateScope start_scope(isolate);
    StackZone zone(isolate);
    HandleScope handle_scope(isolate);
    if (!ClassFinalizer::FinalizePendingClasses()) {
      // Error is in sticky error already.
      return false;
    }

    Object& result = Object::Handle();
    result = state->ResolveFunction();
    delete state;
    state = NULL;
    if (result.IsError()) {
      StoreError(isolate, result);
      return false;
    }
    ASSERT(result.IsFunction());
    Function& func = Function::Handle(isolate);
    func ^= result.raw();
    result = DartEntry::InvokeStatic(func, Object::empty_array());
    if (result.IsError()) {
      StoreError(isolate, result);
      return false;
    }
  }
  return true;
}


static RawObject* Spawn(NativeArguments* arguments, SpawnState* state) {
  // Create a new isolate.
  char* error = NULL;
  if (!CreateIsolate(state, &error)) {
    delete state;
    const String& msg = String::Handle(String::New(error));
    free(error);
    ThrowIsolateSpawnException(msg);
  }

  // Try to create a SendPort for the new isolate.
  const Object& port = Object::Handle(
      DartLibraryCalls::NewSendPort(state->isolate()->main_port()));
  if (port.IsError()) {
    state->Cleanup();
    delete state;
    Exceptions::PropagateError(Error::Cast(port));
  }

  // Start the new isolate.
  state->isolate()->set_spawn_data(reinterpret_cast<uword>(state));
  state->isolate()->message_handler()->Run(
      Dart::thread_pool(), RunIsolate, ShutdownIsolate,
      reinterpret_cast<uword>(state->isolate()));

  return port.raw();
}


DEFINE_NATIVE_ENTRY(isolate_spawnFunction, 2) {
  GET_NON_NULL_NATIVE_ARGUMENT(Instance, closure, arguments->NativeArgAt(0));
  bool throw_exception = false;
  Function& func = Function::Handle();
  if (closure.IsClosure()) {
    func = Closure::function(closure);
    const Class& cls = Class::Handle(func.Owner());
    if (!func.IsClosureFunction() || !func.is_static() || !cls.IsTopLevel()) {
      throw_exception = true;
    }
  } else {
    throw_exception = true;
  }
  if (throw_exception) {
    const String& msg = String::Handle(String::New(
        "spawnFunction expects to be passed a closure to a top-level static "
        "function"));
    ThrowIllegalArgException(msg);
  }

  GET_NATIVE_ARGUMENT(Instance, callback, arguments->NativeArgAt(1));
  Function& callback_func = Function::Handle();
  if (callback.IsClosure()) {
    callback_func = Closure::function(callback);
    const Class& cls = Class::Handle(callback_func.Owner());
    if (!callback_func.IsClosureFunction() || !callback_func.is_static() ||
        !cls.IsTopLevel()) {
      throw_exception = true;
    }
  } else if (!callback.IsNull()) {
    throw_exception = true;
  }
  if (throw_exception) {
    const String& msg = String::Handle(String::New(
        "spawnFunction expects to be passed either a unhandled exception "
        "callback to a top-level static function, or null"));
    ThrowIllegalArgException(msg);
  }

#if defined(DEBUG)
  Context& ctx = Context::Handle();
  ctx = Closure::context(closure);
  ASSERT(ctx.num_variables() == 0);
  if (!callback.IsNull()) {
    ctx = Closure::context(callback);
    ASSERT(ctx.num_variables() == 0);
  }
#endif

  return Spawn(arguments, new SpawnState(func, callback_func));
}


DEFINE_NATIVE_ENTRY(isolate_spawnUri, 1) {
  GET_NON_NULL_NATIVE_ARGUMENT(String, uri, arguments->NativeArgAt(0));

  // Canonicalize the uri with respect to the current isolate.
  char* error = NULL;
  char* canonical_uri = NULL;
  const Library& root_lib =
      Library::Handle(arguments->isolate()->object_store()->root_library());
  if (!CanonicalizeUri(arguments->isolate(), root_lib, uri,
                       &canonical_uri, &error)) {
    const String& msg = String::Handle(String::New(error));
    free(error);
    ThrowIsolateSpawnException(msg);
  }

  return Spawn(arguments, new SpawnState(canonical_uri));
}


DEFINE_NATIVE_ENTRY(isolate_getPortInternal, 0) {
  const Object& port = Object::Handle(ReceivePortCreate(isolate->main_port()));
  if (port.IsError()) {
    Exceptions::PropagateError(Error::Cast(port));
  }
  return port.raw();
}

}  // namespace dart
