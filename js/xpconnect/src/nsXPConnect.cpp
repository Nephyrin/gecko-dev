/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=8 sts=4 et sw=4 tw=99: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* High level class and public functions implementation. */

#include "mozilla/Assertions.h"
#include "mozilla/Base64.h"
#include "mozilla/Likely.h"

#include "xpcprivate.h"
#include "XPCWrapper.h"
#include "jsfriendapi.h"
#include "js/OldDebugAPI.h"
#include "nsJSEnvironment.h"
#include "nsThreadUtils.h"
#include "nsDOMJSUtils.h"

#include "WrapperFactory.h"
#include "AccessCheck.h"

#include "XPCQuickStubs.h"

#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/Exceptions.h"
#include "mozilla/dom/PromiseBinding.h"
#include "mozilla/dom/TextDecoderBinding.h"
#include "mozilla/dom/TextEncoderBinding.h"
#include "mozilla/dom/DOMErrorBinding.h"

#include "nsDOMMutationObserver.h"
#include "nsICycleCollectorListener.h"
#include "nsThread.h"
#include "mozilla/XPTInterfaceInfoManager.h"
#include "nsIObjectInputStream.h"
#include "nsIObjectOutputStream.h"
#include "nsScriptSecurityManager.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace xpc;
using namespace JS;

NS_IMPL_ISUPPORTS(nsXPConnect,
                  nsIXPConnect,
                  nsISupportsWeakReference,
                  nsIThreadObserver,
                  nsIJSRuntimeService)

nsXPConnect* nsXPConnect::gSelf = nullptr;
bool         nsXPConnect::gOnceAliveNowDead = false;
uint32_t     nsXPConnect::gReportAllJSExceptions = 0;

// Global cache of the default script security manager (QI'd to
// nsIScriptSecurityManager)
nsIScriptSecurityManager *nsXPConnect::gScriptSecurityManager = nullptr;

const char XPC_CONTEXT_STACK_CONTRACTID[] = "@mozilla.org/js/xpc/ContextStack;1";
const char XPC_RUNTIME_CONTRACTID[]       = "@mozilla.org/js/xpc/RuntimeService;1";
const char XPC_EXCEPTION_CONTRACTID[]     = "@mozilla.org/js/xpc/Exception;1";
const char XPC_CONSOLE_CONTRACTID[]       = "@mozilla.org/consoleservice;1";
const char XPC_SCRIPT_ERROR_CONTRACTID[]  = "@mozilla.org/scripterror;1";
const char XPC_ID_CONTRACTID[]            = "@mozilla.org/js/xpc/ID;1";
const char XPC_XPCONNECT_CONTRACTID[]     = "@mozilla.org/js/xpc/XPConnect;1";

/***************************************************************************/

nsXPConnect::nsXPConnect()
    :   mRuntime(nullptr),
        mShuttingDown(false),
        mEventDepth(0)
{
    mRuntime = XPCJSRuntime::newXPCJSRuntime(this);

    char* reportableEnv = PR_GetEnv("MOZ_REPORT_ALL_JS_EXCEPTIONS");
    if (reportableEnv && *reportableEnv)
        gReportAllJSExceptions = 1;
}

nsXPConnect::~nsXPConnect()
{
    mRuntime->DeleteSingletonScopes();
    mRuntime->DestroyJSContextStack();

    // In order to clean up everything properly, we need to GC twice: once now,
    // to clean anything that can go away on its own (like the Junk Scope, which
    // we unrooted above), and once after forcing a bunch of shutdown in
    // XPConnect, to clean the stuff we forcibly disconnected. The forced
    // shutdown code defaults to leaking in a number of situations, so we can't
    // get by with only the second GC. :-(
    JS_GC(mRuntime->Runtime());

    mShuttingDown = true;
    XPCWrappedNativeScope::SystemIsBeingShutDown();
    mRuntime->SystemIsBeingShutDown();

    // The above causes us to clean up a bunch of XPConnect data structures,
    // after which point we need to GC to clean everything up. We need to do
    // this before deleting the XPCJSRuntime, because doing so destroys the
    // maps that our finalize callback depends on.
    JS_GC(mRuntime->Runtime());

    gScriptSecurityManager = nullptr;

    // shutdown the logging system
    XPC_LOG_FINISH();

    delete mRuntime;

    gSelf = nullptr;
    gOnceAliveNowDead = true;
}

// static
void
nsXPConnect::InitStatics()
{
    gSelf = new nsXPConnect();
    gOnceAliveNowDead = false;
    if (!gSelf->mRuntime) {
        NS_RUNTIMEABORT("Couldn't create XPCJSRuntime.");
    }

    // Initial extra ref to keep the singleton alive
    // balanced by explicit call to ReleaseXPConnectSingleton()
    NS_ADDREF(gSelf);

    // Set XPConnect as the main thread observer.
    if (NS_FAILED(nsThread::SetMainThreadObserver(gSelf))) {
        MOZ_CRASH();
    }

    // Fire up the SSM.
    nsScriptSecurityManager::InitStatics();
    gScriptSecurityManager = nsScriptSecurityManager::GetScriptSecurityManager();

    // Initialize the SafeJSContext.
    gSelf->mRuntime->GetJSContextStack()->InitSafeJSContext();
}

nsXPConnect*
nsXPConnect::GetSingleton()
{
    nsXPConnect* xpc = nsXPConnect::XPConnect();
    NS_IF_ADDREF(xpc);
    return xpc;
}

// static
void
nsXPConnect::ReleaseXPConnectSingleton()
{
    nsXPConnect* xpc = gSelf;
    if (xpc) {
        nsThread::SetMainThreadObserver(nullptr);

        nsrefcnt cnt;
        NS_RELEASE2(xpc, cnt);
    }
}

// static
XPCJSRuntime*
nsXPConnect::GetRuntimeInstance()
{
    nsXPConnect* xpc = XPConnect();
    return xpc->GetRuntime();
}

// static
bool
nsXPConnect::IsISupportsDescendant(nsIInterfaceInfo* info)
{
    bool found = false;
    if (info)
        info->HasAncestor(&NS_GET_IID(nsISupports), &found);
    return found;
}

void
xpc::SystemErrorReporter(JSContext *cx, const char *message, JSErrorReport *rep)
{
    // It would be nice to assert !DescribeScriptedCaller here, to be sure
    // that there isn't any script running that could catch the exception. But
    // the JS engine invokes the error reporter directly if someone reports an
    // ErrorReport that it doesn't know how to turn into an exception. Arguably
    // it should just learn how to throw everything. But either way, if the
    // exception is ending here, it's not going to get propagated to a caller,
    // so it's up to us to make it known.

    nsresult rv;

    /* Use the console service to register the error. */
    nsCOMPtr<nsIConsoleService> consoleService =
        do_GetService(NS_CONSOLESERVICE_CONTRACTID);

    /*
     * Make an nsIScriptError, populate it with information from this
     * error, then log it with the console service.
     */
    nsCOMPtr<nsIScriptError> errorObject =
        do_CreateInstance(NS_SCRIPTERROR_CONTRACTID);

    if (consoleService && errorObject) {
        uint32_t column = rep->uctokenptr - rep->uclinebuf;

        const char16_t* ucmessage =
            static_cast<const char16_t*>(rep->ucmessage);
        const char16_t* uclinebuf =
            static_cast<const char16_t*>(rep->uclinebuf);

        rv = errorObject->Init(
              ucmessage ? nsDependentString(ucmessage) : EmptyString(),
              NS_ConvertASCIItoUTF16(rep->filename),
              uclinebuf ? nsDependentString(uclinebuf) : EmptyString(),
              rep->lineno, column, rep->flags,
              "system javascript");
        if (NS_SUCCEEDED(rv))
            consoleService->LogMessage(errorObject);
    }

    if (nsContentUtils::DOMWindowDumpEnabled()) {
        fprintf(stderr, "System JS : %s %s:%d - %s\n",
                JSREPORT_IS_WARNING(rep->flags) ? "WARNING" : "ERROR",
                rep->filename, rep->lineno,
                message ? message : "<no message>");
    }

}


/***************************************************************************/


nsresult
nsXPConnect::GetInfoForIID(const nsIID * aIID, nsIInterfaceInfo** info)
{
  return XPTInterfaceInfoManager::GetSingleton()->GetInfoForIID(aIID, info);
}

nsresult
nsXPConnect::GetInfoForName(const char * name, nsIInterfaceInfo** info)
{
  nsresult rv = XPTInterfaceInfoManager::GetSingleton()->GetInfoForName(name, info);
  return NS_FAILED(rv) ? NS_OK : NS_ERROR_NO_INTERFACE;
}

NS_IMETHODIMP
nsXPConnect::GarbageCollect(uint32_t reason)
{
    GetRuntime()->GarbageCollect(reason);
    return NS_OK;
}

bool
xpc_GCThingIsGrayCCThing(void *thing)
{
    return AddToCCKind(js::GCThingTraceKind(thing)) &&
           xpc_IsGrayGCThing(thing);
}

void
xpc_MarkInCCGeneration(nsISupports* aVariant, uint32_t aGeneration)
{
    nsCOMPtr<XPCVariant> variant = do_QueryInterface(aVariant);
    if (variant) {
        variant->SetCCGeneration(aGeneration);
        variant->GetJSVal(); // Unmarks gray JSObject.
        XPCVariant* weak = variant.get();
        variant = nullptr;
        if (weak->IsPurple()) {
          weak->RemovePurple();
        }
    }
}

void
xpc_TryUnmarkWrappedGrayObject(nsISupports* aWrappedJS)
{
    nsCOMPtr<nsIXPConnectWrappedJS> wjs = do_QueryInterface(aWrappedJS);
    if (wjs) {
        // Unmarks gray JSObject.
        static_cast<nsXPCWrappedJS*>(wjs.get())->GetJSObject();
    }
}

/***************************************************************************/
/***************************************************************************/
// nsIXPConnect interface methods...

template<typename T>
static inline T UnexpectedFailure(T rv)
{
    NS_ERROR("This is not supposed to fail!");
    return rv;
}

void
TraceXPCGlobal(JSTracer *trc, JSObject *obj)
{
    if (js::GetObjectClass(obj)->flags & JSCLASS_DOM_GLOBAL)
        mozilla::dom::TraceProtoAndIfaceCache(trc, obj);
}

namespace xpc {

JSObject*
CreateGlobalObject(JSContext *cx, const JSClass *clasp, nsIPrincipal *principal,
                   JS::CompartmentOptions& aOptions)
{
    MOZ_ASSERT(NS_IsMainThread(), "using a principal off the main thread?");
    MOZ_ASSERT(principal);

    MOZ_RELEASE_ASSERT(principal != nsContentUtils::GetNullSubjectPrincipal(),
                       "The null subject principal is getting inherited - fix that!");

    RootedObject global(cx,
                        JS_NewGlobalObject(cx, clasp, nsJSPrincipals::get(principal),
                                           JS::DontFireOnNewGlobalHook, aOptions));
    if (!global)
        return nullptr;
    JSAutoCompartment ac(cx, global);

    // The constructor automatically attaches the scope to the compartment private
    // of |global|.
    (void) new XPCWrappedNativeScope(cx, global);

#ifdef DEBUG
    // Verify that the right trace hook is called. Note that this doesn't
    // work right for wrapped globals, since the tracing situation there is
    // more complicated. Manual inspection shows that they do the right thing.
    if (!((const js::Class*)clasp)->ext.isWrappedNative)
    {
        VerifyTraceProtoAndIfaceCacheCalledTracer trc(JS_GetRuntime(cx));
        JS_TraceChildren(&trc, global, JSTRACE_OBJECT);
        MOZ_ASSERT(trc.ok, "Trace hook on global needs to call TraceXPCGlobal for XPConnect compartments.");
    }
#endif

    if (clasp->flags & JSCLASS_DOM_GLOBAL) {
        const char* className = clasp->name;
        AllocateProtoAndIfaceCache(global,
                                   (strcmp(className, "Window") == 0 ||
                                    strcmp(className, "ChromeWindow") == 0)
                                   ? ProtoAndIfaceCache::WindowLike
                                   : ProtoAndIfaceCache::NonWindowLike);
    }

    return global;
}

bool
InitGlobalObject(JSContext* aJSContext, JS::Handle<JSObject*> aGlobal, uint32_t aFlags)
{
    // Immediately enter the global's compartment, so that everything else we
    // create ends up there.
    JSAutoCompartment ac(aJSContext, aGlobal);
    if (!(aFlags & nsIXPConnect::OMIT_COMPONENTS_OBJECT)) {
        // XPCCallContext gives us an active request needed to save/restore.
        if (!CompartmentPrivate::Get(aGlobal)->scope->AttachComponentsObject(aJSContext) ||
            !XPCNativeWrapper::AttachNewConstructorObject(aJSContext, aGlobal)) {
            return UnexpectedFailure(false);
        }
    }

    if (ShouldDiscardSystemSource()) {
        nsIPrincipal *prin = GetObjectPrincipal(aGlobal);
        bool isSystem = nsContentUtils::IsSystemPrincipal(prin);
        if (!isSystem) {
            short status = prin->GetAppStatus();
            isSystem = status == nsIPrincipal::APP_STATUS_PRIVILEGED ||
                       status == nsIPrincipal::APP_STATUS_CERTIFIED;
        }
        JS::CompartmentOptionsRef(aGlobal).setDiscardSource(isSystem);
    }

    // Stuff coming through this path always ends up as a DOM global.
    MOZ_ASSERT(js::GetObjectClass(aGlobal)->flags & JSCLASS_DOM_GLOBAL);

    // Init WebIDL binding constructors wanted on all XPConnect globals.
    //
    // XXX Please do not add any additional classes here without the approval of
    //     the XPConnect module owner.
    if (!PromiseBinding::GetConstructorObject(aJSContext, aGlobal) ||
        !TextDecoderBinding::GetConstructorObject(aJSContext, aGlobal) ||
        !TextEncoderBinding::GetConstructorObject(aJSContext, aGlobal) ||
        !DOMErrorBinding::GetConstructorObject(aJSContext, aGlobal)) {
        return UnexpectedFailure(false);
    }

    if (!(aFlags & nsIXPConnect::DONT_FIRE_ONNEWGLOBALHOOK))
        JS_FireOnNewGlobalObject(aJSContext, aGlobal);

    return true;
}

} // namespace xpc

NS_IMETHODIMP
nsXPConnect::InitClassesWithNewWrappedGlobal(JSContext * aJSContext,
                                             nsISupports *aCOMObj,
                                             nsIPrincipal * aPrincipal,
                                             uint32_t aFlags,
                                             JS::CompartmentOptions& aOptions,
                                             nsIXPConnectJSObjectHolder **_retval)
{
    MOZ_ASSERT(aJSContext, "bad param");
    MOZ_ASSERT(aCOMObj, "bad param");
    MOZ_ASSERT(_retval, "bad param");

    // We pass null for the 'extra' pointer during global object creation, so
    // we need to have a principal.
    MOZ_ASSERT(aPrincipal);

    // Call into XPCWrappedNative to make a new global object, scope, and global
    // prototype.
    xpcObjectHelper helper(aCOMObj);
    MOZ_ASSERT(helper.GetScriptableFlags() & nsIXPCScriptable::IS_GLOBAL_OBJECT);
    nsRefPtr<XPCWrappedNative> wrappedGlobal;
    nsresult rv =
        XPCWrappedNative::WrapNewGlobal(helper, aPrincipal,
                                        aFlags & nsIXPConnect::INIT_JS_STANDARD_CLASSES,
                                        aOptions, getter_AddRefs(wrappedGlobal));
    NS_ENSURE_SUCCESS(rv, rv);

    // Grab a copy of the global and enter its compartment.
    RootedObject global(aJSContext, wrappedGlobal->GetFlatJSObject());
    MOZ_ASSERT(!js::GetObjectParent(global));

    if (!InitGlobalObject(aJSContext, global, aFlags))
        return UnexpectedFailure(NS_ERROR_FAILURE);

    wrappedGlobal.forget(_retval);
    return NS_OK;
}

static nsresult
NativeInterface2JSObject(HandleObject aScope,
                         nsISupports *aCOMObj,
                         nsWrapperCache *aCache,
                         const nsIID * aIID,
                         bool aAllowWrapping,
                         MutableHandleValue aVal,
                         nsIXPConnectJSObjectHolder **aHolder)
{
    AutoJSContext cx;
    JSAutoCompartment ac(cx, aScope);

    nsresult rv;
    xpcObjectHelper helper(aCOMObj, aCache);
    if (!XPCConvert::NativeInterface2JSObject(aVal, aHolder, helper, aIID,
                                              nullptr, aAllowWrapping, &rv))
        return rv;

    MOZ_ASSERT(aAllowWrapping || !xpc::WrapperFactory::IsXrayWrapper(&aVal.toObject()),
               "Shouldn't be returning a xray wrapper here");

    return NS_OK;
}

/* nsIXPConnectJSObjectHolder wrapNative (in JSContextPtr aJSContext, in JSObjectPtr aScope, in nsISupports aCOMObj, in nsIIDRef aIID); */
NS_IMETHODIMP
nsXPConnect::WrapNative(JSContext * aJSContext,
                        JSObject * aScopeArg,
                        nsISupports *aCOMObj,
                        const nsIID & aIID,
                        nsIXPConnectJSObjectHolder **aHolder)
{
    MOZ_ASSERT(aHolder, "bad param");
    MOZ_ASSERT(aJSContext, "bad param");
    MOZ_ASSERT(aScopeArg, "bad param");
    MOZ_ASSERT(aCOMObj, "bad param");

    RootedObject aScope(aJSContext, aScopeArg);
    RootedValue v(aJSContext);
    return NativeInterface2JSObject(aScope, aCOMObj, nullptr, &aIID,
                                    true, &v, aHolder);
}

/* void wrapNativeToJSVal (in JSContextPtr aJSContext, in JSObjectPtr aScope, in nsISupports aCOMObj, in nsIIDPtr aIID, out jsval aVal, out nsIXPConnectJSObjectHolder aHolder); */
NS_IMETHODIMP
nsXPConnect::WrapNativeToJSVal(JSContext *aJSContext,
                               JSObject *aScopeArg,
                               nsISupports *aCOMObj,
                               nsWrapperCache *aCache,
                               const nsIID *aIID,
                               bool aAllowWrapping,
                               MutableHandleValue aVal)
{
    MOZ_ASSERT(aJSContext, "bad param");
    MOZ_ASSERT(aScopeArg, "bad param");
    MOZ_ASSERT(aCOMObj, "bad param");

    RootedObject aScope(aJSContext, aScopeArg);
    return NativeInterface2JSObject(aScope, aCOMObj, aCache, aIID,
                                    aAllowWrapping, aVal, nullptr);
}

/* void wrapJS (in JSContextPtr aJSContext, in JSObjectPtr aJSObj, in nsIIDRef aIID, [iid_is (aIID), retval] out nsQIResult result); */
NS_IMETHODIMP
nsXPConnect::WrapJS(JSContext * aJSContext,
                    JSObject * aJSObjArg,
                    const nsIID & aIID,
                    void * *result)
{
    MOZ_ASSERT(aJSContext, "bad param");
    MOZ_ASSERT(aJSObjArg, "bad param");
    MOZ_ASSERT(result, "bad param");

    *result = nullptr;

    RootedObject aJSObj(aJSContext, aJSObjArg);
    JSAutoCompartment ac(aJSContext, aJSObj);

    nsresult rv = NS_ERROR_UNEXPECTED;
    if (!XPCConvert::JSObject2NativeInterface(result, aJSObj,
                                              &aIID, nullptr, &rv))
        return rv;
    return NS_OK;
}

NS_IMETHODIMP
nsXPConnect::JSValToVariant(JSContext *cx,
                            HandleValue aJSVal,
                            nsIVariant **aResult)
{
    NS_PRECONDITION(aResult, "bad param");

    nsRefPtr<XPCVariant> variant = XPCVariant::newVariant(cx, aJSVal);
    variant.forget(aResult);
    NS_ENSURE_TRUE(*aResult, NS_ERROR_OUT_OF_MEMORY);

    return NS_OK;
}

/* void wrapJSAggregatedToNative (in nsISupports aOuter, in JSContextPtr aJSContext, in JSObjectPtr aJSObj, in nsIIDRef aIID, [iid_is (aIID), retval] out nsQIResult result); */
NS_IMETHODIMP
nsXPConnect::WrapJSAggregatedToNative(nsISupports *aOuter,
                                      JSContext *aJSContext,
                                      JSObject *aJSObjArg,
                                      const nsIID &aIID,
                                      void **result)
{
    MOZ_ASSERT(aOuter, "bad param");
    MOZ_ASSERT(aJSContext, "bad param");
    MOZ_ASSERT(aJSObjArg, "bad param");
    MOZ_ASSERT(result, "bad param");

    *result = nullptr;

    RootedObject aJSObj(aJSContext, aJSObjArg);
    nsresult rv;
    if (!XPCConvert::JSObject2NativeInterface(result, aJSObj,
                                              &aIID, aOuter, &rv))
        return rv;
    return NS_OK;
}

/* nsIXPConnectWrappedNative getWrappedNativeOfJSObject (in JSContextPtr aJSContext, in JSObjectPtr aJSObj); */
NS_IMETHODIMP
nsXPConnect::GetWrappedNativeOfJSObject(JSContext * aJSContext,
                                        JSObject * aJSObjArg,
                                        nsIXPConnectWrappedNative **_retval)
{
    MOZ_ASSERT(aJSContext, "bad param");
    MOZ_ASSERT(aJSObjArg, "bad param");
    MOZ_ASSERT(_retval, "bad param");

    RootedObject aJSObj(aJSContext, aJSObjArg);
    aJSObj = js::CheckedUnwrap(aJSObj, /* stopAtOuter = */ false);
    if (!aJSObj || !IS_WN_REFLECTOR(aJSObj)) {
        *_retval = nullptr;
        return NS_ERROR_FAILURE;
    }

    nsRefPtr<XPCWrappedNative> temp = XPCWrappedNative::Get(aJSObj);
    temp.forget(_retval);
    return NS_OK;
}

nsISupports*
xpc::UnwrapReflectorToISupports(JSObject *reflector)
{
    // Unwrap security wrappers, if allowed.
    reflector = js::CheckedUnwrap(reflector, /* stopAtOuter = */ false);
    if (!reflector)
        return nullptr;

    // Try XPCWrappedNatives.
    if (IS_WN_REFLECTOR(reflector)) {
        XPCWrappedNative *wn = XPCWrappedNative::Get(reflector);
        if (!wn)
            return nullptr;
        return wn->Native();
    }

    // Try DOM objects.
    nsCOMPtr<nsISupports> canonical =
        do_QueryInterface(mozilla::dom::UnwrapDOMObjectToISupports(reflector));
    return canonical;
}

/* nsISupports getNativeOfWrapper(in JSContextPtr aJSContext, in JSObjectPtr  aJSObj); */
NS_IMETHODIMP_(nsISupports*)
nsXPConnect::GetNativeOfWrapper(JSContext *aJSContext,
                                JSObject *aJSObj)
{
    return UnwrapReflectorToISupports(aJSObj);
}

/* nsIXPConnectWrappedNative getWrappedNativeOfNativeObject (in JSContextPtr aJSContext, in JSObjectPtr aScope, in nsISupports aCOMObj, in nsIIDRef aIID); */
NS_IMETHODIMP
nsXPConnect::GetWrappedNativeOfNativeObject(JSContext * aJSContext,
                                            JSObject * aScopeArg,
                                            nsISupports *aCOMObj,
                                            const nsIID & aIID,
                                            nsIXPConnectWrappedNative **_retval)
{
    MOZ_ASSERT(aJSContext, "bad param");
    MOZ_ASSERT(aScopeArg, "bad param");
    MOZ_ASSERT(aCOMObj, "bad param");
    MOZ_ASSERT(_retval, "bad param");

    *_retval = nullptr;

    RootedObject aScope(aJSContext, aScopeArg);

    XPCWrappedNativeScope* scope = ObjectScope(aScope);
    if (!scope)
        return UnexpectedFailure(NS_ERROR_FAILURE);

    AutoMarkingNativeInterfacePtr iface(aJSContext);
    iface = XPCNativeInterface::GetNewOrUsed(&aIID);
    if (!iface)
        return NS_ERROR_FAILURE;

    XPCWrappedNative* wrapper;

    nsresult rv = XPCWrappedNative::GetUsedOnly(aCOMObj, scope, iface, &wrapper);
    if (NS_FAILED(rv))
        return NS_ERROR_FAILURE;
    *_retval = static_cast<nsIXPConnectWrappedNative*>(wrapper);
    return NS_OK;
}

/* void reparentWrappedNativeIfFound (in JSContextPtr aJSContext,
 *                                    in JSObjectPtr aScope,
 *                                    in JSObjectPtr aNewParent,
 *                                    in nsISupports aCOMObj); */
NS_IMETHODIMP
nsXPConnect::ReparentWrappedNativeIfFound(JSContext * aJSContext,
                                          JSObject * aScopeArg,
                                          JSObject * aNewParentArg,
                                          nsISupports *aCOMObj)
{
    RootedObject aScope(aJSContext, aScopeArg);
    RootedObject aNewParent(aJSContext, aNewParentArg);

    XPCWrappedNativeScope* scope = ObjectScope(aScope);
    XPCWrappedNativeScope* scope2 = ObjectScope(aNewParent);
    if (!scope || !scope2)
        return UnexpectedFailure(NS_ERROR_FAILURE);

    RootedObject newParent(aJSContext, aNewParent);
    return XPCWrappedNative::
        ReparentWrapperIfFound(scope, scope2, newParent, aCOMObj);
}

static PLDHashOperator
MoveableWrapperFinder(PLDHashTable *table, PLDHashEntryHdr *hdr,
                      uint32_t number, void *arg)
{
    nsTArray<nsRefPtr<XPCWrappedNative> > *array =
        static_cast<nsTArray<nsRefPtr<XPCWrappedNative> > *>(arg);
    XPCWrappedNative *wn = ((Native2WrappedNativeMap::Entry*)hdr)->value;

    // If a wrapper is expired, then there are no references to it from JS, so
    // we don't have to move it.
    if (!wn->IsWrapperExpired())
        array->AppendElement(wn);
    return PL_DHASH_NEXT;
}

/* void rescueOrphansInScope(in JSContextPtr aJSContext, in JSObjectPtr  aScope); */
NS_IMETHODIMP
nsXPConnect::RescueOrphansInScope(JSContext *aJSContext, JSObject *aScopeArg)
{
    RootedObject aScope(aJSContext, aScopeArg);

    XPCWrappedNativeScope *scope = ObjectScope(aScope);
    if (!scope)
        return UnexpectedFailure(NS_ERROR_FAILURE);

    // First, look through the old scope and find all of the wrappers that we
    // might need to rescue.
    nsTArray<nsRefPtr<XPCWrappedNative> > wrappersToMove;

    Native2WrappedNativeMap *map = scope->GetWrappedNativeMap();
    wrappersToMove.SetCapacity(map->Count());
    map->Enumerate(MoveableWrapperFinder, &wrappersToMove);

    // Now that we have the wrappers, reparent them to the new scope.
    for (uint32_t i = 0, stop = wrappersToMove.Length(); i < stop; ++i) {
        nsresult rv = wrappersToMove[i]->RescueOrphans();
        NS_ENSURE_SUCCESS(rv, rv);
    }

    return NS_OK;
}

/* nsIStackFrame createStackFrameLocation (in uint32_t aLanguage, in string aFilename, in string aFunctionName, in int32_t aLineNumber, in nsIStackFrame aCaller); */
NS_IMETHODIMP
nsXPConnect::CreateStackFrameLocation(uint32_t aLanguage,
                                      const char *aFilename,
                                      const char *aFunctionName,
                                      int32_t aLineNumber,
                                      nsIStackFrame *aCaller,
                                      nsIStackFrame **_retval)
{
    MOZ_ASSERT(_retval, "bad param");

    nsCOMPtr<nsIStackFrame> stackFrame =
        exceptions::CreateStackFrameLocation(aLanguage,
                                             aFilename,
                                             aFunctionName,
                                             aLineNumber,
                                             aCaller);
    stackFrame.forget(_retval);
    return NS_OK;
}

/* readonly attribute nsIStackFrame CurrentJSStack; */
NS_IMETHODIMP
nsXPConnect::GetCurrentJSStack(nsIStackFrame * *aCurrentJSStack)
{
    MOZ_ASSERT(aCurrentJSStack, "bad param");

    nsCOMPtr<nsIStackFrame> currentStack = dom::GetCurrentJSStack();
    currentStack.forget(aCurrentJSStack);

    return NS_OK;
}

/* readonly attribute nsIXPCNativeCallContext CurrentNativeCallContext; */
NS_IMETHODIMP
nsXPConnect::GetCurrentNativeCallContext(nsAXPCNativeCallContext * *aCurrentNativeCallContext)
{
    MOZ_ASSERT(aCurrentNativeCallContext, "bad param");

    *aCurrentNativeCallContext = XPCJSRuntime::Get()->GetCallContext();
    return NS_OK;
}

/* void setFunctionThisTranslator (in nsIIDRef aIID, in nsIXPCFunctionThisTranslator aTranslator); */
NS_IMETHODIMP
nsXPConnect::SetFunctionThisTranslator(const nsIID & aIID,
                                       nsIXPCFunctionThisTranslator *aTranslator)
{
    XPCJSRuntime* rt = GetRuntime();
    IID2ThisTranslatorMap* map = rt->GetThisTranslatorMap();
    map->Add(aIID, aTranslator);
    return NS_OK;
}

NS_IMETHODIMP
nsXPConnect::CreateSandbox(JSContext *cx, nsIPrincipal *principal,
                           nsIXPConnectJSObjectHolder **_retval)
{
    *_retval = nullptr;

    RootedValue rval(cx);
    SandboxOptions options;
    nsresult rv = CreateSandboxObject(cx, &rval, principal, options);
    MOZ_ASSERT(NS_FAILED(rv) || !rval.isPrimitive(),
               "Bad return value from xpc_CreateSandboxObject()!");

    if (NS_SUCCEEDED(rv) && !rval.isPrimitive()) {
        *_retval = XPCJSObjectHolder::newHolder(rval.toObjectOrNull());
        NS_ENSURE_TRUE(*_retval, NS_ERROR_OUT_OF_MEMORY);

        NS_ADDREF(*_retval);
    }

    return rv;
}

NS_IMETHODIMP
nsXPConnect::EvalInSandboxObject(const nsAString& source, const char *filename,
                                 JSContext *cx, JSObject *sandboxArg,
                                 MutableHandleValue rval)
{
    if (!sandboxArg)
        return NS_ERROR_INVALID_ARG;

    RootedObject sandbox(cx, sandboxArg);
    nsCString filenameStr;
    if (filename) {
        filenameStr.Assign(filename);
    } else {
        filenameStr = NS_LITERAL_CSTRING("x-bogus://XPConnect/Sandbox");
    }
    return EvalInSandbox(cx, sandbox, source, filenameStr, 1,
                         JSVERSION_DEFAULT, rval);
}

/* nsIXPConnectJSObjectHolder getWrappedNativePrototype (in JSContextPtr aJSContext, in JSObjectPtr aScope, in nsIClassInfo aClassInfo); */
NS_IMETHODIMP
nsXPConnect::GetWrappedNativePrototype(JSContext * aJSContext,
                                       JSObject * aScopeArg,
                                       nsIClassInfo *aClassInfo,
                                       nsIXPConnectJSObjectHolder **_retval)
{
    RootedObject aScope(aJSContext, aScopeArg);
    JSAutoCompartment ac(aJSContext, aScope);

    XPCWrappedNativeScope* scope = ObjectScope(aScope);
    if (!scope)
        return UnexpectedFailure(NS_ERROR_FAILURE);

    XPCNativeScriptableCreateInfo sciProto;
    XPCWrappedNative::GatherProtoScriptableCreateInfo(aClassInfo, sciProto);

    AutoMarkingWrappedNativeProtoPtr proto(aJSContext);
    proto = XPCWrappedNativeProto::GetNewOrUsed(scope, aClassInfo, &sciProto);
    if (!proto)
        return UnexpectedFailure(NS_ERROR_FAILURE);

    nsIXPConnectJSObjectHolder* holder;
    *_retval = holder = XPCJSObjectHolder::newHolder(proto->GetJSProtoObject());
    if (!holder)
        return UnexpectedFailure(NS_ERROR_FAILURE);

    NS_ADDREF(holder);
    return NS_OK;
}

/* void debugDump (in short depth); */
NS_IMETHODIMP
nsXPConnect::DebugDump(int16_t depth)
{
#ifdef DEBUG
    depth-- ;
    XPC_LOG_ALWAYS(("nsXPConnect @ %x with mRefCnt = %d", this, mRefCnt.get()));
    XPC_LOG_INDENT();
        XPC_LOG_ALWAYS(("gSelf @ %x", gSelf));
        XPC_LOG_ALWAYS(("gOnceAliveNowDead is %d", (int)gOnceAliveNowDead));
        if (mRuntime) {
            if (depth)
                mRuntime->DebugDump(depth);
            else
                XPC_LOG_ALWAYS(("XPCJSRuntime @ %x", mRuntime));
        } else
            XPC_LOG_ALWAYS(("mRuntime is null"));
        XPCWrappedNativeScope::DebugDumpAllScopes(depth);
    XPC_LOG_OUTDENT();
#endif
    return NS_OK;
}

/* void debugDumpObject (in nsISupports aCOMObj, in short depth); */
NS_IMETHODIMP
nsXPConnect::DebugDumpObject(nsISupports *p, int16_t depth)
{
#ifdef DEBUG
    if (!depth)
        return NS_OK;
    if (!p) {
        XPC_LOG_ALWAYS(("*** Cound not dump object with NULL address"));
        return NS_OK;
    }

    nsIXPConnect* xpc;
    nsIXPCWrappedJSClass* wjsc;
    nsIXPConnectWrappedNative* wn;
    nsIXPConnectWrappedJS* wjs;

    if (NS_SUCCEEDED(p->QueryInterface(NS_GET_IID(nsIXPConnect),
                                       (void**)&xpc))) {
        XPC_LOG_ALWAYS(("Dumping a nsIXPConnect..."));
        xpc->DebugDump(depth);
        NS_RELEASE(xpc);
    } else if (NS_SUCCEEDED(p->QueryInterface(NS_GET_IID(nsIXPCWrappedJSClass),
                                              (void**)&wjsc))) {
        XPC_LOG_ALWAYS(("Dumping a nsIXPCWrappedJSClass..."));
        wjsc->DebugDump(depth);
        NS_RELEASE(wjsc);
    } else if (NS_SUCCEEDED(p->QueryInterface(NS_GET_IID(nsIXPConnectWrappedNative),
                                              (void**)&wn))) {
        XPC_LOG_ALWAYS(("Dumping a nsIXPConnectWrappedNative..."));
        wn->DebugDump(depth);
        NS_RELEASE(wn);
    } else if (NS_SUCCEEDED(p->QueryInterface(NS_GET_IID(nsIXPConnectWrappedJS),
                                              (void**)&wjs))) {
        XPC_LOG_ALWAYS(("Dumping a nsIXPConnectWrappedJS..."));
        wjs->DebugDump(depth);
        NS_RELEASE(wjs);
    } else
        XPC_LOG_ALWAYS(("*** Could not dump the nsISupports @ %x", p));
#endif
    return NS_OK;
}

/* void debugDumpJSStack (in bool showArgs, in bool showLocals, in bool showThisProps); */
NS_IMETHODIMP
nsXPConnect::DebugDumpJSStack(bool showArgs,
                              bool showLocals,
                              bool showThisProps)
{
    JSContext* cx = GetCurrentJSContext();
    if (!cx)
        printf("there is no JSContext on the nsIThreadJSContextStack!\n");
    else
        xpc_DumpJSStack(cx, showArgs, showLocals, showThisProps);

    return NS_OK;
}

char*
nsXPConnect::DebugPrintJSStack(bool showArgs,
                               bool showLocals,
                               bool showThisProps)
{
    JSContext* cx = GetCurrentJSContext();
    if (!cx)
        printf("there is no JSContext on the nsIThreadJSContextStack!\n");
    else
        return xpc_PrintJSStack(cx, showArgs, showLocals, showThisProps);

    return nullptr;
}

/* void debugDumpEvalInJSStackFrame (in uint32_t aFrameNumber, in string aSourceText); */
NS_IMETHODIMP
nsXPConnect::DebugDumpEvalInJSStackFrame(uint32_t aFrameNumber, const char *aSourceText)
{
    JSContext* cx = GetCurrentJSContext();
    if (!cx)
        printf("there is no JSContext on the nsIThreadJSContextStack!\n");
    else
        xpc_DumpEvalInJSStackFrame(cx, aFrameNumber, aSourceText);

    return NS_OK;
}

/* jsval variantToJS (in JSContextPtr ctx, in JSObjectPtr scope, in nsIVariant value); */
NS_IMETHODIMP
nsXPConnect::VariantToJS(JSContext* ctx, JSObject* scopeArg, nsIVariant* value,
                         MutableHandleValue _retval)
{
    NS_PRECONDITION(ctx, "bad param");
    NS_PRECONDITION(scopeArg, "bad param");
    NS_PRECONDITION(value, "bad param");

    RootedObject scope(ctx, scopeArg);
    MOZ_ASSERT(js::IsObjectInContextCompartment(scope, ctx));

    nsresult rv = NS_OK;
    if (!XPCVariant::VariantDataToJS(value, &rv, _retval)) {
        if (NS_FAILED(rv))
            return rv;

        return NS_ERROR_FAILURE;
    }
    return NS_OK;
}

/* nsIVariant JSToVariant (in JSContextPtr ctx, in jsval value); */
NS_IMETHODIMP
nsXPConnect::JSToVariant(JSContext* ctx, HandleValue value, nsIVariant** _retval)
{
    NS_PRECONDITION(ctx, "bad param");
    NS_PRECONDITION(_retval, "bad param");

    nsRefPtr<XPCVariant> variant = XPCVariant::newVariant(ctx, value);
    variant.forget(_retval);
    if (!(*_retval))
        return NS_ERROR_FAILURE;

    return NS_OK;
}

NS_IMETHODIMP
nsXPConnect::OnProcessNextEvent(nsIThreadInternal *aThread, bool aMayWait,
                                uint32_t aRecursionDepth)
{
    // Record this event.
    mEventDepth++;

    // Push a null JSContext so that we don't see any script during
    // event processing.
    MOZ_ASSERT(NS_IsMainThread());
    bool ok = PushJSContextNoScriptContext(nullptr);
    NS_ENSURE_TRUE(ok, NS_ERROR_FAILURE);
    return NS_OK;
}

NS_IMETHODIMP
nsXPConnect::AfterProcessNextEvent(nsIThreadInternal *aThread,
                                   uint32_t aRecursionDepth,
                                   bool aEventWasProcessed)
{
    // Watch out for unpaired events during observer registration.
    if (MOZ_UNLIKELY(mEventDepth == 0))
        return NS_OK;
    mEventDepth--;

    // Now that we're back to the event loop, reset the slow script checkpoint.
    mRuntime->OnAfterProcessNextEvent();

    // Call cycle collector occasionally.
    MOZ_ASSERT(NS_IsMainThread());
    nsJSContext::MaybePokeCC();
    nsDOMMutationObserver::HandleMutations();

    PopJSContextNoScriptContext();

    return NS_OK;
}

NS_IMETHODIMP
nsXPConnect::OnDispatchedEvent(nsIThreadInternal* aThread)
{
    NS_NOTREACHED("Why tell us?");
    return NS_ERROR_UNEXPECTED;
}

NS_IMETHODIMP
nsXPConnect::SetReportAllJSExceptions(bool newval)
{
    // Ignore if the environment variable was set.
    if (gReportAllJSExceptions != 1)
        gReportAllJSExceptions = newval ? 2 : 0;

    return NS_OK;
}

/* attribute JSRuntime runtime; */
NS_IMETHODIMP
nsXPConnect::GetRuntime(JSRuntime **runtime)
{
    if (!runtime)
        return NS_ERROR_NULL_POINTER;

    JSRuntime *rt = GetRuntime()->Runtime();
    JS_AbortIfWrongThread(rt);
    *runtime = rt;
    return NS_OK;
}

/* [noscript, notxpcom] void registerGCCallback(in xpcGCCallback func); */
NS_IMETHODIMP_(void)
nsXPConnect::RegisterGCCallback(xpcGCCallback func)
{
    mRuntime->AddGCCallback(func);
}

/* [noscript, notxpcom] void unregisterGCCallback(in xpcGCCallback func); */
NS_IMETHODIMP_(void)
nsXPConnect::UnregisterGCCallback(xpcGCCallback func)
{
    mRuntime->RemoveGCCallback(func);
}

/* [noscript, notxpcom] void registerContextCallback(in xpcContextCallback func); */
NS_IMETHODIMP_(void)
nsXPConnect::RegisterContextCallback(xpcContextCallback func)
{
    mRuntime->AddContextCallback(func);
}

/* [noscript, notxpcom] void unregisterContextCallback(in xpcContextCallback func); */
NS_IMETHODIMP_(void)
nsXPConnect::UnregisterContextCallback(xpcContextCallback func)
{
    mRuntime->RemoveContextCallback(func);
}

/* virtual */
JSContext*
nsXPConnect::GetCurrentJSContext()
{
    return GetRuntime()->GetJSContextStack()->Peek();
}

/* virtual */
JSContext*
nsXPConnect::GetSafeJSContext()
{
    return GetRuntime()->GetJSContextStack()->GetSafeJSContext();
}

namespace xpc {

bool
PushJSContextNoScriptContext(JSContext *aCx)
{
    MOZ_ASSERT_IF(aCx, !GetScriptContextFromJSContext(aCx));
    return XPCJSRuntime::Get()->GetJSContextStack()->Push(aCx);
}

void
PopJSContextNoScriptContext()
{
    XPCJSRuntime::Get()->GetJSContextStack()->Pop();
}

} // namespace xpc

nsIPrincipal*
nsXPConnect::GetPrincipal(JSObject* obj, bool allowShortCircuit) const
{
    MOZ_ASSERT(IS_WN_REFLECTOR(obj), "What kind of wrapper is this?");

    XPCWrappedNative *xpcWrapper = XPCWrappedNative::Get(obj);
    if (xpcWrapper) {
        if (allowShortCircuit) {
            nsIPrincipal *result = xpcWrapper->GetObjectPrincipal();
            if (result) {
                return result;
            }
        }

        // If not, check if it points to an nsIScriptObjectPrincipal
        nsCOMPtr<nsIScriptObjectPrincipal> objPrin =
            do_QueryInterface(xpcWrapper->Native());
        if (objPrin) {
            nsIPrincipal *result = objPrin->GetPrincipal();
            if (result) {
                return result;
            }
        }
    }

    return nullptr;
}

NS_IMETHODIMP
nsXPConnect::HoldObject(JSContext *aJSContext, JSObject *aObjectArg,
                        nsIXPConnectJSObjectHolder **aHolder)
{
    RootedObject aObject(aJSContext, aObjectArg);
    XPCJSObjectHolder* objHolder = XPCJSObjectHolder::newHolder(aObject);
    if (!objHolder)
        return NS_ERROR_OUT_OF_MEMORY;

    NS_ADDREF(*aHolder = objHolder);
    return NS_OK;
}

namespace xpc {

bool
Base64Encode(JSContext *cx, HandleValue val, MutableHandleValue out)
{
    MOZ_ASSERT(cx);

    JS::RootedValue root(cx, val);
    xpc_qsACString encodedString(cx, root, &root, false,
                                 xpc_qsACString::eStringify,
                                 xpc_qsACString::eStringify);
    if (!encodedString.IsValid())
        return false;

    nsAutoCString result;
    if (NS_FAILED(mozilla::Base64Encode(encodedString, result))) {
        JS_ReportError(cx, "Failed to encode base64 data!");
        return false;
    }

    JSString *str = JS_NewStringCopyN(cx, result.get(), result.Length());
    if (!str)
        return false;

    out.setString(str);
    return true;
}

bool
Base64Decode(JSContext *cx, HandleValue val, MutableHandleValue out)
{
    MOZ_ASSERT(cx);

    JS::RootedValue root(cx, val);
    xpc_qsACString encodedString(cx, root, &root, false,
                                 xpc_qsACString::eStringify,
                                 xpc_qsACString::eStringify);
    if (!encodedString.IsValid())
        return false;

    nsAutoCString result;
    if (NS_FAILED(mozilla::Base64Decode(encodedString, result))) {
        JS_ReportError(cx, "Failed to decode base64 string!");
        return false;
    }

    JSString *str = JS_NewStringCopyN(cx, result.get(), result.Length());
    if (!str)
        return false;

    out.setString(str);
    return true;
}

void
SetLocationForGlobal(JSObject *global, const nsACString& location)
{
    MOZ_ASSERT(global);
    CompartmentPrivate::Get(global)->SetLocation(location);
}

void
SetLocationForGlobal(JSObject *global, nsIURI *locationURI)
{
    MOZ_ASSERT(global);
    CompartmentPrivate::Get(global)->SetLocationURI(locationURI);
}

} // namespace xpc

NS_IMETHODIMP
nsXPConnect::NotifyDidPaint()
{
    JS::NotifyDidPaint(GetRuntime()->Runtime());
    return NS_OK;
}

// Note - We used to have HAS_PRINCIPALS_FLAG = 1 here, so reusing that flag
// will require bumping the XDR version number.
static const uint8_t HAS_ORIGIN_PRINCIPALS_FLAG        = 2;

static nsresult
WriteScriptOrFunction(nsIObjectOutputStream *stream, JSContext *cx,
                      JSScript *scriptArg, HandleObject functionObj)
{
    // Exactly one of script or functionObj must be given
    MOZ_ASSERT(!scriptArg != !functionObj);

    RootedScript script(cx, scriptArg);
    if (!script) {
        RootedFunction fun(cx, JS_GetObjectFunction(functionObj));
        script.set(JS_GetFunctionScript(cx, fun));
    }

    nsIPrincipal *principal =
        nsJSPrincipals::get(JS_GetScriptPrincipals(script));
    nsIPrincipal *originPrincipal =
        nsJSPrincipals::get(JS_GetScriptOriginPrincipals(script));

    uint8_t flags = 0;

    // Optimize for the common case when originPrincipals == principals. As
    // originPrincipals is set to principals when the former is null we can
    // simply skip the originPrincipals when they are the same as principals.
    if (originPrincipal && originPrincipal != principal)
        flags |= HAS_ORIGIN_PRINCIPALS_FLAG;

    nsresult rv = stream->Write8(flags);
    if (NS_FAILED(rv))
        return rv;

    if (flags & HAS_ORIGIN_PRINCIPALS_FLAG) {
        rv = stream->WriteObject(originPrincipal, true);
        if (NS_FAILED(rv))
            return rv;
    }

    uint32_t size;
    void* data;
    {
        if (functionObj)
            data = JS_EncodeInterpretedFunction(cx, functionObj, &size);
        else
            data = JS_EncodeScript(cx, script, &size);
    }

    if (!data)
        return NS_ERROR_OUT_OF_MEMORY;
    MOZ_ASSERT(size);
    rv = stream->Write32(size);
    if (NS_SUCCEEDED(rv))
        rv = stream->WriteBytes(static_cast<char *>(data), size);
    js_free(data);

    return rv;
}

static nsresult
ReadScriptOrFunction(nsIObjectInputStream *stream, JSContext *cx,
                     JSScript **scriptp, JSObject **functionObjp)
{
    // Exactly one of script or functionObj must be given
    MOZ_ASSERT(!scriptp != !functionObjp);

    uint8_t flags;
    nsresult rv = stream->Read8(&flags);
    if (NS_FAILED(rv))
        return rv;

    nsJSPrincipals* originPrincipal = nullptr;
    nsCOMPtr<nsIPrincipal> readOriginPrincipal;
    if (flags & HAS_ORIGIN_PRINCIPALS_FLAG) {
        nsCOMPtr<nsISupports> supports;
        rv = stream->ReadObject(true, getter_AddRefs(supports));
        if (NS_FAILED(rv))
            return rv;
        readOriginPrincipal = do_QueryInterface(supports);
        originPrincipal = nsJSPrincipals::get(readOriginPrincipal);
    }

    uint32_t size;
    rv = stream->Read32(&size);
    if (NS_FAILED(rv))
        return rv;

    char* data;
    rv = stream->ReadBytes(size, &data);
    if (NS_FAILED(rv))
        return rv;

    {
        if (scriptp) {
            JSScript *script = JS_DecodeScript(cx, data, size, originPrincipal);
            if (!script)
                rv = NS_ERROR_OUT_OF_MEMORY;
            else
                *scriptp = script;
        } else {
            JSObject *funobj = JS_DecodeInterpretedFunction(cx, data, size,
                                                            originPrincipal);
            if (!funobj)
                rv = NS_ERROR_OUT_OF_MEMORY;
            else
                *functionObjp = funobj;
        }
    }

    nsMemory::Free(data);
    return rv;
}

NS_IMETHODIMP
nsXPConnect::WriteScript(nsIObjectOutputStream *stream, JSContext *cx, JSScript *script)
{
    return WriteScriptOrFunction(stream, cx, script, NullPtr());
}

NS_IMETHODIMP
nsXPConnect::ReadScript(nsIObjectInputStream *stream, JSContext *cx, JSScript **scriptp)
{
    return ReadScriptOrFunction(stream, cx, scriptp, nullptr);
}

NS_IMETHODIMP
nsXPConnect::WriteFunction(nsIObjectOutputStream *stream, JSContext *cx, JSObject *functionObjArg)
{
    RootedObject functionObj(cx, functionObjArg);
    return WriteScriptOrFunction(stream, cx, nullptr, functionObj);
}

NS_IMETHODIMP
nsXPConnect::ReadFunction(nsIObjectInputStream *stream, JSContext *cx, JSObject **functionObjp)
{
    return ReadScriptOrFunction(stream, cx, nullptr, functionObjp);
}

NS_IMETHODIMP
nsXPConnect::MarkErrorUnreported(JSContext *cx)
{
    XPCContext *xpcc = XPCContext::GetXPCContext(cx);
    xpcc->MarkErrorUnreported();
    return NS_OK;
}

/* These are here to be callable from a debugger */
extern "C" {
JS_EXPORT_API(void) DumpJSStack()
{
    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
    if (NS_SUCCEEDED(rv) && xpc)
        xpc->DebugDumpJSStack(true, true, false);
    else
        printf("failed to get XPConnect service!\n");
}

JS_EXPORT_API(char*) PrintJSStack()
{
    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
    return (NS_SUCCEEDED(rv) && xpc) ?
        xpc->DebugPrintJSStack(true, true, false) :
        nullptr;
}

JS_EXPORT_API(void) DumpJSEval(uint32_t frameno, const char* text)
{
    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
    if (NS_SUCCEEDED(rv) && xpc)
        xpc->DebugDumpEvalInJSStackFrame(frameno, text);
    else
        printf("failed to get XPConnect service!\n");
}

JS_EXPORT_API(void) DumpCompleteHeap()
{
    nsCOMPtr<nsICycleCollectorListener> listener =
      do_CreateInstance("@mozilla.org/cycle-collector-logger;1");
    if (!listener) {
      NS_WARNING("Failed to create CC logger");
      return;
    }

    nsCOMPtr<nsICycleCollectorListener> alltracesListener;
    listener->AllTraces(getter_AddRefs(alltracesListener));
    if (!alltracesListener) {
      NS_WARNING("Failed to get all traces logger");
      return;
    }

    nsJSContext::CycleCollectNow(alltracesListener);
}

} // extern "C"

namespace xpc {

bool
Atob(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.length())
        return true;

    return xpc::Base64Decode(cx, args[0], args.rval());
}

bool
Btoa(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.length())
        return true;

    return xpc::Base64Encode(cx, args[0], args.rval());
}

bool
IsXrayWrapper(JSObject *obj)
{
    return WrapperFactory::IsXrayWrapper(obj);
}

JSAddonId *
NewAddonId(JSContext *cx, const nsACString &id)
{
    JS::RootedString str(cx, JS_NewStringCopyN(cx, id.BeginReading(), id.Length()));
    if (!str)
        return nullptr;
    return JS::NewAddonId(cx, str);
}

bool
SetAddonInterposition(const nsACString &addonIdStr, nsIAddonInterposition *interposition)
{
    JSAddonId *addonId;
    {
        // We enter the junk scope just to allocate a string, which actually will go
        // in the system zone.
        AutoJSAPI jsapi;
        jsapi.Init(xpc::GetJunkScopeGlobal());
        addonId = NewAddonId(jsapi.cx(), addonIdStr);
        if (!addonId)
            return false;
    }

    return XPCWrappedNativeScope::SetAddonInterposition(addonId, interposition);
}

} // namespace xpc

namespace mozilla {
namespace dom {

bool
IsChromeOrXBL(JSContext* cx, JSObject* /* unused */)
{
    MOZ_ASSERT(NS_IsMainThread());
    JSCompartment* c = js::GetContextCompartment(cx);

    // For remote XUL, we run XBL in the XUL scope. Given that we care about
    // compat and not security for remote XUL, we just always claim to be XBL.
    //
    // Note that, for performance, we don't check AllowXULXBLForPrincipal here,
    // and instead rely on the fact that AllowContentXBLScope() only returns false in
    // remote XUL situations.
    return AccessCheck::isChrome(c) || IsContentXBLScope(c) || !AllowContentXBLScope(c);
}

} // namespace dom
} // namespace mozilla
