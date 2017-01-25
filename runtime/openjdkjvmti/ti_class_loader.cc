/* Copyright (C) 2017 The Android Open Source Project
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This file implements interfaces from the file jvmti.h. This implementation
 * is licensed under the same terms as the file jvmti.h.  The
 * copyright and license information for the file jvmti.h follows.
 *
 * Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "ti_class_loader.h"

#include <limits>

#include "android-base/stringprintf.h"

#include "art_jvmti.h"
#include "base/array_slice.h"
#include "base/logging.h"
#include "dex_file.h"
#include "dex_file_types.h"
#include "events-inl.h"
#include "gc/allocation_listener.h"
#include "gc/heap.h"
#include "instrumentation.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jni_env_ext-inl.h"
#include "jvmti_allocator.h"
#include "mirror/class.h"
#include "mirror/class_ext.h"
#include "mirror/object.h"
#include "object_lock.h"
#include "runtime.h"
#include "ScopedLocalRef.h"
#include "transform.h"

namespace openjdkjvmti {

bool ClassLoaderHelper::AddToClassLoader(art::Thread* self,
                                         art::Handle<art::mirror::ClassLoader> loader,
                                         const art::DexFile* dex_file) {
  art::StackHandleScope<2> hs(self);
  art::Handle<art::mirror::Object> java_dex_file_obj(hs.NewHandle(FindSourceDexFileObject(self,
                                                                                          loader)));
  if (java_dex_file_obj.IsNull()) {
    return false;
  }
  art::Handle<art::mirror::LongArray> cookie(hs.NewHandle(
      AllocateNewDexFileCookie(self, java_dex_file_obj, dex_file)));
  if (cookie.IsNull()) {
    return false;
  }
  art::ScopedAssertNoThreadSuspension nts("Replacing cookie fields in j.l.DexFile object");
  UpdateJavaDexFile(java_dex_file_obj.Get(), cookie.Get());
  return true;
}

void ClassLoaderHelper::UpdateJavaDexFile(art::ObjPtr<art::mirror::Object> java_dex_file,
                                          art::ObjPtr<art::mirror::LongArray> new_cookie) {
  art::ArtField* internal_cookie_field = java_dex_file->GetClass()->FindDeclaredInstanceField(
      "mInternalCookie", "Ljava/lang/Object;");
  art::ArtField* cookie_field = java_dex_file->GetClass()->FindDeclaredInstanceField(
      "mCookie", "Ljava/lang/Object;");
  CHECK(internal_cookie_field != nullptr);
  art::ObjPtr<art::mirror::LongArray> orig_internal_cookie(
      internal_cookie_field->GetObject(java_dex_file)->AsLongArray());
  art::ObjPtr<art::mirror::LongArray> orig_cookie(
      cookie_field->GetObject(java_dex_file)->AsLongArray());
  internal_cookie_field->SetObject<false>(java_dex_file, new_cookie);
  if (!orig_cookie.IsNull()) {
    cookie_field->SetObject<false>(java_dex_file, new_cookie);
  }
}

// TODO Really wishing I had that mirror of java.lang.DexFile now.
art::ObjPtr<art::mirror::LongArray> ClassLoaderHelper::AllocateNewDexFileCookie(
    art::Thread* self,
    art::Handle<art::mirror::Object> java_dex_file_obj,
    const art::DexFile* dex_file) {
  art::StackHandleScope<2> hs(self);
  // mCookie is nulled out if the DexFile has been closed but mInternalCookie sticks around until
  // the object is finalized. Since they always point to the same array if mCookie is not null we
  // just use the mInternalCookie field. We will update one or both of these fields later.
  // TODO Should I get the class from the classloader or directly?
  art::ArtField* internal_cookie_field = java_dex_file_obj->GetClass()->FindDeclaredInstanceField(
      "mInternalCookie", "Ljava/lang/Object;");
  // TODO Add check that mCookie is either null or same as mInternalCookie
  CHECK(internal_cookie_field != nullptr);
  art::Handle<art::mirror::LongArray> cookie(
      hs.NewHandle(internal_cookie_field->GetObject(java_dex_file_obj.Get())->AsLongArray()));
  // TODO Maybe make these non-fatal.
  CHECK(cookie.Get() != nullptr);
  CHECK_GE(cookie->GetLength(), 1);
  art::Handle<art::mirror::LongArray> new_cookie(
      hs.NewHandle(art::mirror::LongArray::Alloc(self, cookie->GetLength() + 1)));
  if (new_cookie.Get() == nullptr) {
    self->AssertPendingOOMException();
    return nullptr;
  }
  // Copy the oat-dex field at the start.
  // TODO Should I clear this field?
  // TODO This is a really crappy thing here with the first element being different.
  new_cookie->SetWithoutChecks<false>(0, cookie->GetWithoutChecks(0));
  new_cookie->SetWithoutChecks<false>(
      1, static_cast<int64_t>(reinterpret_cast<intptr_t>(dex_file)));
  new_cookie->Memcpy(2, cookie.Get(), 1, cookie->GetLength() - 1);
  return new_cookie.Get();
}

// TODO This should return the actual source java.lang.DexFile object for the klass being loaded.
art::ObjPtr<art::mirror::Object> ClassLoaderHelper::FindSourceDexFileObject(
    art::Thread* self, art::Handle<art::mirror::ClassLoader> loader) {
  const char* dex_path_list_element_array_name = "[Ldalvik/system/DexPathList$Element;";
  const char* dex_path_list_element_name = "Ldalvik/system/DexPathList$Element;";
  const char* dex_file_name = "Ldalvik/system/DexFile;";
  const char* dex_path_list_name = "Ldalvik/system/DexPathList;";
  const char* dex_class_loader_name = "Ldalvik/system/BaseDexClassLoader;";

  CHECK(!self->IsExceptionPending());
  art::StackHandleScope<5> hs(self);
  art::ClassLinker* class_linker = art::Runtime::Current()->GetClassLinker();

  art::Handle<art::mirror::ClassLoader> null_loader(hs.NewHandle<art::mirror::ClassLoader>(
      nullptr));
  art::Handle<art::mirror::Class> base_dex_loader_class(hs.NewHandle(class_linker->FindClass(
      self, dex_class_loader_name, null_loader)));

  // Get all the ArtFields so we can look in the BaseDexClassLoader
  art::ArtField* path_list_field = base_dex_loader_class->FindDeclaredInstanceField(
      "pathList", dex_path_list_name);
  CHECK(path_list_field != nullptr);

  art::ArtField* dex_path_list_element_field =
      class_linker->FindClass(self, dex_path_list_name, null_loader)
        ->FindDeclaredInstanceField("dexElements", dex_path_list_element_array_name);
  CHECK(dex_path_list_element_field != nullptr);

  art::ArtField* element_dex_file_field =
      class_linker->FindClass(self, dex_path_list_element_name, null_loader)
        ->FindDeclaredInstanceField("dexFile", dex_file_name);
  CHECK(element_dex_file_field != nullptr);

  // Check if loader is a BaseDexClassLoader
  art::Handle<art::mirror::Class> loader_class(hs.NewHandle(loader->GetClass()));
  // Currently only base_dex_loader is allowed to actually define classes but if this changes in the
  // future we should make sure to support all class loader types.
  if (!loader_class->IsSubClass(base_dex_loader_class.Get())) {
    LOG(ERROR) << "The classloader is not a BaseDexClassLoader which is currently the only "
               << "supported class loader type!";
    return nullptr;
  }
  // Start navigating the fields of the loader (now known to be a BaseDexClassLoader derivative)
  art::Handle<art::mirror::Object> path_list(
      hs.NewHandle(path_list_field->GetObject(loader.Get())));
  CHECK(path_list.Get() != nullptr);
  CHECK(!self->IsExceptionPending());
  art::Handle<art::mirror::ObjectArray<art::mirror::Object>> dex_elements_list(hs.NewHandle(
      dex_path_list_element_field->GetObject(path_list.Get())->
      AsObjectArray<art::mirror::Object>()));
  CHECK(!self->IsExceptionPending());
  CHECK(dex_elements_list.Get() != nullptr);
  size_t num_elements = dex_elements_list->GetLength();
  // Iterate over the DexPathList$Element to find the right one
  for (size_t i = 0; i < num_elements; i++) {
    art::ObjPtr<art::mirror::Object> current_element = dex_elements_list->Get(i);
    CHECK(!current_element.IsNull());
    // TODO It would be cleaner to put the art::DexFile into the dalvik.system.DexFile the class
    // comes from but it is more annoying because we would need to find this class. It is not
    // necessary for proper function since we just need to be in front of the classes old dex file
    // in the path.
    art::ObjPtr<art::mirror::Object> first_dex_file(
        element_dex_file_field->GetObject(current_element));
    if (!first_dex_file.IsNull()) {
      return first_dex_file;
    }
  }
  return nullptr;
}

}  // namespace openjdkjvmti
