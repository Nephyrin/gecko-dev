/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBWrapperCache.h"

#include "mozilla/HoldDropJSObjects.h"
#include "nsCOMPtr.h"
#include "nsIScriptGlobalObject.h"
#include "nsPIDOMWindow.h"

#ifdef DEBUG
#include "nsCycleCollector.h"
#endif

using namespace mozilla;
using namespace mozilla::dom::indexedDB;

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBWrapperCache)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBWrapperCache,
                                                  DOMEventTargetHelper)
  // Don't need NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS because
  // DOMEventTargetHelper does it for us.
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBWrapperCache,
                                                DOMEventTargetHelper)
  if (tmp->mScriptOwner) {
    tmp->mScriptOwner = nullptr;
    mozilla::DropJSObjects(tmp);
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(IDBWrapperCache,
                                               DOMEventTargetHelper)
  // Don't need NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER because
  // DOMEventTargetHelper does it for us.
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mScriptOwner)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBWrapperCache)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(IDBWrapperCache, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(IDBWrapperCache, DOMEventTargetHelper)

IDBWrapperCache::IDBWrapperCache(nsDOMEventTargetHelper* aOwner)
  : nsDOMEventTargetHelper(aOwner), mScriptOwner(nullptr)
{ }

IDBWrapperCache::IDBWrapperCache(nsPIDOMWindow* aOwner)
  : nsDOMEventTargetHelper(aOwner), mScriptOwner(nullptr)
{ }

IDBWrapperCache::~IDBWrapperCache()
{
  mScriptOwner = nullptr;
  ReleaseWrapper(this);
  mozilla::DropJSObjects(this);
}

void
IDBWrapperCache::SetScriptOwner(JSObject* aScriptOwner)
{
  MOZ_ASSERT(aScriptOwner);

  mScriptOwner = aScriptOwner;
  mozilla::HoldJSObjects(this);
}

JSObject*
IDBWrapperCache::GetParentObject()
{
  if (mScriptOwner) {
    return mScriptOwner;
  }

  // Do what nsEventTargetSH::PreCreate does.
  nsCOMPtr<nsIScriptGlobalObject> parent;
  nsDOMEventTargetHelper::GetParentObject(getter_AddRefs(parent));

  return parent ? parent->GetGlobalJSObject() : nullptr;
}

#ifdef DEBUG
void
IDBWrapperCache::AssertIsRooted() const
{
  MOZ_ASSERT(cyclecollector::IsJSHolder(const_cast<IDBWrapperCache*>(this)),
             "Why aren't we rooted?!");
}
#endif
