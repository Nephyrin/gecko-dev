/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
// vim:set ts=2 sts=2 sw=2 et cin:
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef AndroidSurfaceTexture_h__
#define AndroidSurfaceTexture_h__
#ifdef MOZ_WIDGET_ANDROID

#include <jni.h>
#include "nsIRunnable.h"
#include "gfxPlatform.h"
#include "GLDefs.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/Monitor.h"

#include "AndroidNativeWindow.h"

class gfxASurface;

namespace mozilla {
namespace gfx {
class Matrix4x4;
}
}

namespace mozilla {
namespace gl {

class GLContext;

/**
 * This class is a wrapper around Android's SurfaceTexture class.
 * Usage is pretty much exactly like the Java class, so see
 * the Android documentation for details.
 */
class AndroidSurfaceTexture {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(AndroidSurfaceTexture)

public:

  // The SurfaceTexture is created in an attached state. This method requires
  // Android Ice Cream Sandwich.
  static AndroidSurfaceTexture* Create(GLContext* aGLContext, GLuint aTexture);

  // Here the SurfaceTexture will be created in a detached state. You must call
  // Attach() with the GLContext you wish to composite with. It must be done
  // on the thread where that GLContext is current. This method requires
  // Android Jelly Bean.
  static AndroidSurfaceTexture* Create();

  static AndroidSurfaceTexture* Find(int id);

  // Returns with reasonable certainty whether or not we'll
  // be able to create and use a SurfaceTexture
  static bool Check();

  // If we are on Jelly Bean, the SurfaceTexture can be detached and reattached
  // to allow consumption from different GLContexts. It is recommended to only
  // attach while you are consuming in order to allow this.
  //
  // Only one GLContext may be attached at any given time. If another is already
  // attached, we try to wait for it to become detached.
  bool Attach(GLContext* aContext, PRIntervalTime aTiemout = PR_INTERVAL_NO_TIMEOUT);

  // This is a noop on ICS, and will always fail
  bool Detach();

  GLContext* GetAttachedContext() { return mAttachedContext; }

  AndroidNativeWindow* NativeWindow() {
    return mNativeWindow;
  }

  // This attaches the updated data to the TEXTURE_EXTERNAL target
  void UpdateTexImage();

  bool GetTransformMatrix(mozilla::gfx::Matrix4x4& aMatrix);
  int ID() { return mID; }

  void SetDefaultSize(mozilla::gfx::IntSize size);

  // The callback is guaranteed to be called on the main thread even
  // if the upstream callback is received on a different thread
  void SetFrameAvailableCallback(nsIRunnable* aRunnable);

  // Only should be called by AndroidJNI when we get a
  // callback from the underlying SurfaceTexture instance
  void NotifyFrameAvailable();

  GLuint Texture() { return mTexture; }
  jobject JavaSurface() { return mSurface; }
private:
  AndroidSurfaceTexture();
  ~AndroidSurfaceTexture();

  bool Init(GLContext* aContext, GLuint aTexture);

  GLuint mTexture;
  jobject mSurfaceTexture;
  jobject mSurface;

  Monitor mMonitor;
  GLContext* mAttachedContext;

  RefPtr<AndroidNativeWindow> mNativeWindow;
  int mID;
  nsRefPtr<nsIRunnable> mFrameAvailableCallback;
};

}
}


#endif
#endif
