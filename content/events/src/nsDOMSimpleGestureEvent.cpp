/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Thomas K. Dyas <tdyas@zecador.org>.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "nsDOMSimpleGestureEvent.h"
#include "nsGUIEvent.h"
#include "nsContentUtils.h"


nsDOMSimpleGestureEvent::nsDOMSimpleGestureEvent(nsPresContext* aPresContext, nsSimpleGestureEvent* aEvent)
  : nsDOMUIEvent(aPresContext, aEvent ? aEvent : new nsSimpleGestureEvent(PR_FALSE, 0, nsnull, 0, 0.0))
{
  NS_ASSERTION(mEvent->eventStructType == NS_SIMPLE_GESTURE_EVENT, "event type mismatch");

  if (aEvent) {
    mEventIsInternal = PR_FALSE;
  } else {
    mEventIsInternal = PR_TRUE;
    mEvent->time = PR_Now();
  }
}

nsDOMSimpleGestureEvent::~nsDOMSimpleGestureEvent()
{
  if (mEventIsInternal) {
    delete static_cast<nsSimpleGestureEvent*>(mEvent);
    mEvent = nsnull;
  }
}

NS_IMPL_ADDREF_INHERITED(nsDOMSimpleGestureEvent, nsDOMUIEvent)
NS_IMPL_RELEASE_INHERITED(nsDOMSimpleGestureEvent, nsDOMUIEvent)

NS_INTERFACE_MAP_BEGIN(nsDOMSimpleGestureEvent)
  NS_INTERFACE_MAP_ENTRY(nsIDOMSimpleGestureEvent)
  NS_INTERFACE_MAP_ENTRY_CONTENT_CLASSINFO(SimpleGestureEvent)
NS_INTERFACE_MAP_END_INHERITING(nsDOMUIEvent)

/* readonly attribute unsigned long direction; */
NS_IMETHODIMP
nsDOMSimpleGestureEvent::GetDirection(PRUint32 *aDirection)
{
  NS_ENSURE_ARG_POINTER(aDirection);
  *aDirection = static_cast<nsSimpleGestureEvent*>(mEvent)->direction;
  return NS_OK;
}

/* readonly attribute float delta; */
NS_IMETHODIMP
nsDOMSimpleGestureEvent::GetDelta(PRFloat64 *aDelta)
{
  NS_ENSURE_ARG_POINTER(aDelta);
  *aDelta = static_cast<nsSimpleGestureEvent*>(mEvent)->delta;
  return NS_OK;
}

/* readonly attribute boolean altKey; */
NS_IMETHODIMP
nsDOMSimpleGestureEvent::GetAltKey(PRBool* aIsDown)
{
  NS_ENSURE_ARG_POINTER(aIsDown);
  *aIsDown = static_cast<nsInputEvent*>(mEvent)->isAlt;
  return NS_OK;
}

/* readonly attribute boolean ctrlKey; */
NS_IMETHODIMP
nsDOMSimpleGestureEvent::GetCtrlKey(PRBool* aIsDown)
{
  NS_ENSURE_ARG_POINTER(aIsDown);
  *aIsDown = static_cast<nsInputEvent*>(mEvent)->isControl;
  return NS_OK;
}

/* readonly attribute boolean shiftKey; */
NS_IMETHODIMP
nsDOMSimpleGestureEvent::GetShiftKey(PRBool* aIsDown)
{
  NS_ENSURE_ARG_POINTER(aIsDown);
  *aIsDown = static_cast<nsInputEvent*>(mEvent)->isShift;
  return NS_OK;
}

/* readonly attribute boolean metaKey; */
NS_IMETHODIMP
nsDOMSimpleGestureEvent::GetMetaKey(PRBool* aIsDown)
{
  NS_ENSURE_ARG_POINTER(aIsDown);
  *aIsDown = static_cast<nsInputEvent*>(mEvent)->isMeta;
  return NS_OK;
}


NS_IMETHODIMP
nsDOMSimpleGestureEvent::InitSimpleGestureEvent(const nsAString & typeArg, PRBool canBubbleArg, PRBool cancelableArg, nsIDOMAbstractView *viewArg, PRInt32 detailArg, PRUint32 directionArg, PRFloat64 deltaArg, PRBool altKeyArg, PRBool ctrlKeyArg, PRBool shiftKeyArg, PRBool metaKeyArg)
{
  nsresult rv = nsDOMUIEvent::InitUIEvent(typeArg, canBubbleArg, cancelableArg, viewArg, detailArg);
  NS_ENSURE_SUCCESS(rv, rv);

  nsSimpleGestureEvent* simpleGestureEvent = static_cast<nsSimpleGestureEvent*>(mEvent);
  simpleGestureEvent->direction = directionArg;
  simpleGestureEvent->delta = deltaArg;
  simpleGestureEvent->isAlt = altKeyArg;
  simpleGestureEvent->isControl = ctrlKeyArg;
  simpleGestureEvent->isShift = shiftKeyArg;
  simpleGestureEvent->isMeta = metaKeyArg;

  return NS_OK;
}

nsresult NS_NewDOMSimpleGestureEvent(nsIDOMEvent** aInstancePtrResult,
                                     nsPresContext* aPresContext,
                                     nsSimpleGestureEvent *aEvent)
{
  nsDOMSimpleGestureEvent *it = new nsDOMSimpleGestureEvent(aPresContext, aEvent);
  if (nsnull == it) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  return CallQueryInterface(it, aInstancePtrResult);
}
