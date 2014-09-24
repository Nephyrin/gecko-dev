/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2014 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef asmjs_AsmJSSignalHandlers_h
#define asmjs_AsmJSSignalHandlers_h

struct JSRuntime;

#ifdef _WIN64
#include "jstypes.h"
#endif

#ifdef XP_MACOSX
# include <mach/mach.h>
# include "jslock.h"
#endif

namespace js {

// Returns whether signal handlers for asm.js and for JitRuntime access
// violations have been installed.
bool
EnsureAsmJSSignalHandlersInstalled(JSRuntime *rt);

// Force any currently-executing asm.js code to call
// js::HandleExecutionInterrupt.
extern void
RequestInterruptForAsmJSCode(JSRuntime *rt, int interruptMode);

// The exception handling situation on Win64 is slightly goofy. There are three
// major phases of exception handling:
//
// - vectored exception handlers;
// - structured exception handling, used for e.g. C++ exceptions; and
// - vectored continue handlers.
//
// If any vectored exception handler indicates execution may continue, the
// structured exception handling phase is skipped.  If all vectored exception
// handlers indicate that the search for a proper exception handler should
// continue, then the structured exception handling mechanisms, which unwind
// the stack via platform-mandated unwind information, are invoked.  In both
// cases, the vectored continue handlers are invoked after the other phases.
//
// asm.js uses a vectored exception handler to catch access violations and
// resume execution if the access violations occurred within asm.js code.
//
// The Gecko crashreporter uses vectored continue handlers as a last-chance
// point at which to dump crash reports. However, note that, given the above
// description, the crashreporter's handler will execute even if asm.js's
// vectored exception handler fixes things up nicely.  Therefore, we need
// some method of communicating to the Gecko crashreporter (or any other
// JS engine client on Win64 that uses vectored continue handlers, really)
// that asm.js has successfully handled this exception and no other action
// is necessary.
//
// Please note: the client is responsible for resetting this value to false
// after examining it!
#ifdef _WIN64
extern JS_PUBLIC_DATA(bool) AsmJSHandledException;
#endif

// On OSX we are forced to use the lower-level Mach exception mechanism instead
// of Unix signals. Mach exceptions are not handled on the victim's stack but
// rather require an extra thread. For simplicity, we create one such thread
// per JSRuntime (upon the first use of asm.js in the JSRuntime). This thread
// and related resources are owned by AsmJSMachExceptionHandler which is owned
// by JSRuntime.
#ifdef XP_MACOSX
class AsmJSMachExceptionHandler
{
    bool installed_;
    PRThread *thread_;
    mach_port_t port_;

    void uninstall();

  public:
    AsmJSMachExceptionHandler();
    ~AsmJSMachExceptionHandler() { uninstall(); }
    mach_port_t port() const { return port_; }
    bool installed() const { return installed_; }
    bool install(JSRuntime *rt);
};
#endif

} // namespace js

#endif // asmjs_AsmJSSignalHandlers_h
