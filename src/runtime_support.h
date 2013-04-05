/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_SRC_RUNTIME_SUPPORT_H_
#define ART_SRC_RUNTIME_SUPPORT_H_

#include "class_linker.h"
#include "common_throws.h"
#include "dex_file.h"
#include "indirect_reference_table.h"
#include "invoke_type.h"
#include "jni_internal.h"
#include "mirror/abstract_method.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/throwable.h"
#include "object_utils.h"
#include "thread.h"

extern "C" void art_interpreter_invoke_handler();
extern "C" void art_portable_proxy_invoke_handler();
extern "C" void art_quick_proxy_invoke_handler();
extern "C" void art_work_around_app_jni_bugs();

extern "C" double art_l2d(int64_t l);
extern "C" float art_l2f(int64_t l);
extern "C" int64_t art_d2l(double d);
extern "C" int32_t art_d2i(double d);
extern "C" int64_t art_f2l(float f);
extern "C" int32_t art_f2i(float f);

namespace art {
namespace mirror {
class Class;
class Field;
class Object;
}

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static inline mirror::Object* AllocObjectFromCode(uint32_t type_idx, mirror::AbstractMethod* method,
                                                  Thread* self,
                                                  bool access_check)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  Runtime* runtime = Runtime::Current();
  if (UNLIKELY(klass == NULL)) {
    klass = runtime->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {
      DCHECK(self->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (access_check) {
    if (UNLIKELY(!klass->IsInstantiable())) {
      self->ThrowNewException("Ljava/lang/InstantiationError;",
                              PrettyDescriptor(klass).c_str());
      return NULL;  // Failure
    }
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      return NULL;  // Failure
    }
  }
  if (!klass->IsInitialized() &&
      !runtime->GetClassLinker()->EnsureInitialized(klass, true, true)) {
    DCHECK(self->IsExceptionPending());
    return NULL;  // Failure
  }
  return klass->AllocObject(self);
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
static inline mirror::Array* AllocArrayFromCode(uint32_t type_idx, mirror::AbstractMethod* method,
                                                int32_t component_count,
                                                Thread* self, bool access_check)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (UNLIKELY(component_count < 0)) {
    self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", component_count);
    return NULL;  // Failure
  }
  mirror::Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
  }
  if (access_check) {
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      return NULL;  // Failure
    }
  }
  return mirror::Array::Alloc(self, klass, component_count);
}

extern mirror::Array* CheckAndAllocArrayFromCode(uint32_t type_idx, mirror::AbstractMethod* method,
                                                 int32_t component_count,
                                                 Thread* self, bool access_check)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// Type of find field operation for fast and slow case.
enum FindFieldType {
  InstanceObjectRead,
  InstanceObjectWrite,
  InstancePrimitiveRead,
  InstancePrimitiveWrite,
  StaticObjectRead,
  StaticObjectWrite,
  StaticPrimitiveRead,
  StaticPrimitiveWrite,
};

// Slow field find that can initialize classes and may throw exceptions.
extern mirror::Field* FindFieldFromCode(uint32_t field_idx, const mirror::AbstractMethod* referrer,
                                        Thread* self, FindFieldType type, size_t expected_size)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// Fast path field resolution that can't initialize classes or throw exceptions.
static inline mirror::Field* FindFieldFast(uint32_t field_idx,
                                           const mirror::AbstractMethod* referrer,
                                           FindFieldType type, size_t expected_size)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Field* resolved_field =
      referrer->GetDeclaringClass()->GetDexCache()->GetResolvedField(field_idx);
  if (UNLIKELY(resolved_field == NULL)) {
    return NULL;
  }
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  // Check class is initiliazed or initializing.
  if (UNLIKELY(!fields_class->IsInitializing())) {
    return NULL;
  }
  // Check for incompatible class change.
  bool is_primitive;
  bool is_set;
  bool is_static;
  switch (type) {
    case InstanceObjectRead:     is_primitive = false; is_set = false; is_static = false; break;
    case InstanceObjectWrite:    is_primitive = false; is_set = true;  is_static = false; break;
    case InstancePrimitiveRead:  is_primitive = true;  is_set = false; is_static = false; break;
    case InstancePrimitiveWrite: is_primitive = true;  is_set = true;  is_static = false; break;
    case StaticObjectRead:       is_primitive = false; is_set = false; is_static = true;  break;
    case StaticObjectWrite:      is_primitive = false; is_set = true;  is_static = true;  break;
    case StaticPrimitiveRead:    is_primitive = true;  is_set = false; is_static = true;  break;
    case StaticPrimitiveWrite:   is_primitive = true;  is_set = true;  is_static = true;  break;
    default: LOG(FATAL) << "UNREACHABLE";  // Assignment below to avoid GCC warnings.
             is_primitive = true;  is_set = true;  is_static = true;  break;
  }
  if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
    // Incompatible class change.
    return NULL;
  }
  mirror::Class* referring_class = referrer->GetDeclaringClass();
  if (UNLIKELY(!referring_class->CanAccess(fields_class) ||
               !referring_class->CanAccessMember(fields_class,
                                                 resolved_field->GetAccessFlags()) ||
               (is_set && resolved_field->IsFinal() && (fields_class != referring_class)))) {
    // Illegal access.
    return NULL;
  }
  FieldHelper fh(resolved_field);
  if (UNLIKELY(fh.IsPrimitiveType() != is_primitive ||
               fh.FieldSize() != expected_size)) {
    return NULL;
  }
  return resolved_field;
}

// Fast path method resolution that can't throw exceptions.
static inline mirror::AbstractMethod* FindMethodFast(uint32_t method_idx,
                                                     mirror::Object* this_object,
                                                     const mirror::AbstractMethod* referrer,
                                                     bool access_check, InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  bool is_direct = type == kStatic || type == kDirect;
  if (UNLIKELY(this_object == NULL && !is_direct)) {
    return NULL;
  }
  mirror::AbstractMethod* resolved_method =
      referrer->GetDeclaringClass()->GetDexCache()->GetResolvedMethod(method_idx);
  if (UNLIKELY(resolved_method == NULL)) {
    return NULL;
  }
  if (access_check) {
    // Check for incompatible class change errors and access.
    bool icce = resolved_method->CheckIncompatibleClassChange(type);
    if (UNLIKELY(icce)) {
      return NULL;
    }
    mirror::Class* methods_class = resolved_method->GetDeclaringClass();
    mirror::Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CanAccess(methods_class) ||
                 !referring_class->CanAccessMember(methods_class,
                                                   resolved_method->GetAccessFlags()))) {
      // Potential illegal access, may need to refine the method's class.
      return NULL;
    }
  }
  if (type == kInterface) {  // Most common form of slow path dispatch.
    return this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
  } else if (is_direct) {
    return resolved_method;
  } else if (type == kSuper) {
    return referrer->GetDeclaringClass()->GetSuperClass()->GetVTable()->
        Get(resolved_method->GetMethodIndex());
  } else {
    DCHECK(type == kVirtual);
    return this_object->GetClass()->GetVTable()->Get(resolved_method->GetMethodIndex());
  }
}

extern mirror::AbstractMethod* FindMethodFromCode(uint32_t method_idx, mirror::Object* this_object,
                                                  mirror::AbstractMethod* referrer,
                                                  Thread* self, bool access_check, InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

extern mirror::Class* ResolveVerifyAndClinit(uint32_t type_idx,
                                             const mirror::AbstractMethod* referrer, Thread* self,
                                             bool can_run_clinit, bool verify_access)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

extern void ThrowStackOverflowError(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

static inline mirror::String* ResolveStringFromCode(const mirror::AbstractMethod* referrer,
                                                    uint32_t string_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveString(string_idx, referrer);
}

static inline void UnlockJniSynchronizedMethod(jobject locked, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
    UNLOCK_FUNCTION(monitor_lock_) {
  // Save any pending exception over monitor exit call.
  mirror::Throwable* saved_exception = NULL;
  if (UNLIKELY(self->IsExceptionPending())) {
    saved_exception = self->GetException();
    self->ClearException();
  }
  // Decode locked object and unlock, before popping local references.
  self->DecodeJObject(locked)->MonitorExit(self);
  if (UNLIKELY(self->IsExceptionPending())) {
    LOG(FATAL) << "Synchronized JNI code returning with an exception:\n"
        << saved_exception->Dump()
        << "\nEncountered second exception during implicit MonitorExit:\n"
        << self->GetException()->Dump();
  }
  // Restore pending exception.
  if (saved_exception != NULL) {
    self->SetException(saved_exception);
  }
}

static inline void CheckReferenceResult(mirror::Object* o, Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (o == NULL) {
    return;
  }
  if (o == kInvalidIndirectRefObject) {
    JniAbortF(NULL, "invalid reference returned from %s",
              PrettyMethod(self->GetCurrentMethod()).c_str());
  }
  // Make sure that the result is an instance of the type this method was expected to return.
  mirror::AbstractMethod* m = self->GetCurrentMethod();
  MethodHelper mh(m);
  mirror::Class* return_type = mh.GetReturnType();

  if (!o->InstanceOf(return_type)) {
    JniAbortF(NULL, "attempt to return an instance of %s from %s",
              PrettyTypeOf(o).c_str(), PrettyMethod(m).c_str());
  }
}

static inline void CheckSuspend(Thread* thread) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  for (;;) {
    if (thread->ReadFlag(kCheckpointRequest)) {
      thread->RunCheckpointFunction();
      thread->AtomicClearFlag(kCheckpointRequest);
    } else if (thread->ReadFlag(kSuspendRequest)) {
      thread->FullSuspendCheck();
    } else {
      break;
    }
  }
}

JValue InvokeProxyInvocationHandler(ScopedObjectAccessUnchecked& soa, const char* shorty,
                                    jobject rcvr_jobj, jobject interface_method_jobj,
                                    std::vector<jvalue>& args)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) ;

}  // namespace art

#endif  // ART_SRC_RUNTIME_SUPPORT_H_
