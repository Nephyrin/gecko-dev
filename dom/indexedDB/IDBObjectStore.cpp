/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBObjectStore.h"

#include "FileInfo.h"
#include "IDBCursor.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBIndex.h"
#include "IDBKeyRange.h"
#include "IDBMutableFile.h"
#include "IDBRequest.h"
#include "IDBTransaction.h"
#include "IndexedDatabase.h"
#include "IndexedDatabaseInlines.h"
#include "IndexedDatabaseManager.h"
#include "js/Class.h"
#include "js/StructuredClone.h"
#include "KeyPath.h"
#include "mozilla/Endian.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Move.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/DOMStringList.h"
#include "mozilla/dom/FileHandleBinding.h"
#include "mozilla/dom/IDBObjectStoreBinding.h"
#include "mozilla/dom/StructuredCloneTags.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "mozilla/dom/ipc/BlobChild.h"
#include "mozilla/dom/ipc/BlobParent.h"
#include "mozilla/dom/ipc/nsIRemoteBlob.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "nsCOMPtr.h"
#include "nsDOMFile.h"
#include "nsIDOMFile.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

// Include this last to avoid path problems on Windows.
#include "ActorsChild.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

struct IDBObjectStore::StructuredCloneWriteInfo
{
  JSAutoStructuredCloneBuffer mCloneBuffer;
  nsTArray<nsCOMPtr<nsIDOMBlob>> mFiles;
  uint64_t mOffsetToKeyProp;

  StructuredCloneWriteInfo()
    : mOffsetToKeyProp(0)
  {
    MOZ_COUNT_CTOR(StructuredCloneWriteInfo);
  }

  StructuredCloneWriteInfo(StructuredCloneWriteInfo&& aCloneWriteInfo)
    : mCloneBuffer(Move(aCloneWriteInfo.mCloneBuffer))
    , mOffsetToKeyProp(aCloneWriteInfo.mOffsetToKeyProp)
  {
    MOZ_COUNT_CTOR(StructuredCloneWriteInfo);

    mFiles.SwapElements(aCloneWriteInfo.mFiles);
    aCloneWriteInfo.mOffsetToKeyProp = 0;
  }

  ~StructuredCloneWriteInfo()
  {
    MOZ_COUNT_DTOR(StructuredCloneWriteInfo);
  }

  bool
  operator==(const StructuredCloneWriteInfo& aOther) const
  {
    return this->mCloneBuffer.nbytes() == aOther.mCloneBuffer.nbytes() &&
           this->mCloneBuffer.data() == aOther.mCloneBuffer.data() &&
           this->mFiles == aOther.mFiles &&
           this->mOffsetToKeyProp == aOther.mOffsetToKeyProp;
  }

  bool
  SetFromSerialized(const SerializedStructuredCloneWriteInfo& aOther)
  {
    if (aOther.data().IsEmpty()) {
      mCloneBuffer.clear();
    } else {
      auto* aOtherBuffer =
        reinterpret_cast<uint64_t*>(
          const_cast<uint8_t*>(aOther.data().Elements()));
      if (!mCloneBuffer.copy(aOtherBuffer, aOther.data().Length())) {
        return false;
      }
    }

    mFiles.Clear();
    mOffsetToKeyProp = aOther.offsetToKeyProp();
    return true;
  }
};

namespace {

struct MOZ_STACK_CLASS MutableFileData
{
  nsString type;
  nsString name;

  MutableFileData()
  {
    MOZ_COUNT_CTOR(MutableFileData);
  }

  ~MutableFileData()
  {
    MOZ_COUNT_DTOR(MutableFileData);
  }
};

struct MOZ_STACK_CLASS BlobOrFileData
{
  uint32_t tag;
  uint64_t size;
  nsString type;
  nsString name;
  uint64_t lastModifiedDate;

  BlobOrFileData()
    : tag(0)
    , size(0)
    , lastModifiedDate(UINT64_MAX)
  {
    MOZ_COUNT_CTOR(BlobOrFileData);
  }

  ~BlobOrFileData()
  {
    MOZ_COUNT_DTOR(BlobOrFileData);
  }
};

struct MOZ_STACK_CLASS GetAddInfoClosure
{
  IDBObjectStore::StructuredCloneWriteInfo& mCloneWriteInfo;
  JS::Handle<JS::Value> mValue;

  GetAddInfoClosure(IDBObjectStore::StructuredCloneWriteInfo& aCloneWriteInfo,
                    JS::Handle<JS::Value> aValue)
    : mCloneWriteInfo(aCloneWriteInfo)
    , mValue(aValue)
  {
    MOZ_COUNT_CTOR(GetAddInfoClosure);
  }

  ~GetAddInfoClosure()
  {
    MOZ_COUNT_DTOR(GetAddInfoClosure);
  }
};

already_AddRefed<IDBRequest>
GenerateRequest(IDBObjectStore* aObjectStore)
{
  MOZ_ASSERT(aObjectStore);
  aObjectStore->AssertIsOnOwningThread();

  IDBTransaction* transaction = aObjectStore->Transaction();

  nsRefPtr<IDBRequest> request =
    IDBRequest::Create(aObjectStore, transaction->Database(), transaction);
  MOZ_ASSERT(request);

  return request.forget();
}

bool
StructuredCloneWriteCallback(JSContext* aCx,
                             JSStructuredCloneWriter* aWriter,
                             JS::Handle<JSObject*> aObj,
                             void* aClosure)
{
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aClosure);

  auto cloneWriteInfo =
    static_cast<IDBObjectStore::StructuredCloneWriteInfo*>(aClosure);

  if (JS_GetClass(aObj) == IDBObjectStore::DummyPropClass()) {
    MOZ_ASSERT(!cloneWriteInfo->mOffsetToKeyProp);
    cloneWriteInfo->mOffsetToKeyProp = js_GetSCOffset(aWriter);

    uint64_t value = 0;
    // Omit endian swap
    return JS_WriteBytes(aWriter, &value, sizeof(value));
  }

  // XXX Fix filehandle!
  /*
  IDBTransaction* transaction = cloneWriteInfo->mTransaction;
  FileManager* fileManager = transaction->Database()->Manager();

  MutableFile* mutableFile = nullptr;
  if (NS_SUCCEEDED(UNWRAP_OBJECT(MutableFile, aObj, mutableFile))) {
    nsRefPtr<FileInfo> fileInfo = mutableFile->GetFileInfo();

    // Throw when trying to store non IDB mutable files or IDB mutable files
    // across databases.
    if (!fileInfo || fileInfo->Manager() != fileManager) {
      return false;
    }

    NS_ConvertUTF16toUTF8 convType(mutableFile->Type());
    uint32_t convTypeLength =
      NativeEndian::swapToLittleEndian(convType.Length());

    NS_ConvertUTF16toUTF8 convName(mutableFile->Name());
    uint32_t convNameLength =
      NativeEndian::swapToLittleEndian(convName.Length());

    if (!JS_WriteUint32Pair(aWriter, SCTAG_DOM_MUTABLEFILE,
                            cloneWriteInfo->mFiles.Length()) ||
        !JS_WriteBytes(aWriter, &convTypeLength, sizeof(uint32_t)) ||
        !JS_WriteBytes(aWriter, convType.get(), convType.Length()) ||
        !JS_WriteBytes(aWriter, &convNameLength, sizeof(uint32_t)) ||
        !JS_WriteBytes(aWriter, convName.get(), convName.Length())) {
      return false;
    }

    StructuredCloneFile* file = cloneWriteInfo->mFiles.AppendElement();
    file->mFileInfo = fileInfo.forget();

    return true;
  }
  */

  MOZ_ASSERT(NS_IsMainThread(), "This can't work off the main thread!");

  nsCOMPtr<nsIXPConnectWrappedNative> wrappedNative;
  nsContentUtils::XPConnect()->
    GetWrappedNativeOfJSObject(aCx, aObj, getter_AddRefs(wrappedNative));

  if (wrappedNative) {
    nsISupports* supports = wrappedNative->Native();

    nsCOMPtr<nsIDOMBlob> blob = do_QueryInterface(supports);
    if (blob) {
      uint64_t size;
      MOZ_ALWAYS_TRUE(NS_SUCCEEDED(blob->GetSize(&size)));

      size = NativeEndian::swapToLittleEndian(size);

      nsString type;
      MOZ_ALWAYS_TRUE(NS_SUCCEEDED(blob->GetType(type)));

      NS_ConvertUTF16toUTF8 convType(type);
      uint32_t convTypeLength =
        NativeEndian::swapToLittleEndian(convType.Length());

      nsCOMPtr<nsIDOMFile> file = do_QueryInterface(blob);

      if (!JS_WriteUint32Pair(aWriter, file ? SCTAG_DOM_FILE : SCTAG_DOM_BLOB,
                              cloneWriteInfo->mFiles.Length()) ||
          !JS_WriteBytes(aWriter, &size, sizeof(size)) ||
          !JS_WriteBytes(aWriter, &convTypeLength, sizeof(convTypeLength)) ||
          !JS_WriteBytes(aWriter, convType.get(), convType.Length())) {
        return false;
      }

      if (file) {
        uint64_t lastModifiedDate;
        MOZ_ALWAYS_TRUE(NS_SUCCEEDED(
          file->GetMozLastModifiedDate(&lastModifiedDate)));

        lastModifiedDate = NativeEndian::swapToLittleEndian(lastModifiedDate);

        nsString name;
        MOZ_ALWAYS_TRUE(NS_SUCCEEDED(file->GetName(name)));

        NS_ConvertUTF16toUTF8 convName(name);
        uint32_t convNameLength =
          NativeEndian::swapToLittleEndian(convName.Length());

        if (!JS_WriteBytes(aWriter, &lastModifiedDate, sizeof(lastModifiedDate)) || 
            !JS_WriteBytes(aWriter, &convNameLength, sizeof(convNameLength)) ||
            !JS_WriteBytes(aWriter, convName.get(), convName.Length())) {
          return false;
        }
      }

      nsCOMPtr<nsIDOMBlob>* fileEntry = cloneWriteInfo->mFiles.AppendElement();
      fileEntry->swap(blob);
      return true;
    }
  }

  // Try using the runtime callbacks
  const JSStructuredCloneCallbacks* runtimeCallbacks =
    js::GetContextStructuredCloneCallbacks(aCx);
  if (runtimeCallbacks) {
    return runtimeCallbacks->write(aCx, aWriter, aObj, nullptr);
  }

  return false;
}

nsresult
GetAddInfoCallback(JSContext* aCx, void* aClosure)
{
  static const JSStructuredCloneCallbacks kStructuredCloneCallbacks = {
    nullptr /* read */,
    StructuredCloneWriteCallback /* write */,
    nullptr /* reportError */,
    nullptr /* readTransfer */,
    nullptr /* writeTransfer */,
    nullptr /* freeTransfer */
  };

  MOZ_ASSERT(aCx);

  auto* data = static_cast<GetAddInfoClosure*>(aClosure);
  MOZ_ASSERT(data);

  data->mCloneWriteInfo.mOffsetToKeyProp = 0;

  if (!data->mCloneWriteInfo.mCloneBuffer.write(aCx,
                                                data->mValue,
                                                &kStructuredCloneCallbacks,
                                                &data->mCloneWriteInfo)) {
    return NS_ERROR_DOM_DATA_CLONE_ERR;
  }

  return NS_OK;
}

BlobChild*
ActorFromRemoteBlob(nsIDOMBlob* aBlob)
{
  MOZ_ASSERT(aBlob);

  nsRefPtr<DOMFile> blob = static_cast<DOMFile*>(aBlob);
  nsCOMPtr<nsIRemoteBlob> remoteBlob = do_QueryInterface(blob->Impl());
  if (remoteBlob) {
    BlobChild* actor = remoteBlob->GetBlobChild();
    MOZ_ASSERT(actor);

    if (actor->GetContentManager()) {
      return nullptr;
    }

    MOZ_ASSERT(actor->GetBackgroundManager());
    MOZ_ASSERT(BackgroundChild::GetForCurrentThread());
    MOZ_ASSERT(actor->GetBackgroundManager() ==
                 BackgroundChild::GetForCurrentThread(),
               "Blob actor is not bound to this thread!");

    return actor;
  }

  return nullptr;
}

bool
ResolveMysteryFile(nsIDOMBlob* aBlob,
                   const nsString& aName,
                   const nsString& aContentType,
                   uint64_t aSize,
                   uint64_t aLastModifiedDate)
{
  BlobChild* actor = ActorFromRemoteBlob(aBlob);
  if (actor) {
    return actor->SetMysteryBlobInfo(aName, aContentType,
                                     aSize, aLastModifiedDate);
  }
  return true;
}

bool
ResolveMysteryBlob(nsIDOMBlob* aBlob,
                   const nsString& aContentType,
                   uint64_t aSize)
{
  BlobChild* actor = ActorFromRemoteBlob(aBlob);
  if (actor) {
    return actor->SetMysteryBlobInfo(aContentType, aSize);
  }
  return true;
}

bool
StructuredCloneReadString(JSStructuredCloneReader* aReader,
                          nsCString& aString)
{
  uint32_t length;
  if (!JS_ReadBytes(aReader, &length, sizeof(uint32_t))) {
    NS_WARNING("Failed to read length!");
    return false;
  }
  length = NativeEndian::swapFromLittleEndian(length);

  if (!aString.SetLength(length, fallible_t())) {
    NS_WARNING("Out of memory?");
    return false;
  }
  char* buffer = aString.BeginWriting();

  if (!JS_ReadBytes(aReader, buffer, length)) {
    NS_WARNING("Failed to read type!");
    return false;
  }

  return true;
}

bool
ReadFileHandle(JSStructuredCloneReader* aReader,
               MutableFileData* aRetval)
{
  static_assert(SCTAG_DOM_MUTABLEFILE == 0xFFFF8004, "Update me!");
  MOZ_ASSERT(aReader && aRetval);

  nsCString type;
  if (!StructuredCloneReadString(aReader, type)) {
    return false;
  }
  CopyUTF8toUTF16(type, aRetval->type);

  nsCString name;
  if (!StructuredCloneReadString(aReader, name)) {
    return false;
  }
  CopyUTF8toUTF16(name, aRetval->name);

  return true;
}

bool
ReadBlobOrFile(JSStructuredCloneReader* aReader,
               uint32_t aTag,
               BlobOrFileData* aRetval)
{
  static_assert(SCTAG_DOM_BLOB == 0xffff8001 &&
                SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE == 0xffff8002 &&
                SCTAG_DOM_FILE == 0xffff8005,
                "Update me!");

  MOZ_ASSERT(aReader);
  MOZ_ASSERT(aTag == SCTAG_DOM_FILE ||
             aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
             aTag == SCTAG_DOM_BLOB);
  MOZ_ASSERT(aRetval);

  aRetval->tag = aTag;

  uint64_t size;
  if (NS_WARN_IF(!JS_ReadBytes(aReader, &size, sizeof(uint64_t)))) {
    return false;
  }

  aRetval->size = NativeEndian::swapFromLittleEndian(size);

  nsCString type;
  if (NS_WARN_IF(!StructuredCloneReadString(aReader, type))) {
    return false;
  }

  CopyUTF8toUTF16(type, aRetval->type);

  // Blobs are done.
  if (aTag == SCTAG_DOM_BLOB) {
    return true;
  }

  MOZ_ASSERT(aTag == SCTAG_DOM_FILE ||
             aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE);

  uint64_t lastModifiedDate;
  if (aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE) {
    lastModifiedDate = UINT64_MAX;
  } else {
    if (NS_WARN_IF(!JS_ReadBytes(aReader, &lastModifiedDate,
                                sizeof(lastModifiedDate)))) {
      return false;
    }
    lastModifiedDate = NativeEndian::swapFromLittleEndian(lastModifiedDate);
  }

  aRetval->lastModifiedDate = lastModifiedDate;

  nsCString name;
  if (NS_WARN_IF(!StructuredCloneReadString(aReader, name))) {
    return false;
  }

  CopyUTF8toUTF16(name, aRetval->name);

  return true;
}

class ValueDeserializationHelper
{
public:
  static bool
  CreateAndWrapMutableFile(JSContext* aCx,
                           IDBDatabase* aDatabase,
                           StructuredCloneFile& aFile,
                           const MutableFileData& aData,
                           JS::MutableHandle<JSObject*> aResult)
  {
    MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
    MOZ_ASSERT(NS_IsMainThread());
    MOZ_ASSERT(aDatabase);

    nsRefPtr<FileInfo>& fileInfo = aFile.mFileInfo;

    nsRefPtr<IDBMutableFile> mutableFile =
      IDBMutableFile::Create(aDatabase,
                             aData.name,
                             aData.type,
                             fileInfo.forget());
    MOZ_ASSERT(mutableFile);

    JS::Rooted<JSObject*> result(aCx, mutableFile->WrapObject(aCx));
    if (NS_WARN_IF(!result)) {
      return false;
    }

    aResult.set(result);
    return true;
  }

  static bool
  CreateAndWrapBlobOrFile(JSContext* aCx,
                          StructuredCloneFile& aFile,
                          const BlobOrFileData& aData,
                          JS::MutableHandle<JSObject*> aResult)
  {
    MOZ_ASSERT(aData.tag == SCTAG_DOM_FILE ||
               aData.tag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
               aData.tag == SCTAG_DOM_BLOB);
    MOZ_ASSERT(aFile.mFile);

    MOZ_ASSERT(NS_IsMainThread(),
               "This wrapping currently only works on the main thread!");

    if (aData.tag == SCTAG_DOM_BLOB) {
      if (NS_WARN_IF(!ResolveMysteryBlob(aFile.mFile,
                                         aData.type,
                                         aData.size))) {
        return false;
      }

      JS::Rooted<JS::Value> wrappedBlob(aCx);
      nsresult rv =
        nsContentUtils::WrapNative(aCx,
                                   aFile.mFile,
                                   &NS_GET_IID(nsIDOMBlob),
                                   &wrappedBlob);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return false;
      }

      aResult.set(&wrappedBlob.toObject());
      return true;
    }

    nsCOMPtr<nsIDOMFile> domFile = do_QueryInterface(aFile.mFile);
    MOZ_ASSERT(domFile);

    if (NS_WARN_IF(!ResolveMysteryFile(domFile,
                                       aData.name,
                                       aData.type,
                                       aData.size,
                                       aData.lastModifiedDate))) {
      return false;
    }

    JS::Rooted<JS::Value> wrappedFile(aCx);
    nsresult rv =
      nsContentUtils::WrapNative(aCx,
                                 aFile.mFile,
                                 &NS_GET_IID(nsIDOMFile),
                                 &wrappedFile);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return false;
    }

    aResult.set(&wrappedFile.toObject());
    return true;
  }
};

class IndexDeserializationHelper
{
public:
  static bool
  CreateAndWrapMutableFile(JSContext* aCx,
                           IDBDatabase* aDatabase,
                           StructuredCloneFile& aFile,
                           const MutableFileData& aData,
                           JS::MutableHandle<JSObject*> aResult)
  {
    MOZ_ASSERT(!aDatabase);

    // MutableFile can't be used in index creation, so just make a dummy object.
    JS::Rooted<JSObject*> obj(aCx,
      JS_NewObject(aCx, nullptr, JS::NullPtr(), JS::NullPtr()));

    if (NS_WARN_IF(!obj)) {
      return false;
    }

    aResult.set(obj);
    return true;
  }

  static bool
  CreateAndWrapBlobOrFile(JSContext* aCx,
                          StructuredCloneFile& aFile,
                          const BlobOrFileData& aData,
                          JS::MutableHandle<JSObject*> aResult)
  {
    MOZ_ASSERT(aData.tag == SCTAG_DOM_FILE ||
               aData.tag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
               aData.tag == SCTAG_DOM_BLOB);

    // The following properties are available for use in index creation
    //   Blob.size
    //   Blob.type
    //   File.name
    //   File.lastModifiedDate

    JS::Rooted<JSObject*> obj(aCx,
      JS_NewObject(aCx, nullptr, JS::NullPtr(), JS::NullPtr()));
    if (NS_WARN_IF(!obj)) {
      return false;
    }

    // Technically these props go on the proto, but this detail won't change
    // the results of index creation.

    JS::Rooted<JSString*> type(aCx,
      JS_NewUCStringCopyN(aCx, aData.type.get(), aData.type.Length()));
    if (NS_WARN_IF(!type)) {
      return false;
    }

    if (NS_WARN_IF(!JS_DefineProperty(aCx,
                                      obj,
                                      "size",
                                      double(aData.size),
                                      0))) {
      return false;
    }

    if (NS_WARN_IF(!JS_DefineProperty(aCx, obj, "type", type, 0))) {
      return false;
    }

    if (aData.tag == SCTAG_DOM_BLOB) {
      aResult.set(obj);
      return true;
    }

    JS::Rooted<JSString*> name(aCx,
      JS_NewUCStringCopyN(aCx, aData.name.get(), aData.name.Length()));
    if (NS_WARN_IF(!name)) {
      return false;
    }

    JS::Rooted<JSObject*> date(aCx,
      JS_NewDateObjectMsec(aCx, aData.lastModifiedDate));
    if (NS_WARN_IF(!date)) {
      return false;
    }

    if (NS_WARN_IF(!JS_DefineProperty(aCx, obj, "name", name, 0))) {
      return false;
    }

    if (NS_WARN_IF(!JS_DefineProperty(aCx, obj, "lastModifiedDate", date, 0))) {
      return false;
    }

    aResult.set(obj);
    return true;
  }
};

template <class Traits>
JSObject*
CommonStructuredCloneReadCallback(JSContext* aCx,
                                  JSStructuredCloneReader* aReader,
                                  uint32_t aTag,
                                  uint32_t aData,
                                  void* aClosure)
{
  // We need to statically assert that our tag values are what we expect
  // so that if people accidentally change them they notice.
  static_assert(SCTAG_DOM_BLOB == 0xffff8001 &&
                SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE == 0xffff8002 &&
                SCTAG_DOM_MUTABLEFILE == 0xffff8004 &&
                SCTAG_DOM_FILE == 0xffff8005,
                "You changed our structured clone tag values and just ate "
                "everyone's IndexedDB data.  I hope you are happy.");

  if (aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
      aTag == SCTAG_DOM_MUTABLEFILE ||
      aTag == SCTAG_DOM_BLOB ||
      aTag == SCTAG_DOM_FILE) {
    StructuredCloneReadInfo* cloneReadInfo =
      reinterpret_cast<StructuredCloneReadInfo*>(aClosure);

    if (aData >= cloneReadInfo->mFiles.Length()) {
      MOZ_ASSERT(false, "Bad index value!");
      return nullptr;
    }

    StructuredCloneFile& file = cloneReadInfo->mFiles[aData];

    JS::Rooted<JSObject*> result(aCx);

    if (aTag == SCTAG_DOM_MUTABLEFILE) {
      MutableFileData data;
      if (NS_WARN_IF(!ReadFileHandle(aReader, &data))) {
        return nullptr;
      }

      if (NS_WARN_IF(!Traits::CreateAndWrapMutableFile(aCx,
                                                       cloneReadInfo->mDatabase,
                                                       file,
                                                       data,
                                                       &result))) {
        return nullptr;
      }

      return result;
    }

    BlobOrFileData data;
    if (NS_WARN_IF(!ReadBlobOrFile(aReader, aTag, &data))) {
      return nullptr;
    }

    if (NS_WARN_IF(!Traits::CreateAndWrapBlobOrFile(aCx,
                                                    file,
                                                    data,
                                                    &result))) {
      return nullptr;
    }

    return result;
  }

  const JSStructuredCloneCallbacks* runtimeCallbacks =
    js::GetContextStructuredCloneCallbacks(aCx);

  if (runtimeCallbacks) {
    return runtimeCallbacks->read(aCx, aReader, aTag, aData, nullptr);
  }

  return nullptr;
}

// static
void
ClearStructuredCloneBuffer(JSAutoStructuredCloneBuffer& aBuffer)
{
  if (aBuffer.data()) {
    aBuffer.clear();
  }
}

} // anonymous namespace

const JSClass IDBObjectStore::sDummyPropJSClass = {
  "IDBObjectStore Dummy",
  0 /* flags */,
  JS_PropertyStub /* addProperty */,
  JS_DeletePropertyStub /* delProperty */,
  JS_PropertyStub /* getProperty */,
  JS_StrictPropertyStub /* setProperty */,
  JS_EnumerateStub /* enumerate */,
  JS_ResolveStub /* resolve */,
  JS_ConvertStub /* convert */,
  JSCLASS_NO_OPTIONAL_MEMBERS
};

IDBObjectStore::IDBObjectStore(IDBTransaction* aTransaction,
                               const ObjectStoreSpec* aSpec)
  : mTransaction(aTransaction)
  , mCachedKeyPath(JSVAL_VOID)
  , mSpec(aSpec)
  , mId(aSpec->metadata().id())
  , mRooted(false)
{
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();
  MOZ_ASSERT(aSpec);

  SetIsDOMBinding();
}

IDBObjectStore::~IDBObjectStore()
{
  AssertIsOnOwningThread();

  if (mRooted) {
    mCachedKeyPath = JSVAL_VOID;
    mozilla::DropJSObjects(this);
  }
}

// static
already_AddRefed<IDBObjectStore>
IDBObjectStore::Create(IDBTransaction* aTransaction,
                       const ObjectStoreSpec& aSpec)
{
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();

  nsRefPtr<IDBObjectStore> objectStore =
    new IDBObjectStore(aTransaction, &aSpec);

  return objectStore.forget();
}

// static
nsresult
IDBObjectStore::AppendIndexUpdateInfo(
                                    int64_t aIndexID,
                                    const KeyPath& aKeyPath,
                                    bool aUnique,
                                    bool aMultiEntry,
                                    JSContext* aCx,
                                    JS::Handle<JS::Value> aVal,
                                    nsTArray<IndexUpdateInfo>& aUpdateInfoArray)
{
  nsresult rv;

  if (!aMultiEntry) {
    Key key;
    rv = aKeyPath.ExtractKey(aCx, aVal, key);

    // If an index's keyPath doesn't match an object, we ignore that object.
    if (rv == NS_ERROR_DOM_INDEXEDDB_DATA_ERR || key.IsUnset()) {
      return NS_OK;
    }

    if (NS_FAILED(rv)) {
      return rv;
    }

    IndexUpdateInfo* updateInfo = aUpdateInfoArray.AppendElement();
    updateInfo->indexId() = aIndexID;
    updateInfo->value() = key;

    return NS_OK;
  }

  JS::Rooted<JS::Value> val(aCx);
  if (NS_FAILED(aKeyPath.ExtractKeyAsJSVal(aCx, aVal, val.address()))) {
    return NS_OK;
  }

  if (JS_IsArrayObject(aCx, val)) {
    JS::Rooted<JSObject*> array(aCx, &val.toObject());
    uint32_t arrayLength;
    if (NS_WARN_IF(!JS_GetArrayLength(aCx, array, &arrayLength))) {
      IDB_REPORT_INTERNAL_ERR();
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    for (uint32_t arrayIndex = 0; arrayIndex < arrayLength; arrayIndex++) {
      JS::Rooted<JS::Value> arrayItem(aCx);
      if (NS_WARN_IF(!JS_GetElement(aCx, array, arrayIndex, &arrayItem))) {
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      Key value;
      if (NS_FAILED(value.SetFromJSVal(aCx, arrayItem)) ||
          value.IsUnset()) {
        // Not a value we can do anything with, ignore it.
        continue;
      }

      IndexUpdateInfo* updateInfo = aUpdateInfoArray.AppendElement();
      updateInfo->indexId() = aIndexID;
      updateInfo->value() = value;
    }
  }
  else {
    Key value;
    if (NS_FAILED(value.SetFromJSVal(aCx, val)) ||
        value.IsUnset()) {
      // Not a value we can do anything with, ignore it.
      return NS_OK;
    }

    IndexUpdateInfo* updateInfo = aUpdateInfoArray.AppendElement();
    updateInfo->indexId() = aIndexID;
    updateInfo->value() = value;
  }

  return NS_OK;
}

// static
void
IDBObjectStore::ClearCloneReadInfo(StructuredCloneReadInfo& aReadInfo)
{
  // This is kind of tricky, we only want to release stuff on the main thread,
  // but we can end up being called on other threads if we have already been
  // cleared on the main thread.
  if (!aReadInfo.mCloneBuffer.data() && !aReadInfo.mFiles.Length()) {
    return;
  }

  // If there's something to clear, we should be on the main thread.
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  ClearStructuredCloneBuffer(aReadInfo.mCloneBuffer);
  aReadInfo.mFiles.Clear();
}

// static
bool
IDBObjectStore::DeserializeValue(JSContext* aCx,
                                 StructuredCloneReadInfo& aCloneReadInfo,
                                 JS::MutableHandle<JS::Value> aValue)
{
  MOZ_ASSERT(aCx);

  if (aCloneReadInfo.mData.IsEmpty()) {
    aValue.setUndefined();
    return true;
  }

  size_t dataLen = aCloneReadInfo.mData.Length();

  uint64_t* data =
    const_cast<uint64_t*>(reinterpret_cast<uint64_t*>(
      aCloneReadInfo.mData.Elements()));

  MOZ_ASSERT(!(dataLen % sizeof(*data)));

  JSAutoRequest ar(aCx);

  static JSStructuredCloneCallbacks callbacks = {
    CommonStructuredCloneReadCallback<ValueDeserializationHelper>,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
  };

  if (!JS_ReadStructuredClone(aCx, data, dataLen, JS_STRUCTURED_CLONE_VERSION,
                              aValue, &callbacks, &aCloneReadInfo)) {
    return false;
  }

  return true;
}

// static
bool
IDBObjectStore::DeserializeIndexValue(JSContext* aCx,
                                      StructuredCloneReadInfo& aCloneReadInfo,
                                      JS::MutableHandle<JS::Value> aValue)
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(aCx);

  if (aCloneReadInfo.mData.IsEmpty()) {
    aValue.setUndefined();
    return true;
  }

  size_t dataLen = aCloneReadInfo.mData.Length();

  uint64_t* data =
    const_cast<uint64_t*>(reinterpret_cast<uint64_t*>(
      aCloneReadInfo.mData.Elements()));

  MOZ_ASSERT(!(dataLen % sizeof(*data)));

  JSAutoRequest ar(aCx);

  static JSStructuredCloneCallbacks callbacks = {
    CommonStructuredCloneReadCallback<IndexDeserializationHelper>,
    nullptr,
    nullptr
  };

  if (!JS_ReadStructuredClone(aCx, data, dataLen, JS_STRUCTURED_CLONE_VERSION,
                              aValue, &callbacks, &aCloneReadInfo)) {
    return false;
  }

  return true;
}

// static
void
IDBObjectStore::ConvertActorsToBlobs(
                                   const nsTArray<PBlobChild*>& aActors,
                                   nsTArray<StructuredCloneFile>& aFiles)
{
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aFiles.IsEmpty(), "Should be empty!");

  if (!aActors.IsEmpty()) {
    NS_ASSERTION(ContentChild::GetSingleton(), "This should never be null!");

    uint32_t length = aActors.Length();
    aFiles.SetCapacity(length);

    for (uint32_t index = 0; index < length; index++) {
      BlobChild* actor = static_cast<BlobChild*>(aActors[index]);

      StructuredCloneFile* file = aFiles.AppendElement();
      file->mFile = actor->GetBlob();
    }
  }
}

// static
nsresult
IDBObjectStore::ConvertBlobsToActors(
                                    nsIContentParent* aContentParent,
                                    FileManager* aFileManager,
                                    const nsTArray<StructuredCloneFile>& aFiles,
                                    nsTArray<PBlobParent*>& aActors)
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aContentParent, "Null contentParent!");
  NS_ASSERTION(aFileManager, "Null file manager!");

  if (!aFiles.IsEmpty()) {
    nsCOMPtr<nsIFile> directory = aFileManager->GetDirectory();
    if (!directory) {
      IDB_WARNING("Failed to get directory!");
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    uint32_t fileCount = aFiles.Length();
    aActors.SetCapacity(fileCount);

    for (uint32_t index = 0; index < fileCount; index++) {
      const StructuredCloneFile& file = aFiles[index];
      NS_ASSERTION(file.mFileInfo, "This should never be null!");

      nsCOMPtr<nsIFile> nativeFile =
        aFileManager->GetFileForId(directory, file.mFileInfo->Id());
      if (!nativeFile) {
        IDB_WARNING("Failed to get file!");
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      nsCOMPtr<nsIDOMBlob> blob = DOMFile::CreateFromFile(nativeFile,
                                                          file.mFileInfo);

      BlobParent* actor =
        aContentParent->GetOrCreateActorForBlob(blob);
      if (!actor) {
        // This can only fail if the child has crashed.
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      aActors.AppendElement(actor);
    }
  }

  return NS_OK;
}

#ifdef DEBUG

void
IDBObjectStore::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();
}

#endif // DEBUG

nsresult
IDBObjectStore::GetAddInfo(JSContext* aCx,
                           JS::Handle<JS::Value> aValue,
                           JS::Handle<JS::Value> aKeyVal,
                           StructuredCloneWriteInfo& aCloneWriteInfo,
                           Key& aKey,
                           nsTArray<IndexUpdateInfo>& aUpdateInfoArray)
{
  // Return DATA_ERR if a key was passed in and this objectStore uses inline
  // keys.
  if (!aKeyVal.isUndefined() && HasValidKeyPath()) {
    return NS_ERROR_DOM_INDEXEDDB_DATA_ERR;
  }

  bool isAutoIncrement = AutoIncrement();

  nsresult rv;

  if (!HasValidKeyPath()) {
    // Out-of-line keys must be passed in.
    rv = aKey.SetFromJSVal(aCx, aKeyVal);
    if (NS_FAILED(rv)) {
      return rv;
    }
  } else if (!isAutoIncrement) {
    rv = GetKeyPath().ExtractKey(aCx, aValue, aKey);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  // Return DATA_ERR if no key was specified this isn't an autoIncrement
  // objectStore.
  if (aKey.IsUnset() && !isAutoIncrement) {
    return NS_ERROR_DOM_INDEXEDDB_DATA_ERR;
  }

  // Figure out indexes and the index values to update here.
  const nsTArray<IndexMetadata>& indexes = mSpec->indexes();

  const uint32_t idxCount = indexes.Length();
  aUpdateInfoArray.SetCapacity(idxCount); // Pretty good estimate

  for (uint32_t idxIndex = 0; idxIndex < idxCount; idxIndex++) {
    const IndexMetadata& metadata = indexes[idxIndex];

    rv = AppendIndexUpdateInfo(metadata.id(), metadata.keyPath(),
                               metadata.unique(), metadata.multiEntry(), aCx,
                               aValue, aUpdateInfoArray);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  GetAddInfoClosure data(aCloneWriteInfo, aValue);

  if (isAutoIncrement && HasValidKeyPath()) {
    MOZ_ASSERT(aKey.IsUnset());

    rv = GetKeyPath().ExtractOrCreateKey(aCx,
                                         aValue,
                                         aKey,
                                         &GetAddInfoCallback,
                                         &data);
  } else {
    rv = GetAddInfoCallback(aCx, &data);
  }

  return rv;
}

already_AddRefed<IDBRequest>
IDBObjectStore::AddOrPut(JSContext* aCx,
                         JS::Handle<JS::Value> aValue,
                         JS::Handle<JS::Value> aKey,
                         bool aOverwrite,
                         ErrorResult& aRv)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aCx);

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  JS::Rooted<JS::Value> value(aCx, aValue);
  Key key;
  StructuredCloneWriteInfo cloneWriteInfo;
  nsTArray<IndexUpdateInfo> updateInfo;

  aRv = GetAddInfo(aCx, value, aKey, cloneWriteInfo, key, updateInfo);
  if (aRv.Failed()) {
    return nullptr;
  }

  FallibleTArray<uint8_t> cloneData;
  if (NS_WARN_IF(!cloneData.SetLength(cloneWriteInfo.mCloneBuffer.nbytes()))) {
    aRv = NS_ERROR_OUT_OF_MEMORY;
    return nullptr;
  }

  // XXX Remove this
  memcpy(cloneData.Elements(), cloneWriteInfo.mCloneBuffer.data(),
         cloneWriteInfo.mCloneBuffer.nbytes());

  cloneWriteInfo.mCloneBuffer.clear();

  ObjectStoreAddPutParams commonParams;
  commonParams.objectStoreId() = Id();
  commonParams.cloneInfo().data().SwapElements(cloneData);
  commonParams.cloneInfo().offsetToKeyProp() = cloneWriteInfo.mOffsetToKeyProp;
  commonParams.key() = key;
  commonParams.indexUpdateInfos().SwapElements(updateInfo);

  // Convert any blobs into PBackgroundIDBDatabaseFiles.
  const nsTArray<nsCOMPtr<nsIDOMBlob>>& blobs = cloneWriteInfo.mFiles;

  if (!blobs.IsEmpty()) {
    const uint32_t count = blobs.Length();

    FallibleTArray<PBackgroundIDBDatabaseFileChild*> fileActors;
    if (NS_WARN_IF(!fileActors.SetCapacity(count))) {
      aRv = NS_ERROR_OUT_OF_MEMORY;
      return nullptr;
    }

    for (uint32_t index = 0; index < count; index++) {
      const nsCOMPtr<nsIDOMBlob>& blob = blobs[index];
      MOZ_ASSERT(blob);

      PBackgroundIDBDatabaseFileChild* fileActor =
        mTransaction->Database()->GetOrCreateFileActorForBlob(blob);
      if (NS_WARN_IF(!fileActor)) {
        IDB_REPORT_INTERNAL_ERR();
        aRv = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
        return nullptr;
      }

      fileActors.AppendElement(fileActor);
    }

    commonParams.filesChild().SwapElements(fileActors);
  }

  RequestParams params;
  if (aOverwrite) {
    params = ObjectStorePutParams(commonParams);
  } else {
    params = ObjectStoreAddParams(commonParams);
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

#ifdef IDB_PROFILER_USE_MARKS
  if (aOverwrite) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).%s(%s)",
                      "IDBRequest[%llu] MT IDBObjectStore.put()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  }
  else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).add(%s)",
                      "IDBRequest[%llu] MT IDBObjectStore.add()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  }
#endif

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::GetAllInternal(bool aKeysOnly,
                               JSContext* aCx,
                               JS::Handle<JS::Value> aKey,
                               const Optional<uint32_t>& aLimit,
                               ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  const int64_t id = Id();

  OptionalKeyRange optionalKeyRange;
  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);
    optionalKeyRange = serializedKeyRange;
  } else {
    optionalKeyRange = void_t();
  }

  const uint32_t limit = aLimit.WasPassed() ? aLimit.Value() : 0;

  RequestParams params;
  if (aKeysOnly) {
    params = ObjectStoreGetAllKeysParams(id, optionalKeyRange, limit);
  } else {
    params = ObjectStoreGetAllParams(id, optionalKeyRange, limit);
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

#ifdef IDB_PROFILER_USE_MARKS
  if (aKeysOnly) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s)."
                      "getAllKeys(%s, %lu)",
                      "IDBRequest[%llu] MT IDBObjectStore.getAllKeys()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      IDB_PROFILER_STRING(aKeyRange),
                      aLimit);
  } else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s)."
                      "getAll(%s, %lu)",
                      "IDBRequest[%llu] MT IDBObjectStore.getAll()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      IDB_PROFILER_STRING(aKeyRange),
                      aLimit);
  }
#endif

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::Clear(ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  ObjectStoreClearParams params;
  params.objectStoreId() = Id();

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).clear()",
                    "IDBRequest[%llu] MT IDBObjectStore.clear()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this));

  return request.forget();
}

already_AddRefed<IDBIndex>
IDBObjectStore::Index(const nsAString& aName, ErrorResult &aRv)
{
  AssertIsOnOwningThread();

  if (mTransaction->IsFinished()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  const nsTArray<IndexMetadata>& indexes = mSpec->indexes();

  const IndexMetadata* metadata = nullptr;

  for (uint32_t idxCount = indexes.Length(), idxIndex = 0;
       idxIndex < idxCount;
       idxIndex++) {
    const IndexMetadata& index = indexes[idxIndex];
    if (index.name() == aName) {
      metadata = &index;
      break;
    }
  }

  if (!metadata) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return nullptr;
  }

  const int64_t desiredId = metadata->id();

  nsRefPtr<IDBIndex> index;

  for (uint32_t idxCount = mIndexes.Length(), idxIndex = 0;
       idxIndex < idxCount;
       idxIndex++) {
    nsRefPtr<IDBIndex>& existingIndex = mIndexes[idxIndex];

    if (existingIndex->Id() == desiredId) {
      index = existingIndex;
      break;
    }
  }

  if (!index) {
    index = IDBIndex::Create(this, *metadata);
    MOZ_ASSERT(index);

    mIndexes.AppendElement(index);
  }

  return index.forget();
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBObjectStore)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mCachedKeyPath)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTransaction)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mIndexes);
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER

  // Don't unlink mTransaction!

  NS_IMPL_CYCLE_COLLECTION_UNLINK(mIndexes);

  tmp->mCachedKeyPath = JSVAL_VOID;

  if (tmp->mRooted) {
    mozilla::DropJSObjects(tmp);
    tmp->mRooted = false;
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(IDBObjectStore)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(IDBObjectStore)
NS_IMPL_CYCLE_COLLECTING_RELEASE(IDBObjectStore)

JSObject*
IDBObjectStore::WrapObject(JSContext* aCx)
{
  return IDBObjectStoreBinding::Wrap(aCx, this);
}

nsPIDOMWindow*
IDBObjectStore::GetParentObject() const
{
  return mTransaction->GetParentObject();
}

void
IDBObjectStore::GetKeyPath(JSContext* aCx,
                           JS::MutableHandle<JS::Value> aResult,
                           ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!mCachedKeyPath.isUndefined()) {
    JS::ExposeValueToActiveJS(mCachedKeyPath);
    aResult.set(mCachedKeyPath);
    return;
  }

  aRv = GetKeyPath().ToJSVal(aCx, mCachedKeyPath);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }

  if (mCachedKeyPath.isGCThing()) {
    mozilla::HoldJSObjects(this);
    mRooted = true;
  }

  JS::ExposeValueToActiveJS(mCachedKeyPath);
  aResult.set(mCachedKeyPath);
}

already_AddRefed<DOMStringList>
IDBObjectStore::IndexNames()
{
  AssertIsOnOwningThread();

  const nsTArray<IndexMetadata>& indexes = mSpec->indexes();

  nsRefPtr<DOMStringList> list = new DOMStringList();

  if (!indexes.IsEmpty()) {
    nsTArray<nsString>& listNames = list->StringArray();
    listNames.SetCapacity(indexes.Length());

    for (uint32_t index = 0; index < indexes.Length(); index++) {
      listNames.InsertElementSorted(indexes[index].name());
    }
  }

  return list.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::Get(JSContext* aCx,
                    JS::Handle<JS::Value> aKey,
                    ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (aRv.Failed()) {
    return nullptr;
  }

  if (!keyRange) {
    // Must specify a key or keyRange for get().
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return nullptr;
  }

  ObjectStoreGetParams params;
  params.objectStoreId() = Id();
  keyRange->ToSerialized(params.keyRange());

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).get(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.get()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange));

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::Delete(JSContext* aCx,
                       JS::Handle<JS::Value> aKey,
                       ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (NS_WARN_IF((aRv.Failed()))) {
    return nullptr;
  }

  if (!keyRange) {
    // Must specify a key or keyRange for delete().
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return nullptr;
  }

  ObjectStoreDeleteParams params;
  params.objectStoreId() = Id();
  keyRange->ToSerialized(params.keyRange());

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).delete(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.delete()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange));

  return request.forget();
}

already_AddRefed<IDBIndex>
IDBObjectStore::CreateIndex(JSContext* aCx,
                            const nsAString& aName,
                            const nsAString& aKeyPath,
                            const IDBIndexParameters& aOptionalParameters,
                            ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  KeyPath keyPath(0);
  if (NS_FAILED(KeyPath::Parse(aCx, aKeyPath, &keyPath)) ||
      !keyPath.IsValid()) {
    aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return nullptr;
  }

  return CreateIndexInternal(aCx, aName, keyPath, aOptionalParameters, aRv);
}

already_AddRefed<IDBIndex>
IDBObjectStore::CreateIndex(JSContext* aCx,
                            const nsAString& aName,
                            const Sequence<nsString >& aKeyPath,
                            const IDBIndexParameters& aOptionalParameters,
                            ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  KeyPath keyPath(0);
  if (aKeyPath.IsEmpty() ||
      NS_FAILED(KeyPath::Parse(aCx, aKeyPath, &keyPath)) ||
      !keyPath.IsValid()) {
    aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return nullptr;
  }

  return CreateIndexInternal(aCx, aName, keyPath, aOptionalParameters, aRv);
}

already_AddRefed<IDBIndex>
IDBObjectStore::CreateIndexInternal(
                                  JSContext* aCx,
                                  const nsAString& aName,
                                  const KeyPath& aKeyPath,
                                  const IDBIndexParameters& aOptionalParameters,
                                  ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = IDBTransaction::GetCurrent();

  if (!transaction ||
      transaction != mTransaction ||
      mTransaction->GetMode() != IDBTransaction::VERSION_CHANGE) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  MOZ_ASSERT(transaction->IsOpen());

  auto& indexes = const_cast<nsTArray<IndexMetadata>&>(mSpec->indexes());
  for (uint32_t count = indexes.Length(), index = 0;
       index < count;
       index++) {
    if (aName == indexes[index].name()) {
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_CONSTRAINT_ERR);
      return nullptr;
    }
  }

  if (aOptionalParameters.mMultiEntry && aKeyPath.IsArray()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_ACCESS_ERR);
    return nullptr;
  }

#ifdef DEBUG
  for (uint32_t count = mIndexes.Length(), index = 0;
       index < count;
       index++) {
    MOZ_ASSERT(mIndexes[index]->Name() != aName);
  }
#endif

  const IndexMetadata* oldMetadataElements =
    indexes.IsEmpty() ? nullptr : indexes.Elements();

  IndexMetadata* metadata = indexes.AppendElement(
    IndexMetadata(transaction->NextIndexId(), nsString(aName), aKeyPath,
                  aOptionalParameters.mUnique,
                  aOptionalParameters.mMultiEntry));

  if (oldMetadataElements &&
      oldMetadataElements != indexes.Elements()) {
    MOZ_ASSERT(indexes.Length() > 1);

    // Array got moved, update the spec pointers for all live indexes.
    RefreshSpec(/* aMayDelete */ false);
  }

  transaction->CreateIndex(this, *metadata);

  nsRefPtr<IDBIndex> index = IDBIndex::Create(this, *metadata);
  MOZ_ASSERT(index);

  mIndexes.AppendElement(index);

  IDB_PROFILER_MARK("IndexedDB Pseudo-request: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "createIndex(%s)",
                    "MT IDBObjectStore.createIndex()",
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(index));

  return index.forget();
}

void
IDBObjectStore::DeleteIndex(const nsAString& aName, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = IDBTransaction::GetCurrent();

  if (!transaction ||
      transaction != mTransaction ||
      mTransaction->GetMode() != IDBTransaction::VERSION_CHANGE) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return;
  }

  MOZ_ASSERT(transaction->IsOpen());

  auto& metadataArray = const_cast<nsTArray<IndexMetadata>&>(mSpec->indexes());

  int64_t foundId = 0;

  for (uint32_t metadataCount = metadataArray.Length(), metadataIndex = 0;
       metadataIndex < metadataCount;
       metadataIndex++) {
    const IndexMetadata& metadata = metadataArray[metadataIndex];
    MOZ_ASSERT(metadata.id());

    if (aName == metadata.name()) {
      foundId = metadata.id();

      // Must do this before altering the metadata array!
      for (uint32_t indexCount = mIndexes.Length(), indexIndex = 0;
           indexIndex < indexCount;
           indexIndex++) {
        nsRefPtr<IDBIndex>& index = mIndexes[indexIndex];

        if (index->Id() == foundId) {
          index->NoteDeletion();
          mIndexes.RemoveElementAt(indexIndex);
          break;
        }
      }

      metadataArray.RemoveElementAt(metadataIndex);

      RefreshSpec(/* aMayDelete */ false);
      break;
    }
  }

  if (!foundId) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return;
  }

  transaction->DeleteIndex(this, foundId);

  IDB_PROFILER_MARK("IndexedDB Pseudo-request: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "deleteIndex(\"%s\")",
                    "MT IDBObjectStore.deleteIndex()",
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this),
                    NS_ConvertUTF16toUTF8(aName).get());
}

already_AddRefed<IDBRequest>
IDBObjectStore::Count(JSContext* aCx,
                      JS::Handle<JS::Value> aKey,
                      ErrorResult& aRv)
{
  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (aRv.Failed()) {
    return nullptr;
  }

  ObjectStoreCountParams params;
  params.objectStoreId() = Id();

  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);
    params.optionalKeyRange() = serializedKeyRange;
  } else {
    params.optionalKeyRange() = void_t();
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).count(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.count()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange));

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::OpenCursorInternal(bool aKeysOnly,
                                   JSContext* aCx,
                                   JS::Handle<JS::Value> aRange,
                                   IDBCursorDirection aDirection,
                                   ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aRange, getter_AddRefs(keyRange));
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  int64_t objectStoreId = Id();

  OptionalKeyRange optionalKeyRange;

  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);

    optionalKeyRange = Move(serializedKeyRange);
  } else {
    optionalKeyRange = void_t();
  }

  IDBCursor::Direction direction = IDBCursor::ConvertDirection(aDirection);

  OpenCursorParams params;
  if (aKeysOnly) {
    ObjectStoreOpenKeyCursorParams openParams;
    openParams.objectStoreId() = objectStoreId;
    openParams.optionalKeyRange() = Move(optionalKeyRange);
    openParams.direction() = direction;

    params = Move(openParams);
  } else {
    ObjectStoreOpenCursorParams openParams;
    openParams.objectStoreId() = objectStoreId;
    openParams.optionalKeyRange() = Move(optionalKeyRange);
    openParams.direction() = direction;

    params = Move(openParams);
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundCursorChild* actor =
    new BackgroundCursorChild(request, this, direction);

  mTransaction->OpenCursor(actor, params);

#ifdef IDB_PROFILER_USE_MARKS
  if (aKeysOnly) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s)."
                      "openKeyCursor(%s, %s)",
                      "IDBRequest[%llu] MT IDBObjectStore.openKeyCursor()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                      IDB_PROFILER_STRING(direction));
  } else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s)."
                      "openCursor(%s, %s)",
                      "IDBRequest[%llu] MT IDBObjectStore.openKeyCursor()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                      IDB_PROFILER_STRING(direction));
  }
#endif
  return request.forget();
}

void
IDBObjectStore::RefreshSpec(bool aMayDelete)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT_IF(mDeletedSpec, mSpec == mDeletedSpec);

  const DatabaseSpec* dbSpec = mTransaction->Database()->Spec();
  MOZ_ASSERT(dbSpec);

  const nsTArray<ObjectStoreSpec>& objectStores = dbSpec->objectStores();

  bool found = false;

  for (uint32_t objCount = objectStores.Length(), objIndex = 0;
       objIndex < objCount;
       objIndex++) {
    const ObjectStoreSpec& objSpec = objectStores[objIndex];

    if (objSpec.metadata().id() == Id()) {
      mSpec = &objSpec;

      for (uint32_t idxCount = mIndexes.Length(), idxIndex = 0;
           idxIndex < idxCount;
           idxIndex++) {
        mIndexes[idxIndex]->RefreshMetadata(aMayDelete);
      }

      found = true;
      break;
    }
  }

  MOZ_ASSERT_IF(!aMayDelete && !mDeletedSpec, found);

  if (found) {
    MOZ_ASSERT(mSpec != mDeletedSpec);
    mDeletedSpec = nullptr;
  } else {
    NoteDeletion();
  }
}

const ObjectStoreSpec&
IDBObjectStore::Spec() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return *mSpec;
}

void
IDBObjectStore::NoteDeletion()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);
  MOZ_ASSERT(Id() == mSpec->metadata().id());

  if (mDeletedSpec) {
    MOZ_ASSERT(mDeletedSpec == mSpec);
    return;
  }

  // Copy the spec here.
  mDeletedSpec = new ObjectStoreSpec(*mSpec);
  mDeletedSpec->indexes().Clear();

  mSpec = mDeletedSpec;

  if (!mIndexes.IsEmpty()) {
    for (uint32_t count = mIndexes.Length(), index = 0;
         index < count;
         index++) {
      mIndexes[index]->NoteDeletion();
    }
  }
}

const nsString&
IDBObjectStore::Name() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().name();
}

bool
IDBObjectStore::AutoIncrement() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().autoIncrement();
}

const KeyPath&
IDBObjectStore::GetKeyPath() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().keyPath();
}

bool
IDBObjectStore::HasValidKeyPath() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return GetKeyPath().IsValid();
}

