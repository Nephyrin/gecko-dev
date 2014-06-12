/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AudioProcessingEvent.h"
#include "mozilla/dom/AudioProcessingEventBinding.h"
#include "AudioContext.h"

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED_3(AudioProcessingEvent, nsDOMEvent,
                                     mInputBuffer, mOutputBuffer, mNode)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(AudioProcessingEvent)
NS_INTERFACE_MAP_END_INHERITING(nsDOMEvent)

NS_IMPL_ADDREF_INHERITED(AudioProcessingEvent, nsDOMEvent)
NS_IMPL_RELEASE_INHERITED(AudioProcessingEvent, nsDOMEvent)

AudioProcessingEvent::AudioProcessingEvent(ScriptProcessorNode* aOwner,
                                           nsPresContext* aPresContext,
                                           WidgetEvent* aEvent)
  : nsDOMEvent(aOwner, aPresContext, aEvent)
  , mPlaybackTime(0.0)
  , mNode(aOwner)
{
  SetIsDOMBinding();
}

JSObject*
AudioProcessingEvent::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return AudioProcessingEventBinding::Wrap(aCx, aScope, this);
}

already_AddRefed<AudioBuffer>
AudioProcessingEvent::LazilyCreateBuffer(uint32_t aNumberOfChannels,
                                         ErrorResult& aRv)
{
  AutoPushJSContext cx(mNode->Context()->GetJSContext());

  nsRefPtr<AudioBuffer> buffer =
    AudioBuffer::Create(mNode->Context(), aNumberOfChannels,
                        mNode->BufferSize(),
                        mNode->Context()->SampleRate(), cx, aRv);
  MOZ_ASSERT(buffer || aRv.ErrorCode() == NS_ERROR_OUT_OF_MEMORY);
  return buffer.forget();
}

}
}

