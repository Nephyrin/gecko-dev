/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WindowHook.h"
#include "nsWindow.h"

namespace mozilla {
namespace widget {

nsresult
WindowHook::AddHook(UINT nMsg, Callback callback, void *context) {
  MessageData *data = LookupOrCreate(nMsg);

  if (!data)
    return NS_ERROR_OUT_OF_MEMORY;

  // Ensure we don't overwrite another hook
  NS_ENSURE_TRUE(nullptr == data->hook.cb, NS_ERROR_UNEXPECTED);

  data->hook = CallbackData(callback, context);

  return NS_OK;
}

nsresult
WindowHook::RemoveHook(UINT nMsg, Callback callback, void *context) {
  CallbackData cbdata(callback, context);
  MessageData *data = Lookup(nMsg);
  if (!data)
    return NS_ERROR_UNEXPECTED;
  if (data->hook != cbdata)
    return NS_ERROR_UNEXPECTED;
  data->hook = CallbackData();

  DeleteIfEmpty(data);
  return NS_OK;
}

nsresult
WindowHook::AddMonitor(UINT nMsg, Callback callback, void *context) {
  MessageData *data = LookupOrCreate(nMsg);
  return (data && data->monitors.AppendElement(CallbackData(callback, context)))
         ? NS_OK : NS_ERROR_OUT_OF_MEMORY;
}

nsresult
WindowHook::RemoveMonitor(UINT nMsg, Callback callback, void *context) {
  CallbackData cbdata(callback, context);
  MessageData *data = Lookup(nMsg);
  if (!data)
    return NS_ERROR_UNEXPECTED;
  CallbackDataArray::index_type idx = data->monitors.IndexOf(cbdata);
  if (idx == CallbackDataArray::NoIndex)
    return NS_ERROR_UNEXPECTED;
  data->monitors.RemoveElementAt(idx);
  DeleteIfEmpty(data);
  return NS_OK;
}

WindowHook::MessageData *
WindowHook::Lookup(UINT nMsg) {
  MessageDataArray::index_type idx;
  for (idx = 0; idx < mMessageData.Length(); idx++) {
    MessageData &data = mMessageData[idx];
    if (data.nMsg == nMsg)
      return &data;
  }
  return nullptr;
}

WindowHook::MessageData *
WindowHook::LookupOrCreate(UINT nMsg) {
  MessageData *data = Lookup(nMsg);
  if (!data) {
    data = mMessageData.AppendElement();

    if (!data)
      return nullptr;

    data->nMsg = nMsg;
  }
  return data;
}

void
WindowHook::DeleteIfEmpty(MessageData *data) {
  // Never remove a MessageData that has still a hook or monitor entries.
  if (data->hook || !data->monitors.IsEmpty())
    return;

  MessageDataArray::index_type idx;
  idx = data - mMessageData.Elements();
  NS_ASSERTION(idx >= 0 && idx < mMessageData.Length(), "Attempted to delete MessageData that doesn't belong to this array!");
  mMessageData.RemoveElementAt(idx);
}

bool
WindowHook::Notify(HWND hWnd, UINT nMsg, WPARAM wParam, LPARAM lParam,
                   LRESULT *aResult) {
  MessageData *data = Lookup(nMsg);
  if (!data)
    return false;

  PRUint32 length = data->monitors.Length();
  for (PRUint32 midx = 0; midx < length; midx++) {
    data->monitors[midx].Invoke(hWnd, nMsg, wParam, lParam, aResult);
  }

  return data->hook.Invoke(hWnd, nMsg, wParam, lParam, aResult);
}

bool
WindowHook::CallbackData::Invoke(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 LRESULT *aResult) {
  if (!cb)
    return false;
  return cb(context, hWnd, msg, wParam, lParam, aResult);
}
} // namespace widget
} // namespace mozilla

