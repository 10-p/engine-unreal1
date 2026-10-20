package com.unreal1.android;

import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Bundle;
import android.util.Log;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity, in the ":game" process (the engine starts once per process, so the process
 * ends with the game, {@link #onDestroy}). Before SDL starts SDL_main, the game folder chosen in the
 * launcher gets the APK's System files and becomes the engine's base directory (the JNI bridge sets the
 * environment and the working directory). Over SDL's surface lies the on-screen pad
 * ({@link PadOverlayView}), which drives a virtual SDL controller in the engine (NSDLDrv/Src/NSDLAndroid.cpp).
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "Unreal";

    /** Which game folder (the launcher's choice): {@link #MODE_INTERNAL} or {@link #MODE_EXTERNAL}. */
    static final String EXTRA_STORAGE_MODE = "com.unreal1.android.STORAGE_MODE";
    static final String MODE_INTERNAL = "internal";
    static final String MODE_EXTERNAL = "external";

    /**
     * An engine command line, whitespace-separated, given to the launcher (GameActivity is not exported)
     * and passed on, the desktop's command line: a map URL first, then switches, e.g.
     *
     *   adb shell am start -n com.unreal1.android/.MainActivity --es com.unreal1.android.ARGS 'DmDeck16 -nosound'
     *
     * The launcher's countdown then starts the game without a touch.
     */
    static final String EXTRA_ARGS = "com.unreal1.android.ARGS";

    /**
     * The frame's size as a multiple of the screen's (the launcher's resolution scale): below 1 fewer
     * pixels than the screen, above 1 more (supersampling). The surface SDL draws to gets that size
     * ({@link android.view.SurfaceHolder#setFixedSize}) and the compositor scales it to the screen, so the
     * engine sees a window of that size and nothing else changes.
     */
    static final String EXTRA_RENDER_SCALE = "com.unreal1.android.RENDER_SCALE";

    private AlertDialog exitDialog;

    private static boolean bridgeLoaded = false;

    static {
        // The bridge first, on its own: nativePrepareProcess runs before SDL loads its libraries.
        try {
            System.loadLibrary("unreal1_android_bridge");
            bridgeLoaded = true;
        } catch (Throwable t) {
            Log.e(TAG, "could not load unreal1_android_bridge", t);
        }
    }

    // android_bridge.cpp.
    private static native boolean nativePrepareProcess(String dataRoot, String homeDir);

    // The on-screen pad (NSDLDrv/Src/NSDLAndroid.cpp, in libUnreal.so).
    static native void nativeTouchPadButton(int button, boolean down);
    static native void nativeTouchPadAxis(int axis, float value);
    static native int nativeTouchPadState();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before super.onCreate, which starts the SDL thread and with it SDL_main.
        File root = resolveGameFolder();
        GameFolder.installSystemFiles(this, root);
        File home = new File(getFilesDir(), "home");
        if (bridgeLoaded) {
            boolean ok = nativePrepareProcess(root.getAbsolutePath(), home.getAbsolutePath());
            Log.i(TAG, "game folder " + root + (ok ? "" : ": the process set-up failed"));
        } else {
            Log.e(TAG, "no bridge: the engine will not find the game folder " + root);
        }
        super.onCreate(savedInstanceState);

        // A level can run for long without a touch (a pad, a flyby).
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // The whole screen is the game's: the window draws under the system bars, with no bar backgrounds of its
        // own. Without FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS the window of this legacy theme paints a black band over
        // the navigation bar's place (the home line) even while SDL's immersive mode hides the bar.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarContrastEnforced(false);
        getWindow().setDecorFitsSystemWindows(false);

        applyRenderScale();

        try {
            addContentView(new PadOverlayView(this), new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        } catch (Throwable t) {
            Log.e(TAG, "could not add the on-screen pad", t);
        }
    }

    /**
     * The surface at the screen's landscape size times the launcher's resolution scale (0.1 to 4; each side at
     * most 8192, the width a multiple of 4 as the engine aligns it, at least 320 x 240). At 1 the surface is left
     * at the screen's own size.
     */
    private void applyRenderScale() {
        float scale = getIntent() != null ? getIntent().getFloatExtra(EXTRA_RENDER_SCALE, 1f) : 1f;
        if (scale <= 0f || Math.abs(scale - 1f) < 0.001f || mSurface == null) return;
        scale = Math.max(0.1f, Math.min(4f, scale));
        Rect b = getWindowManager().getMaximumWindowMetrics().getBounds();
        int sw = Math.max(b.width(), b.height()), sh = Math.min(b.width(), b.height());
        scale = Math.min(scale, 8192f / Math.max(sw, sh));
        int w = Math.max(320, Math.round(sw * scale)) & ~3, h = Math.max(240, Math.round(sh * scale));
        Log.i(TAG, "resolution scale " + scale + ": the surface is " + w + "x" + h + " for the " + sw + "x" + sh + " screen");
        mSurface.getHolder().setFixedSize(w, h);
    }

    /**
     * The launcher's choice; with none (the game process started again from the recent apps), the
     * internal copy if there is one. An internal choice without an internal copy runs from shared storage.
     */
    private File resolveGameFolder() {
        String mode = getIntent() != null ? getIntent().getStringExtra(EXTRA_STORAGE_MODE) : null;
        File internal = GameFolder.internalRoot(this);
        if (MODE_EXTERNAL.equals(mode)) return GameFolder.externalRoot(this);
        if (GameFolder.isGameFolder(internal)) return internal;
        if (MODE_INTERNAL.equals(mode)) Log.w(TAG, "no internal copy at " + internal + ": running from shared storage");
        return GameFolder.externalRoot(this);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "Unreal" };
    }

    @Override
    protected String[] getArguments() {
        // The engine writes its ini, its log and the saves into the game folder, as on a desktop. The command
        // line from adb is the whole command line (a map URL must be the first argument).
        ArrayList<String> args = new ArrayList<String>();
        String extra = getIntent() != null ? getIntent().getStringExtra(EXTRA_ARGS) : null;
        if (extra != null && !extra.trim().isEmpty()) args.addAll(Arrays.asList(extra.trim().split("\\s+")));
        Log.i(TAG, "engine arguments: " + args);
        return args.toArray(new String[0]);
    }

    /**
     * Called by the engine (NSDLDrv's AskToExit) when Back has nothing left to go back to: the usual
     * Android confirmation. Exit quits through SDL, so the engine saves its settings and shuts down.
     */
    public void showExitDialog() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (exitDialog != null && exitDialog.isShowing()) return;
                exitDialog = new AlertDialog.Builder(GameActivity.this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                        .setTitle("Exit game?")
                        .setNegativeButton("Cancel", null)
                        .setPositiveButton("Exit", new android.content.DialogInterface.OnClickListener() {
                            @Override public void onClick(android.content.DialogInterface d, int which) {
                                Log.i(TAG, "exit confirmed");
                                SDLActivity.nativeSendQuit();
                            }
                        })
                        .create();
                exitDialog.show();
                // A pad's confirm button lands on Cancel, as console exit prompts do; Exit is one step away.
                android.widget.Button cancel = exitDialog.getButton(AlertDialog.BUTTON_NEGATIVE);
                if (cancel != null) cancel.requestFocus();
            }
        });
    }

    @Override
    protected void onDestroy() {
        // SDLActivity waits for SDL_main to return (the engine saved its settings and shut down).
        super.onDestroy();
        if (isFinishing()) {
            Log.i(TAG, "the game ended: ending its process");
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    }
}
