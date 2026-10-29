package com.unreal1.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.RelativeLayout;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

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
     * engine sees a window of that size; touches reach SDL in the surface's pixels ({@link FrameSurface}).
     */
    static final String EXTRA_RENDER_SCALE = "com.unreal1.android.RENDER_SCALE";

    /**
     * The on-screen pad's move stick (the launcher's settings), in dp: full tilt at this drag from its base, 4 to 60,
     * and the dead zone for a resting finger, 0 to 10 and below full tilt. Absent: the defaults.
     */
    static final String EXTRA_MOVE_FULL_TILT_DP = "com.unreal1.android.MOVE_FULL_TILT_DP";
    static final String EXTRA_MOVE_DEAD_ZONE_DP = "com.unreal1.android.MOVE_DEAD_ZONE_DP";
    static final float MOVE_FULL_TILT_DP = 12f, MOVE_FULL_TILT_MIN_DP = 4f, MOVE_FULL_TILT_MAX_DP = 60f;
    static final float MOVE_DEAD_ZONE_DP = 3f, MOVE_DEAD_ZONE_MAX_DP = 10f;

    static float moveFullTilt(float dp) {
        return Math.max(MOVE_FULL_TILT_MIN_DP, Math.min(MOVE_FULL_TILT_MAX_DP, dp));
    }

    /** A dead zone in its range, and at least 1 dp below full tilt. */
    static float moveDeadZone(float dp, float fullTiltDp) {
        return Math.max(0f, Math.min(Math.min(MOVE_DEAD_ZONE_MAX_DP, fullTiltDp - 1f), dp));
    }

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
    // Player one's pad from the next frame: the on-screen pad (true) or the first real pad.
    static native void nativeTouchPadWant(boolean touch);
    // Gyro aiming (GyroAim): the view's turn in degrees, to the right and up, which the engine adds up and applies in play.
    static native void nativeGyroLook(float yawDegrees, float pitchDegrees);

    private PadOverlayView pad;
    private GyroAim gyro;

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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) getWindow().setNavigationBarContrastEnforced(false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) getWindow().setDecorFitsSystemWindows(false);

        applyRenderScale();

        try {
            Intent in = getIntent();
            float fullTilt = moveFullTilt(in != null ? in.getFloatExtra(EXTRA_MOVE_FULL_TILT_DP, MOVE_FULL_TILT_DP) : MOVE_FULL_TILT_DP);
            float deadZone = moveDeadZone(in != null ? in.getFloatExtra(EXTRA_MOVE_DEAD_ZONE_DP, MOVE_DEAD_ZONE_DP) : MOVE_DEAD_ZONE_DP,
                    fullTilt);
            Log.i(TAG, String.format(Locale.US, "on-screen pad's move stick: full tilt %.1f dp, dead zone %.1f dp", fullTilt, deadZone));
            pad = new PadOverlayView(this, fullTilt, deadZone);
            addContentView(pad, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        } catch (Throwable t) {
            Log.e(TAG, "could not add the on-screen pad", t);
        }
        gyro = GyroAim.fromIntent(this);
    }

    // The gyro is read while the game is in front.
    @Override
    protected void onResume() {
        super.onResume();
        if (gyro != null) gyro.start();
    }

    @Override
    protected void onPause() {
        if (gyro != null) gyro.stop();
        super.onPause();
    }

    /** SDL's surface, whose pointer events SDL reads in the buffer's pixels at every resolution scale. */
    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new FrameSurface(context);
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
        Rect b = maximumWindowBounds(this);
        int sw = Math.max(b.width(), b.height()), sh = Math.min(b.width(), b.height());
        scale = Math.min(scale, 8192f / Math.max(sw, sh));
        int w = Math.max(320, Math.round(sw * scale)) & ~3, h = Math.max(240, Math.round(sh * scale));
        Log.i(TAG, "resolution scale " + scale + ": the surface is " + w + "x" + h + " for the " + sw + "x" + sh + " screen");
        // SDL adds its surface view to its layout without layout parameters, and a SurfaceView with a fixed-size
        // surface then measures to that size; filling the parent keeps the view the screen's size, so the compositor
        // scales the fixed-size buffer to it.
        mSurface.setLayoutParams(new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
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

    // A real pad's input hides the on-screen pad (PadOverlayView.onRealPadInput); SDL still gets every event.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (pad != null && isPadPress(event)) pad.onRealPadInput();
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (pad != null && isPadPush(event)) pad.onRealPadInput();
        return super.dispatchGenericMotionEvent(event);
    }

    /** A game pad's button going down (a pad's own source, or a gamepad key code from any source). */
    static boolean isPadPress(KeyEvent e) {
        if (e.getAction() != KeyEvent.ACTION_DOWN || e.getRepeatCount() != 0) return false;
        return e.isFromSource(InputDevice.SOURCE_GAMEPAD) || e.isFromSource(InputDevice.SOURCE_JOYSTICK)
                || KeyEvent.isGamepadButton(e.getKeyCode());
    }

    private static final int[] PAD_AXES = {
        MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RX, MotionEvent.AXIS_RY,
        MotionEvent.AXIS_RZ, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_LTRIGGER,
        MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS, MotionEvent.AXIS_BRAKE,
    };

    /** A pad's stick, trigger or hat pushed at least half way (a resting stick's drift stays below). */
    static boolean isPadPush(MotionEvent e) {
        if (!e.isFromSource(InputDevice.SOURCE_JOYSTICK) || e.getActionMasked() != MotionEvent.ACTION_MOVE) return false;
        for (int axis : PAD_AXES) if (Math.abs(e.getAxisValue(axis)) >= 0.5f) return true;
        return false;
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
                // SDL reads keys through its surface view's key listener, and the dialog takes the focus from it:
                // the view gets it back when the dialog goes, or the next Back would stop at the activity.
                exitDialog.setOnDismissListener(new android.content.DialogInterface.OnDismissListener() {
                    @Override public void onDismiss(android.content.DialogInterface d) {
                        if (mSurface != null) mSurface.requestFocus();
                    }
                });
                // The pad's A activates the focused button (Cancel unless the D-pad moved to Exit) and B cancels:
                // Android's own handling of a pad's buttons on a dialog depends on the device's key maps.
                exitDialog.setOnKeyListener(new android.content.DialogInterface.OnKeyListener() {
                    @Override public boolean onKey(android.content.DialogInterface d, int keyCode, android.view.KeyEvent event) {
                        boolean confirm = keyCode == android.view.KeyEvent.KEYCODE_BUTTON_A
                                || keyCode == android.view.KeyEvent.KEYCODE_DPAD_CENTER
                                || keyCode == android.view.KeyEvent.KEYCODE_ENTER;
                        boolean cancel = keyCode == android.view.KeyEvent.KEYCODE_BUTTON_B;
                        boolean left = keyCode == android.view.KeyEvent.KEYCODE_DPAD_LEFT;
                        boolean right = keyCode == android.view.KeyEvent.KEYCODE_DPAD_RIGHT;
                        if (!confirm && !cancel && !left && !right) return false;
                        if (event.getAction() != android.view.KeyEvent.ACTION_UP) return true;
                        if (cancel) { exitDialog.cancel(); return true; }
                        if (left || right) {
                            android.widget.Button b = exitDialog.getButton(left ? AlertDialog.BUTTON_NEGATIVE : AlertDialog.BUTTON_POSITIVE);
                            if (b != null) b.requestFocus();
                            return true;
                        }
                        android.view.View focused = exitDialog.getCurrentFocus();
                        android.widget.Button target = focused instanceof android.widget.Button
                                ? (android.widget.Button) focused : exitDialog.getButton(AlertDialog.BUTTON_NEGATIVE);
                        if (target != null) target.performClick();
                        return true;
                    }
                });
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

    /** The largest window's bounds: WindowMetrics from Android 11, the display's real size before. */
    static Rect maximumWindowBounds(Activity a) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return a.getWindowManager().getMaximumWindowMetrics().getBounds();
        Point p = new Point();
        a.getWindowManager().getDefaultDisplay().getRealSize(p);
        return new Rect(0, 0, p.x, p.y);
    }
}
