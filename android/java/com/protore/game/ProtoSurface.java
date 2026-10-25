package com.protore.game;

import android.content.Context;
import android.view.SurfaceHolder;

import org.libsdl.app.SDLSurface;

/**
 * SDL's surface, telling the native D3D9 bridge when the window goes away: the engine's render thread presents on
 * its own schedule, and a present (or a swap chain recreation) on a destroyed window crashes in the Vulkan loader.
 */
public class ProtoSurface extends SDLSurface {
    static native void nativeSurfaceGone();   // waits for a present in flight, then holds presents back
    static native void nativeSurfaceReady();

    public ProtoSurface(Context context) {
        super(context);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceGone();
        super.surfaceDestroyed(holder);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        super.surfaceChanged(holder, format, width, height);
        nativeSurfaceReady();
    }
}
