package com.protore.game;

import android.content.Context;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.view.ViewGroup;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** SDL's activity running the native port (libmain.so: the recompiled engine and its platform layer). */
public class ProtoActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "FAudio", "dxvk_d3d9", modernCpu() ? "main" : "main_v80"};
    }

    /**
     * libmain.so is built for ARMv8.2 with RCpc (LDAPR keeps the emulated x86 memory ordering cheap) and LSE atomics;
     * older cores (Cortex-A53/A73, e.g. Snapdragon 6xx) get libmain_v80.so, built for ARMv8.0. Every core must have
     * both features: big.LITTLE phones list each core's features separately.
     */
    private static boolean modernCpu() {
        int cores = 0;
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            for (String line; (line = r.readLine()) != null;) {
                if (!line.startsWith("Features")) continue;
                List<String> features = Arrays.asList(line.substring(line.indexOf(':') + 1).trim().split("\\s+"));
                if (!features.contains("atomics") || !features.contains("lrcpc")) return false;
                ++cores;
            }
        } catch (IOException e) {
            return false;
        }
        return cores > 0;
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new ProtoSurface(context);
    }

    /**
     * The engine runs once per process: a new start of the game gets a fresh process. SDL's onDestroy waits on the UI
     * thread for the game thread to return, which the engine's shutdown can make long enough for an ANR, so a
     * finishing activity ends the process before that.
     */
    @Override
    protected void onDestroy() {
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
        super.onDestroy();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The native side loads the bundled Vulkan driver (Turnip, on Adreno) from the extracted libraries.
        try {
            Os.setenv("PROTORE_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
        } catch (ErrnoException ignored) {
            // the system Vulkan driver is used
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);  // no dimming while playing
        // On-screen controls above SDL's surface; they feed SDL keyboard and mouse input.
        addContentView(new TouchControls(this), new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }
}
