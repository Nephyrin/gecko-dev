/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

[Constructor(DOMString type, optional ErrorEventInit eventInitDict)]
interface ErrorEvent : Event
{
  readonly attribute DOMString message;
  readonly attribute DOMString filename;
  readonly attribute unsigned long lineno;
  readonly attribute unsigned long column;
  readonly attribute any error;
};

dictionary ErrorEventInit : EventInit
{
  DOMString message = "";
  DOMString filename = "";
  unsigned long lineno = 0;
  unsigned long column = 0;
  any error = null;
};
