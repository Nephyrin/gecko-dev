/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/TypeUtils.h"

#include "mozilla/unused.h"
#include "mozilla/dom/CacheBinding.h"
#include "mozilla/dom/InternalRequest.h"
#include "mozilla/dom/Request.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/cache/PCacheTypes.h"
#include "mozilla/dom/cache/ReadStream.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "mozilla/ipc/PFileDescriptorSetChild.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "nsCOMPtr.h"
#include "nsIAsyncInputStream.h"
#include "nsIAsyncOutputStream.h"
#include "nsStreamUtils.h"
#include "nsString.h"
#include "nsURLParsers.h"

namespace {

using mozilla::ErrorResult;

// Utility function to remove the fragment from a URL, check its scheme, and optionally
// provide a URL without the query.  We're not using nsIURL or URL to do this because
// they require going to the main thread.
static void
ProcessURL(nsAString& aUrl, bool* aSchemeValidOut,
           nsAString* aUrlWithoutQueryOut, ErrorResult& aRv)
{
  NS_ConvertUTF16toUTF8 flatURL(aUrl);
  const char* url = flatURL.get();

  nsCOMPtr<nsIURLParser> urlParser = new nsStdURLParser();

  uint32_t pathPos;
  int32_t pathLen;
  uint32_t schemePos;
  int32_t schemeLen;
  nsresult rv = urlParser->ParseURL(url, flatURL.Length(),
                                    &schemePos, &schemeLen,
                                    nullptr, nullptr,       // ignore authority
                                    &pathPos, &pathLen);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  if (aSchemeValidOut) {
    nsAutoCString scheme(Substring(flatURL, schemePos, schemeLen));
    *aSchemeValidOut = scheme.LowerCaseEqualsLiteral("http") ||
                       scheme.LowerCaseEqualsLiteral("https");
  }

  uint32_t queryPos;
  int32_t queryLen;
  uint32_t refPos;
  int32_t refLen;

  rv = urlParser->ParsePath(url + pathPos, flatURL.Length() - pathPos,
                            nullptr, nullptr,               // ignore filepath
                            &queryPos, &queryLen,
                            &refPos, &refLen);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aRv.Throw(rv);
    return;
  }

  // TODO: Remove this once Request/Response properly strip the fragment
  if (refLen >= 0) {
    // ParsePath gives us ref position relative to the start of the path
    refPos += pathPos;

    aUrl = Substring(aUrl, 0, refPos - 1);
  }

  if (!aUrlWithoutQueryOut) {
    return;
  }

  if (queryLen < 0) {
    *aUrlWithoutQueryOut = aUrl;
    return;
  }

  // ParsePath gives us query position relative to the start of the path
  queryPos += pathPos;

  // We want everything before the query sine we already removed the trailing
  // fragment
  *aUrlWithoutQueryOut = Substring(aUrl, 0, queryPos - 1);
}

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;
using mozilla::void_t;
using mozilla::ipc::BackgroundChild;
using mozilla::ipc::FileDescriptor;
using mozilla::ipc::PFileDescriptorSetChild;
using mozilla::ipc::PBackgroundChild;

void
TypeUtils::ToPCacheRequest(PCacheRequest& aOut,
                           const RequestOrScalarValueString& aIn, bool aReadBody,
                           ErrorResult& aRv)
{
  AutoJSAPI jsapi;
  jsapi.Init(GetGlobalObject());
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> jsGlobal(cx, GetGlobalObject()->GetGlobalJSObject());
  JSAutoCompartment ac(cx, jsGlobal);

  GlobalObject global(cx, jsGlobal);

  ToPCacheRequest(global, aOut, aIn, aReadBody, aRv);
}

void
TypeUtils::ToPCacheRequest(PCacheRequest& aOut,
                           const OwningRequestOrScalarValueString& aIn,
                           bool aReadBody, ErrorResult& aRv)
{
  AutoJSAPI jsapi;
  jsapi.Init(GetGlobalObject());
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> jsGlobal(cx, GetGlobalObject()->GetGlobalJSObject());
  JSAutoCompartment ac(cx, jsGlobal);

  GlobalObject global(cx, jsGlobal);

  return ToPCacheRequest(global, aOut, aIn, aReadBody, aRv);
}

void
TypeUtils::ToPCacheRequestOrVoid(PCacheRequestOrVoid& aOut,
                                 const Optional<RequestOrScalarValueString>& aIn,
                                 bool aReadBody, ErrorResult& aRv)
{
  AutoJSAPI jsapi;
  jsapi.Init(GetGlobalObject());
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> jsGlobal(cx, GetGlobalObject()->GetGlobalJSObject());
  JSAutoCompartment ac(cx, jsGlobal);

  GlobalObject global(cx, jsGlobal);

  return ToPCacheRequestOrVoid(global, aOut, aIn, aReadBody, aRv);
}

void
TypeUtils::ToPCacheRequest(PCacheRequest& aOut, Request& aIn,
                           bool aReadBody, ErrorResult& aRv)
{
  aIn.GetMethod(aOut.method());
  aIn.GetUrl(aOut.url());

  bool schemeValid;
  ProcessURL(aOut.url(), &schemeValid, &aOut.urlWithoutQuery(), aRv);
  if (aRv.Failed()) {
    return;
  }
  if (!schemeValid) {
    NS_NAMED_LITERAL_STRING(label, "Request");
    aRv.ThrowTypeError(MSG_INVALID_URL_SCHEME, &label, &aOut.url());
    return;
  }

  aIn.GetReferrer(aOut.referrer());
  nsRefPtr<InternalHeaders> headers = aIn.GetInternalHeaders();
  MOZ_ASSERT(headers);
  headers->GetPHeaders(aOut.headers());
  aOut.headersGuard() = headers->Guard();
  aOut.mode() = aIn.Mode();
  aOut.credentials() = aIn.Credentials();

  if (!aReadBody) {
    aOut.body() = void_t();
    return;
  }

  if (aIn.BodyUsed()) {
    aRv.ThrowTypeError(MSG_REQUEST_BODY_CONSUMED_ERROR);
    return;
  }

  nsRefPtr<InternalRequest> internalRequest = aIn.GetInternalRequest();
  MOZ_ASSERT(internalRequest);
  nsCOMPtr<nsIInputStream> stream;

  internalRequest->GetBody(getter_AddRefs(stream));
  if (stream) {
    aIn.SetBodyUsed();
  }

  SerializeCacheStream(stream, &aOut.body(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }
}

// static
void
TypeUtils::ToPCacheRequest(const GlobalObject& aGlobal,
                           PCacheRequest& aOut,
                           const RequestOrScalarValueString& aIn,
                           bool aReadBody, ErrorResult& aRv)
{
  if (aIn.IsRequest()) {
    ToPCacheRequest(aOut, aIn.GetAsRequest(), aReadBody, aRv);
    return;
  }

  RequestInit init;
  nsRefPtr<Request> request = Request::Constructor(aGlobal, aIn, init, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }
  ToPCacheRequest(aOut, *request, aReadBody, aRv);
}

void
TypeUtils::ToPCacheRequestOrVoid(const GlobalObject& aGlobal, PCacheRequestOrVoid& aOut,
                                 const Optional<RequestOrScalarValueString>& aIn,
                                 bool aReadBody, ErrorResult& aRv)
{
  if (!aIn.WasPassed()) {
    aOut = void_t();
    return;
  }
  PCacheRequest request;
  ToPCacheRequest(aGlobal, request, aIn.Value(), aReadBody, aRv);
  if (aRv.Failed()) {
    return;
  }
  aOut = request;
}

void
TypeUtils::ToPCacheRequest(const GlobalObject& aGlobal, PCacheRequest& aOut,
                           const OwningRequestOrScalarValueString& aIn,
                           bool aReadBody, ErrorResult& aRv)
{
  if (aIn.IsRequest()) {
    ToPCacheRequest(aOut, aIn.GetAsRequest(), aReadBody, aRv);
    return;
  }

  RequestOrScalarValueString input;
  RequestInit init;
  nsString str;
  str.Assign(aIn.GetAsScalarValueString());
  input.SetAsScalarValueString().Rebind(str.Data(), str.Length());

  nsRefPtr<Request> request = Request::Constructor(aGlobal, input, init, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }
  ToPCacheRequest(aOut, *request, aReadBody, aRv);
}

void
TypeUtils::ToPCacheResponse(PCacheResponse& aOut, Response& aIn,
                            ErrorResult& aRv)
{
  aOut.type() = aIn.Type();
  aIn.GetUrl(aOut.url());

  if (aOut.url() != EmptyString()) {
    bool schemeValid;
    ProcessURL(aOut.url(), &schemeValid, nullptr, aRv);
    if (aRv.Failed()) {
      return;
    }
    if (!schemeValid) {
      NS_NAMED_LITERAL_STRING(label, "Response");
      aRv.ThrowTypeError(MSG_INVALID_URL_SCHEME, &label, &aOut.url());
      return;
    }
  }

  aOut.status() = aIn.Status();
  aIn.GetStatusText(aOut.statusText());
  nsRefPtr<InternalHeaders> headers = aIn.GetInternalHeaders();
  MOZ_ASSERT(headers);
  headers->GetPHeaders(aOut.headers());
  aOut.headersGuard() = headers->Guard();

  if (aIn.BodyUsed()) {
    aRv.ThrowTypeError(MSG_REQUEST_BODY_CONSUMED_ERROR);
    return;
  }

  nsCOMPtr<nsIInputStream> stream;
  aIn.GetBody(getter_AddRefs(stream));
  if (stream) {
    aIn.SetBodyUsed();
  }

  SerializeCacheStream(stream, &aOut.body(), aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }
}

// static
void
TypeUtils::ToPCacheQueryParams(PCacheQueryParams& aOut, const QueryParams& aIn)
{
  aOut.ignoreSearch() = aIn.mIgnoreSearch.WasPassed() &&
                        aIn.mIgnoreSearch.Value();
  aOut.ignoreMethod() = aIn.mIgnoreMethod.WasPassed() &&
                        aIn.mIgnoreMethod.Value();
  aOut.ignoreVary() = aIn.mIgnoreVary.WasPassed() &&
                      aIn.mIgnoreVary.Value();
  aOut.prefixMatch() = aIn.mPrefixMatch.WasPassed() &&
                       aIn.mPrefixMatch.Value();
  aOut.cacheNameSet() = aIn.mCacheName.WasPassed();
  if (aOut.cacheNameSet()) {
    aOut.cacheName() = aIn.mCacheName.Value();
  } else {
    aOut.cacheName() = NS_LITERAL_STRING("");
  }
}

already_AddRefed<Response>
TypeUtils::ToResponse(const PCacheResponse& aIn,
                      PCacheStreamControlChild* aStreamControl)
{
  nsRefPtr<InternalResponse> ir;
  switch (aIn.type())
  {
    case ResponseType::Error:
      ir = InternalResponse::NetworkError();
      break;
    case ResponseType::Opaque:
      ir = InternalResponse::OpaqueResponse();
      break;
    case ResponseType::Default:
      ir = new InternalResponse(aIn.status(), aIn.statusText());
      break;
    case ResponseType::Basic:
    {
      nsRefPtr<InternalResponse> inner = new InternalResponse(aIn.status(),
                                                              aIn.statusText());
      ir = InternalResponse::BasicResponse(inner);
      break;
    }
    case ResponseType::Cors:
    {
      nsRefPtr<InternalResponse> inner = new InternalResponse(aIn.status(),
                                                              aIn.statusText());
      ir = InternalResponse::CORSResponse(inner);
      break;
    }
    default:
      MOZ_CRASH("Unexpected ResponseType!");
  }
  MOZ_ASSERT(ir);

  ir->SetUrl(NS_ConvertUTF16toUTF8(aIn.url()));

  nsRefPtr<InternalHeaders> internalHeaders =
    new InternalHeaders(aIn.headers(), aIn.headersGuard());
  ErrorResult result;
  ir->Headers()->SetGuard(aIn.headersGuard(), result);
  ir->Headers()->Fill(*internalHeaders, result);
  MOZ_ASSERT(!result.Failed());

  nsCOMPtr<nsIInputStream> stream = ReadStream::Create(aStreamControl,
                                                       aIn.body());
  ir->SetBody(stream);

  nsRefPtr<Response> ref = new Response(GetGlobalObject(), ir);
  return ref.forget();
}

already_AddRefed<Request>
TypeUtils::ToRequest(const PCacheRequest& aIn,
                     PCacheStreamControlChild* aStreamControl)
{
  nsRefPtr<InternalRequest> internalRequest = new InternalRequest();

  internalRequest->SetMethod(aIn.method());
  internalRequest->SetURL(NS_ConvertUTF16toUTF8(aIn.url()));
  internalRequest->SetReferrer(NS_ConvertUTF16toUTF8(aIn.referrer()));
  internalRequest->SetMode(aIn.mode());
  internalRequest->SetCredentialsMode(aIn.credentials());

  nsRefPtr<InternalHeaders> internalHeaders =
    new InternalHeaders(aIn.headers(), aIn.headersGuard());
  ErrorResult result;
  internalRequest->Headers()->SetGuard(aIn.headersGuard(), result);
  internalRequest->Headers()->Fill(*internalHeaders, result);
  MOZ_ASSERT(!result.Failed());

  nsCOMPtr<nsIInputStream> stream = ReadStream::Create(aStreamControl,
                                                       aIn.body());

  internalRequest->SetBody(stream);

  nsRefPtr<Request> request = new Request(GetGlobalObject(), internalRequest);
  return request.forget();
}

void
TypeUtils::SerializeCacheStream(nsIInputStream* aStream,
                                PCacheReadStreamOrVoid* aStreamOut,
                                ErrorResult& aRv)
{
  MOZ_ASSERT(aStreamOut);
  if (!aStream) {
    *aStreamOut = void_t();
    return;
  }

  PCacheReadStream readStream;
  nsTArray<FileDescriptor> fds;
  SerializeInputStream(aStream, readStream.params(), fds);

  PFileDescriptorSetChild* fdSet = nullptr;
  if (!fds.IsEmpty()) {
    // We should not be serializing until we have an actor ready
    PBackgroundChild* manager = BackgroundChild::GetForCurrentThread();
    MOZ_ASSERT(manager);

    fdSet = manager->SendPFileDescriptorSetConstructor(fds[0]);
    for (uint32_t i = 1; i < fds.Length(); ++i) {
      unused << fdSet->SendAddFileDescriptor(fds[i]);
    }
  }

  if (fdSet) {
    readStream.fds() = fdSet;
  } else {
    readStream.fds() = void_t();
  }

  *aStreamOut = readStream;
}

nsIThread*
TypeUtils::GetStreamThread()
{
  AssertOwningThread();

  if (!mStreamThread) {
    nsresult rv = NS_NewNamedThread("DOMCacheTypeU",
                                    getter_AddRefs(mStreamThread));
    if (NS_FAILED(rv) || !mStreamThread) {
      MOZ_CRASH("Failed to create DOM Cache serialization thread.");
    }
  }

  return mStreamThread;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
