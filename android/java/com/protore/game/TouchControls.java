package com.protore.game;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BlurMaskFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.view.HapticFeedbackConstants;
import android.os.Build;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * On-screen controls drawn over the game in the style of Prototype's interface (red neon on black, hexagons,
 * bracket corners). Everything goes into SDL as real keyboard and mouse input, so the engine sees it both as window
 * messages and through DirectInput.
 *
 * Modes (button in the top-left corner):
 *  - menu: a touch is a left mouse click at that point (menus are mouse-driven), plus Enter and Esc;
 *  - game: a stick on the left (WASD; optionally sprints at the rim), the action buttons in an arc around the
 *    big attack button under the right thumb, dragging anywhere else turns the camera.
 * The gear opens the settings panel; from there the layout editor lets the player drag every control to where
 * their thumbs rest. The performance overlay shows FPS, frame time and memory.
 */
public class TouchControls extends View implements InputManager.InputDeviceListener {
    // Performance counters and the frame limiter of the native host (main.cpp).
    static native float nativeFps();
    static native float nativeFrameMs();
    static native int nativeMemoryMiB();
    static native void nativeSetFpsLimit(int fps);

    private static final int MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 4;
    private static final int RED = 0xffe0261e, RED_DIM = 0xff7a1410, INK = 0xff0b0b0b;

    /**
     * A hexagonal button: a key (Android keycode) or a mouse button. Its default place is a dp offset from a corner
     * (right/bottom when the flags say so), scaled with the buttons so a cluster stays tight; the layout editor
     * replaces it with a position in fractions of the view.
     */
    private static final class Button {
        final String label;
        final int keycode, mouseButton;
        final boolean fromRight, fromBottom;
        final float dx, dy, sizeDp;
        final boolean gameOnly;
        float customX = -1, customY = -1;
        float size = 1;  // the layout editor's per-button size factor
        boolean quickPower;  // DISGUISE / ATK PWR / DEFENSE / SENSE: shown only with the "Power buttons" setting
        // POWERS and DISGUISE hold a selection wheel open: sliding the finger that holds the button moves the mouse,
        // which is how the game picks an entry, so one thumb is enough. fx/fy: where that finger is.
        final boolean wheel;
        float fx, fy;
        float cx, cy, r;
        int pointer = -1;

        Button(String label, int keycode, int mouseButton, boolean fromRight, float dx, boolean fromBottom, float dy,
                float sizeDp, boolean gameOnly) {
            this.label = label;
            this.keycode = keycode;
            this.mouseButton = mouseButton;
            this.fromRight = fromRight;
            this.dx = dx;
            this.fromBottom = fromBottom;
            this.dy = dy;
            this.sizeDp = sizeDp;
            this.gameOnly = gameOnly;
            this.quickPower = keycode == KeyEvent.KEYCODE_F || keycode == KeyEvent.KEYCODE_1
                    || keycode == KeyEvent.KEYCODE_3 || keycode == KeyEvent.KEYCODE_4;
            this.wheel = keycode == KeyEvent.KEYCODE_R || keycode == KeyEvent.KEYCODE_F;
        }

        boolean hit(float x, float y) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r * 1.25f;
        }
    }

    // Right thumb: ATTACK in the corner, the inner arc (radius ~105 dp around it) for SPECIAL / GRAB / JUMP, the
    // outer arc (~190 dp) for the rarer TARGET and SPRINT. Menu keys, POWERS and DISGUISE along the top right edge,
    // the quick powers below them, clear of the health bar.
    private final Button[] buttons = {
        new Button("ENTER", KeyEvent.KEYCODE_ENTER, 0, true, 280, false, 33, 24, false),
        new Button("ESC", KeyEvent.KEYCODE_ESCAPE, 0, true, 100, false, 33, 24, false),
        new Button("ATTACK", 0, MOUSE_LEFT, true, 78, true, 74, 46, true),
        new Button("SPECIAL", 0, MOUSE_RIGHT, true, 183, true, 62, 33, true),
        new Button("GRAB", KeyEvent.KEYCODE_E, 0, true, 158, true, 150, 31, true),
        new Button("JUMP", KeyEvent.KEYCODE_SPACE, 0, true, 66, true, 180, 33, true),
        new Button("TARGET", KeyEvent.KEYCODE_TAB, 0, true, 258, true, 140, 27, true),  // lock-on is Tab on PC
        new Button("SPRINT", KeyEvent.KEYCODE_SHIFT_LEFT, 0, true, 168, true, 245, 27, true),
        new Button("POWERS", KeyEvent.KEYCODE_R, 0, true, 190, false, 33, 24, true),  // Power Select Menu: R
        // Quick powers (PC keys from the game's Keyboard Setup): last disguise power menu, last attack, defense and
        // sensory power. Appended, so the saved positions of the buttons above keep their indices.
        new Button("DISGUISE", KeyEvent.KEYCODE_F, 0, true, 370, false, 33, 24, true),
        new Button("ATK PWR", KeyEvent.KEYCODE_1, 0, true, 280, false, 95, 22, true),
        new Button("DEFENSE", KeyEvent.KEYCODE_3, 0, true, 190, false, 95, 22, true),
        new Button("SENSE", KeyEvent.KEYCODE_4, 0, true, 100, false, 95, 22, true),
    };

    // ---- settings ----
    private final SharedPreferences prefs;
    private float opacity, scale;
    private boolean vibrate, showFps, hideWithPad, autoRun, invertY, powerButtons;
    private float camGain;
    private boolean padConnected;

    private static final class Setting {
        final String label, key;     // key: preference name; engine settings also go to protore.ini
        final String[] names;
        final int[] values;
        final boolean engine;        // read by the engine at start: needs a restart
        int index;

        Setting(String label, String key, String[] names, int[] values, boolean engine) {
            this.label = label;
            this.key = key;
            this.names = names;
            this.values = values;
            this.engine = engine;
        }

        int value() {
            return values[index];
        }
    }

    private final Setting[] settings = {
        new Setting("Resolution", "render_height", new String[] {"540p", "720p", "900p", "1080p"},
                new int[] {540, 720, 900, 1080}, true),
        new Setting("Textures", "texture_skip_mips", new String[] {"Full", "Half", "Quarter"},
                new int[] {0, 1, 2}, true),
        new Setting("FPS limit", "fps_limit", new String[] {"Off", "30", "45", "60"}, new int[] {0, 30, 45, 60},
                true),
        new Setting("Battery saver", "battery_saver", new String[] {"Off", "On"}, new int[] {0, 1}, true),
        new Setting("Opacity", "ui_opacity", new String[] {"40%", "60%", "80%", "100%"},
                new int[] {40, 60, 80, 100}, false),
        new Setting("Button size", "ui_scale", new String[] {"80%", "100%", "120%", "140%"},
                new int[] {80, 100, 120, 140}, false),
        new Setting("Vibration", "ui_vibrate", new String[] {"Off", "On"}, new int[] {0, 1}, false),
        new Setting("Show FPS", "ui_fps", new String[] {"Off", "On"}, new int[] {0, 1}, false),
        new Setting("Hide with gamepad", "ui_pad_hide", new String[] {"Off", "On"}, new int[] {0, 1}, false),
        new Setting("Stick edge sprint", "ui_stick_run", new String[] {"Off", "On"}, new int[] {0, 1}, false),
        new Setting("Camera speed", "ui_cam", new String[] {"50%", "75%", "100%", "150%", "200%"},
                new int[] {50, 75, 100, 150, 200}, false),
        new Setting("Invert Y", "ui_invert", new String[] {"Off", "On"}, new int[] {0, 1}, false),
        new Setting("Power buttons", "ui_power_buttons", new String[] {"Off", "On"}, new int[] {0, 1}, false),
    };
    private static final int[] DEFAULTS = {1, 1, 0, 0, 2, 1, 1, 0, 0, 0, 2, 0, 0};
    private boolean panelOpen = false, restartNeeded = false;
    private final RectF panel = new RectF(), restartRect = new RectF(), closeRect = new RectF(),
            layoutRect = new RectF();

    // ---- layout editor ----
    private boolean editing = false;
    private final RectF doneRect = new RectF(), resetRect = new RectF();
    private final RectF smallerRect = new RectF(), biggerRect = new RectF();
    private Button selected;          // the button (or stick) the size buttons act on
    private boolean selectedStick;
    private float stickSize = 1;
    private Button dragButton;
    private boolean dragStick;
    private int dragPointer = -1;
    private float dragDx, dragDy;
    private float stickCustomX = -1, stickCustomY = -1;
    private final List<RectF> leftArrows = new ArrayList<>(), rightArrows = new ArrayList<>();

    // ---- drawing ----
    private final float density;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG),
            glow = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private boolean gameMode = false;
    private final RectF modeRect = new RectF(), gearRect = new RectF();
    private int modePointer = -1, gearPointer = -1;

    // stick
    private float stickHomeX, stickHomeY, stickX, stickY, stickR, knobX, knobY;
    private int stickPointer = -1;
    private final boolean[] dirDown = new boolean[4];  // W A S D
    private boolean stickRun, runButtonHeld, shiftDown;  // Shift comes from the rim of the stick or SPRINT
    private static final int[] DIR_KEYS = {KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_D};

    // camera / menu pointer
    private int lookPointer = -1;
    private float lookX, lookY, lastMouseX, lastMouseY;
    private int mouseState = 0;
    private final Map<Integer, Button> pointerButton = new HashMap<>();

    public TouchControls(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("controls", Context.MODE_PRIVATE);
        for (int i = 0; i < settings.length; i++) {
            Setting s = settings[i];
            s.index = Math.max(0, Math.min(s.values.length - 1, prefs.getInt(s.key, DEFAULTS[i])));
        }
        loadLayout();
        applyUiSettings();
        Typeface face = Typeface.create("sans-serif-condensed", Typeface.BOLD);
        text.setTypeface(face);
        text.setTextAlign(Paint.Align.CENTER);
        text.setLetterSpacing(0.08f);
        edge.setStyle(Paint.Style.STROKE);
        glow.setStyle(Paint.Style.STROKE);
        glow.setMaskFilter(new BlurMaskFilter(6 * density, BlurMaskFilter.Blur.NORMAL));
        setHapticFeedbackEnabled(true);
        InputManager im = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        if (im != null) {
            im.registerInputDeviceListener(this, null);
            padConnected = findGamepad(im);
        }
        // Refresh the performance overlay twice a second.
        postDelayed(new Runnable() {
            @Override
            public void run() {
                if (showFps) invalidate();
                postDelayed(this, 500);
            }
        }, 500);
    }

    // Glowing shapes are rendered once into bitmaps (blur needs the software pipeline) and then only blitted.
    private final Map<String, Bitmap> cache = new HashMap<>();
    private final Paint blit = new Paint(Paint.FILTER_BITMAP_FLAG);

    private interface Painter {
        void paint(Canvas c, float cx, float cy);
    }

    private Bitmap cached(String key, float halfSize, Painter painter) {
        Bitmap b = cache.get(key);
        if (b == null) {
            int n = (int) Math.ceil(halfSize * 2);
            b = Bitmap.createBitmap(n, n, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(b);
            float saved = opacity;
            opacity = 1f;  // opacity is applied when blitting
            painter.paint(c, n / 2f, n / 2f);
            opacity = saved;
            cache.put(key, b);
        }
        return b;
    }

    private void blitCentered(Canvas c, Bitmap b, float cx, float cy) {
        blit.setAlpha(Math.round(255 * opacity));
        c.drawBitmap(b, cx - b.getWidth() / 2f, cy - b.getHeight() / 2f, blit);
    }

    private void applyUiSettings() {
        cache.clear();
        opacity = settings[4].value() / 100f;
        scale = settings[5].value() / 100f;
        vibrate = settings[6].value() != 0;
        showFps = settings[7].value() != 0;
        hideWithPad = settings[8].value() != 0;
        autoRun = settings[9].value() != 0;
        camGain = settings[10].value() / 100f;
        invertY = settings[11].value() != 0;
        powerButtons = settings[12].value() != 0;
        layoutButtons(getWidth(), getHeight());
    }

    private void loadLayout() {
        for (int i = 0; i < buttons.length; i++) {
            buttons[i].customX = prefs.getFloat("pos" + i + "x", -1);
            buttons[i].customY = prefs.getFloat("pos" + i + "y", -1);
            buttons[i].size = prefs.getFloat("size" + i, 1);
        }
        stickCustomX = prefs.getFloat("stick_x", -1);
        stickCustomY = prefs.getFloat("stick_y", -1);
        stickSize = prefs.getFloat("stick_size", 1);
    }

    private void saveLayout() {
        int w = Math.max(1, getWidth()), h = Math.max(1, getHeight());
        SharedPreferences.Editor ed = prefs.edit();
        for (int i = 0; i < buttons.length; i++) {
            Button b = buttons[i];
            ed.putFloat("size" + i, b.size);
            if (b.customX < 0) continue;
            ed.putFloat("pos" + i + "x", b.cx / w).putFloat("pos" + i + "y", b.cy / h);
        }
        ed.putFloat("stick_size", stickSize);
        if (stickCustomX >= 0) ed.putFloat("stick_x", stickHomeX / w).putFloat("stick_y", stickHomeY / h);
        ed.apply();
    }

    private void resetLayout() {
        SharedPreferences.Editor ed = prefs.edit();
        for (int i = 0; i < buttons.length; i++) {
            buttons[i].customX = buttons[i].customY = -1;
            buttons[i].size = 1;
            ed.remove("pos" + i + "x").remove("pos" + i + "y").remove("size" + i);
        }
        stickCustomX = stickCustomY = -1;
        stickSize = 1;
        selected = null;
        selectedStick = false;
        ed.remove("stick_x").remove("stick_y").remove("stick_size").apply();
        layoutButtons(getWidth(), getHeight());
    }

    // ---- gamepads ----
    // A real controller has gamepad buttons and a stick (joystick axes). Phones list built-in devices that claim
    // SOURCE_GAMEPAD too (Xiaomi: fingerprint sensor, side keys), which used to hide the touch controls with no
    // controller anywhere; those have no joystick axes, and on Android 10+ they are not external.
    private static boolean findGamepad(InputManager im) {
        for (int id : im.getInputDeviceIds()) {
            InputDevice d = im.getInputDevice(id);
            if (d == null || d.isVirtual()) continue;
            int sources = d.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD
                    || (sources & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) continue;
            if (d.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) == null
                    || d.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) == null) continue;
            if (Build.VERSION.SDK_INT >= 29 && !d.isExternal()) continue;
            return true;
        }
        return false;
    }

    private void updatePads() {
        InputManager im = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
        if (im != null) padConnected = findGamepad(im);
        invalidate();
    }

    @Override public void onInputDeviceAdded(int id) { updatePads(); }
    @Override public void onInputDeviceRemoved(int id) { updatePads(); }
    @Override public void onInputDeviceChanged(int id) { updatePads(); }

    private boolean controlsHidden() {
        return hideWithPad && padConnected;
    }

    // ---- layout ----
    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layoutButtons(w, h);
        lastMouseX = w / 2f;
        lastMouseY = h / 2f;
    }

    private void layoutButtons(int w, int h) {
        final float u = density * scale;
        for (Button b : buttons) {
            b.r = b.sizeDp * u * b.size;
            if (b.customX >= 0) {
                b.cx = b.customX * w;
                b.cy = b.customY * h;
            } else {
                b.cx = b.fromRight ? w - b.dx * u : b.dx * u;
                b.cy = b.fromBottom ? h - b.dy * u : b.dy * u;
            }
        }
        stickR = 62 * u * stickSize;
        if (stickCustomX >= 0) {
            stickHomeX = stickCustomX * w;
            stickHomeY = stickCustomY * h;
        } else {
            stickHomeX = 225 * u;  // right of the minimap
            stickHomeY = h - 112 * u;
        }
        stickX = knobX = stickHomeX;
        stickY = knobY = stickHomeY;
        modeRect.set(14 * density, 14 * density, 110 * density, 52 * density);
        gearRect.set(w - 58 * density, 12 * density, w - 14 * density, 56 * density);
        float pw = Math.min(w * 0.92f, 860 * density), ph = Math.min(h * 0.94f, 500 * density);
        panel.set((w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2);
    }

    // ---- input to SDL ----
    private void key(int keycode, boolean down) {
        if (down) SDLActivity.onNativeKeyDown(keycode);
        else SDLActivity.onNativeKeyUp(keycode);
    }

    private void mouseButton(int bit, boolean down, float x, float y) {
        if (down) mouseState |= bit;
        else mouseState &= ~bit;
        int action = down ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
        if (gameMode) {
            // SDL sends a motion with every button: in game mode a zero relative one, as an absolute position would
            // jerk the camera (the cursor is wherever the relative camera drags left it)
            SDLActivity.onNativeMouse(mouseState, action, 0, 0, true);
            return;
        }
        lastMouseX = x;
        lastMouseY = y;
        SDLActivity.onNativeMouse(mouseState, action, x, y, false);
    }

    private void mouseMoveTo(float x, float y) {
        lastMouseX = x;
        lastMouseY = y;
        SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_MOVE, x, y, false);
    }

    private float moveRestX, moveRestY;  // SDL takes whole pixels: carry the fractions

    private void mouseMoveBy(float dx, float dy) {
        dx += moveRestX;
        dy += moveRestY;
        int ix = (int) dx, iy = (int) dy;
        moveRestX = dx - ix;
        moveRestY = dy - iy;
        if (ix != 0 || iy != 0) SDLActivity.onNativeMouse(mouseState, MotionEvent.ACTION_MOVE, ix, iy, true);
    }

    private void buzz() {
        if (vibrate) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }

    private void press(Button b, int pointer) {
        b.pointer = pointer;
        pointerButton.put(pointer, b);
        buzz();
        if (b.mouseButton != 0) mouseButton(b.mouseButton, true, lastMouseX, lastMouseY);
        else if (b.keycode == KeyEvent.KEYCODE_SHIFT_LEFT) setRun(stickRun, true);
        else key(b.keycode, true);
    }

    private void release(Button b) {
        pointerButton.remove(b.pointer);
        b.pointer = -1;
        if (b.mouseButton != 0) mouseButton(b.mouseButton, false, lastMouseX, lastMouseY);
        else if (b.keycode == KeyEvent.KEYCODE_SHIFT_LEFT) setRun(stickRun, false);
        else key(b.keycode, false);
    }

    private void setRun(boolean fromStick, boolean fromButton) {
        stickRun = fromStick;
        runButtonHeld = fromButton;
        boolean want = stickRun || runButtonHeld;
        if (want != shiftDown) {
            shiftDown = want;
            key(KeyEvent.KEYCODE_SHIFT_LEFT, want);
        }
    }

    private void updateStick(float x, float y) {
        float dx = x - stickX, dy = y - stickY;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > stickR) {
            dx *= stickR / len;
            dy *= stickR / len;
        }
        knobX = stickX + dx;
        knobY = stickY + dy;
        float dead = stickR * 0.3f;
        boolean[] want = {dy < -dead, dx < -dead, dy > dead, dx > dead};
        for (int i = 0; i < 4; i++) {
            if (want[i] != dirDown[i]) {
                dirDown[i] = want[i];
                key(DIR_KEYS[i], want[i]);
            }
        }
        // Pushed to the rim (with hysteresis): sprint, as with Shift held.
        boolean run = autoRun && stickPointer >= 0 && len > stickR * (stickRun ? 0.85f : 0.95f);
        if (run != stickRun) setRun(run, runButtonHeld);
    }

    private void releaseStick() {
        stickPointer = -1;
        updateStick(stickX, stickY);
        stickX = knobX = stickHomeX;
        stickY = knobY = stickHomeY;
    }

    private void releaseAll() {
        for (Button b : buttons) if (b.pointer >= 0) release(b);
        if (stickPointer >= 0) releaseStick();
        setRun(false, false);
        if (lookPointer >= 0 && !gameMode) mouseButton(MOUSE_LEFT, false, lookX, lookY);
        lookPointer = -1;
    }

    private boolean visible(Button b) {
        if (b.quickPower && !powerButtons) return false;  // off by default: the screen was too busy
        return editing || (!controlsHidden() && (gameMode || !b.gameOnly));
    }

    // ---- layout editor ----
    private void startEditing() {
        releaseAll();
        panelOpen = false;
        editing = true;
    }

    private void editTouch(MotionEvent e, int action, int id, float x, float y) {
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                if (dragPointer >= 0) break;
                if (doneRect.contains(x, y)) {
                    saveLayout();
                    editing = false;
                    buzz();
                    break;
                }
                if (resetRect.contains(x, y)) {
                    resetLayout();
                    buzz();
                    break;
                }
                if ((selected != null || selectedStick) && (smallerRect.contains(x, y) || biggerRect.contains(x, y))) {
                    float step = biggerRect.contains(x, y) ? 0.1f : -0.1f;
                    if (selected != null) {
                        selected.size = Math.max(0.6f, Math.min(2f, selected.size + step));
                    } else {
                        stickSize = Math.max(0.6f, Math.min(2f, stickSize + step));
                    }
                    float sx = stickHomeX, sy = stickHomeY;
                    layoutButtons(getWidth(), getHeight());
                    if (stickCustomX < 0) {  // keep a stick that was not moved where it is drawn
                        stickHomeX = stickX = knobX = sx;
                        stickHomeY = stickY = knobY = sy;
                    }
                    buzz();
                    break;
                }
                for (Button b : buttons) {
                    if (visible(b) && b.hit(x, y)) {
                        dragButton = b;
                        dragDx = b.cx - x;
                        dragDy = b.cy - y;
                        break;
                    }
                }
                if (dragButton == null) {
                    float dx = x - stickHomeX, dy = y - stickHomeY;
                    if (dx * dx + dy * dy <= stickR * stickR) {
                        dragStick = true;
                        dragDx = stickHomeX - x;
                        dragDy = stickHomeY - y;
                    }
                }
                if (dragButton != null || dragStick) {
                    dragPointer = id;
                    selected = dragButton;
                    selectedStick = dragStick;
                    buzz();
                }
                break;
            case MotionEvent.ACTION_MOVE: {
                int i = e.findPointerIndex(dragPointer);
                if (i < 0) break;
                float w = getWidth(), h = getHeight();
                float nx = Math.max(0, Math.min(w, e.getX(i) + dragDx));
                float ny = Math.max(0, Math.min(h, e.getY(i) + dragDy));
                if (dragButton != null) {
                    dragButton.cx = nx;
                    dragButton.cy = ny;
                    dragButton.customX = nx / w;
                    dragButton.customY = ny / h;
                } else if (dragStick) {
                    stickHomeX = stickX = knobX = nx;
                    stickHomeY = stickY = knobY = ny;
                    stickCustomX = nx / w;
                    stickCustomY = ny / h;
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (action == MotionEvent.ACTION_CANCEL || id == dragPointer) {
                    dragPointer = -1;
                    dragButton = null;
                    dragStick = false;
                }
                break;
            default:
                break;
        }
    }

    // ---- touch ----
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        float x = e.getX(index), y = e.getY(index);
        if (panelOpen) {
            if (action == MotionEvent.ACTION_UP) panelTap(x, y);
            invalidate();
            return true;
        }
        if (editing) {
            editTouch(e, action, id, x, y);
            invalidate();
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                if (gearRect.contains(x, y)) {
                    gearPointer = id;
                    break;
                }
                if (!controlsHidden() && modeRect.contains(x, y)) {
                    modePointer = id;
                    break;
                }
                Button hit = null;
                for (Button b : buttons) {
                    if (visible(b) && b.pointer < 0 && b.hit(x, y)) {
                        hit = b;
                        break;
                    }
                }
                if (hit != null) {
                    hit.fx = x;
                    hit.fy = y;
                    press(hit, id);
                } else if (gameMode && !controlsHidden() && stickPointer < 0 && x < getWidth() * 0.4f
                        && y > getHeight() * 0.35f) {
                    stickPointer = id;
                    stickX = x;  // the stick centres where the thumb lands
                    stickY = y;
                    updateStick(x, y);
                } else if (lookPointer < 0) {
                    lookPointer = id;
                    lookX = x;
                    lookY = y;
                    if (!gameMode) {
                        mouseMoveTo(x, y);
                        mouseButton(MOUSE_LEFT, true, x, y);
                    }
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int pid = e.getPointerId(i);
                    float px = e.getX(i), py = e.getY(i);
                    Button held = pointerButton.get(pid);
                    if (held != null && held.wheel) {
                        float g = 2.5f * camGain;
                        mouseMoveBy((px - held.fx) * g, (py - held.fy) * g);
                        held.fx = px;
                        held.fy = py;
                    } else if (pid == stickPointer) {
                        updateStick(px, py);
                    } else if (pid == lookPointer) {
                        float g = 1.5f * camGain;
                        if (gameMode) mouseMoveBy((px - lookX) * g, (py - lookY) * g * (invertY ? -1 : 1));
                        else mouseMoveTo(px, py);
                        lookX = px;
                        lookY = py;
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if (id == gearPointer) {
                    gearPointer = -1;
                    if (gearRect.contains(x, y)) {
                        releaseAll();
                        panelOpen = true;
                        buzz();
                    }
                } else if (id == modePointer) {
                    modePointer = -1;
                    if (modeRect.contains(x, y)) {
                        releaseAll();
                        gameMode = !gameMode;
                        buzz();
                    }
                } else if (pointerButton.containsKey(id)) {
                    release(pointerButton.get(id));
                } else if (id == stickPointer) {
                    releaseStick();
                } else if (id == lookPointer) {
                    lookPointer = -1;
                    if (!gameMode) mouseButton(MOUSE_LEFT, false, x, y);
                }
                break;
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                modePointer = gearPointer = -1;
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    // ---- settings panel ----
    private void panelTap(float x, float y) {
        if (closeRect.contains(x, y) || !panel.contains(x, y)) {
            panelOpen = false;
            return;
        }
        if (restartNeeded && restartRect.contains(x, y)) {
            restartApp();
            return;
        }
        if (layoutRect.contains(x, y)) {
            startEditing();
            return;
        }
        for (int i = 0; i < settings.length; i++) {
            int delta = leftArrows.get(i).contains(x, y) ? -1 : rightArrows.get(i).contains(x, y) ? 1 : 0;
            if (delta == 0) continue;
            Setting s = settings[i];
            s.index = (s.index + delta + s.values.length) % s.values.length;
            prefs.edit().putInt(s.key, s.index).apply();
            buzz();
            if (s.key.equals("fps_limit")) nativeSetFpsLimit(s.value());  // applies right away
            else if (s.engine) restartNeeded = true;
            if (s.engine) writeEngineSettings();
            applyUiSettings();
            return;
        }
    }

    /** Rewrites the engine keys in protore.ini (next to game/), keeping every other line. */
    private void writeEngineSettings() {
        File dir = getContext().getExternalFilesDir(null);
        if (dir == null) return;
        File ini = new File(dir, "protore.ini");
        StringBuilder out = new StringBuilder();
        try {
            if (ini.exists()) {
                for (String line : new String(Files.readAllBytes(ini.toPath()), StandardCharsets.UTF_8).split("\n")) {
                    String k = line.split("=", 2)[0].trim();
                    boolean ours = false;
                    for (Setting s : settings) ours |= s.engine && s.key.equals(k);
                    if (!ours && !line.trim().isEmpty()) out.append(line).append('\n');
                }
            }
            for (Setting s : settings) {
                if (s.engine) out.append(s.key).append(" = ").append(s.value()).append('\n');
            }
            try (FileWriter w = new FileWriter(ini)) {
                w.write(out.toString());
            }
        } catch (Exception ignored) {
            // settings stay in effect for the UI; the engine keeps its previous values
        }
    }

    private void restartApp() {
        Context ctx = getContext();
        Intent intent = new Intent(ctx, ProtoActivity.class);
        PendingIntent pi = PendingIntent.getActivity(ctx, 0, intent,
                PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        AlarmManager am = (AlarmManager) ctx.getSystemService(Context.ALARM_SERVICE);
        if (am != null) am.set(AlarmManager.RTC, System.currentTimeMillis() + 400, pi);
        if (ctx instanceof Activity) ((Activity) ctx).finishAffinity();
        System.exit(0);
    }

    // ---- drawing ----
    private int alpha(int color, float a) {
        return Color.argb(Math.round(Color.alpha(color) * a * opacity), Color.red(color), Color.green(color),
                Color.blue(color));
    }

    private void hexagon(float cx, float cy, float r) {
        path.reset();
        for (int i = 0; i < 6; i++) {
            double a = Math.PI / 3 * i + Math.PI / 6;
            float px = cx + (float) Math.cos(a) * r, py = cy + (float) Math.sin(a) * r;
            if (i == 0) path.moveTo(px, py);
            else path.lineTo(px, py);
        }
        path.close();
    }

    /** Prototype's bracket corners around a rectangle. */
    private void brackets(Canvas c, RectF r, float len, Paint p) {
        c.drawLine(r.left, r.top, r.left + len, r.top, p);
        c.drawLine(r.left, r.top, r.left, r.top + len, p);
        c.drawLine(r.right, r.top, r.right - len, r.top, p);
        c.drawLine(r.right, r.top, r.right, r.top + len, p);
        c.drawLine(r.left, r.bottom, r.left + len, r.bottom, p);
        c.drawLine(r.left, r.bottom, r.left, r.bottom - len, p);
        c.drawLine(r.right, r.bottom, r.right - len, r.bottom, p);
        c.drawLine(r.right, r.bottom, r.right, r.bottom - len, p);
    }

    private void drawButton(Canvas c, Button b) {
        final boolean down = b.pointer >= 0;
        if (down && b.wheel) {  // where the held finger points the wheel
            edge.setStrokeWidth(3 * density);
            edge.setColor(0xccffffff);
            c.drawLine(b.cx, b.cy, b.fx, b.fy, edge);
            c.drawCircle(b.fx, b.fy, 10 * density, edge);
        }
        Bitmap bmp = cached(b.label + (down ? ":1:" : ":0:") + b.r, b.r + 12 * density,
                (cc, x, y) -> paintButton(cc, b, x, y, down));
        blitCentered(c, bmp, b.cx, b.cy);
    }

    private void paintButton(Canvas c, Button b, float bx, float by, boolean down) {
        hexagon(bx, by, b.r);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(down ? 0xd0b0180f : 0xa00b0b0b, 1f));
        c.drawPath(path, fill);
        glow.setStrokeWidth(4 * density);
        glow.setColor(alpha(RED, down ? 1f : 0.55f));
        c.drawPath(path, glow);
        edge.setStrokeWidth(1.8f * density);
        edge.setColor(alpha(down ? 0xffffd0c8 : RED, 1f));
        c.drawPath(path, edge);
        hexagon(bx, by, b.r * 0.78f);
        edge.setStrokeWidth(0.8f * density);
        edge.setColor(alpha(RED_DIM, 0.9f));
        c.drawPath(path, edge);
        text.setTextSize(Math.min(14 * density, b.r * 0.42f));
        text.setColor(alpha(down ? 0xffffffff : 0xffffd6d0, 1f));
        c.drawText(b.label, bx, by + text.getTextSize() * 0.36f, text);
    }

    private void drawStick(Canvas c) {
        blitCentered(c, cached("stick:" + stickR, stickR + 12 * density, this::paintStickBase), stickX, stickY);
        final boolean held = stickPointer >= 0;
        blitCentered(c, cached("knob:" + held + stickR, stickR * 0.5f + 4 * density,
                (cc, x, y) -> paintKnob(cc, x, y, held)), knobX, knobY);
    }

    private void paintKnob(Canvas c, float x, float y, boolean held) {
        hexagon(x, y, stickR * 0.42f);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(held ? 0xe0c01a10 : 0xa0300806, 1f));
        c.drawPath(path, fill);
        edge.setStrokeWidth(1.6f * density);
        edge.setColor(alpha(0xffff6a5e, 1f));
        c.drawPath(path, edge);
    }

    private void paintStickBase(Canvas c, float stickX, float stickY) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(0x700b0b0b, 1f));
        c.drawCircle(stickX, stickY, stickR, fill);
        glow.setStrokeWidth(4 * density);
        glow.setColor(alpha(RED, 0.5f));
        c.drawCircle(stickX, stickY, stickR, glow);
        edge.setStrokeWidth(1.6f * density);
        edge.setColor(alpha(RED, 0.9f));
        c.drawCircle(stickX, stickY, stickR, edge);
        // tick marks every 30 degrees, longer on the four directions
        for (int i = 0; i < 12; i++) {
            double a = Math.PI / 6 * i;
            float in = stickR * (i % 3 == 0 ? 0.80f : 0.88f);
            c.drawLine(stickX + (float) Math.cos(a) * in, stickY + (float) Math.sin(a) * in,
                    stickX + (float) Math.cos(a) * stickR, stickY + (float) Math.sin(a) * stickR, edge);
        }
    }

    private void drawPill(Canvas c, RectF r, String label, boolean active) {
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(alpha(active ? 0xc08a120c : 0xa00b0b0b, 1f));
        c.drawRect(r, fill);
        edge.setStrokeWidth(1.5f * density);
        edge.setColor(alpha(RED, 1f));
        brackets(c, r, 9 * density, edge);
        text.setTextSize(14 * density);
        text.setColor(alpha(0xffffe0dc, 1f));
        c.drawText(label, r.centerX(), r.centerY() + 5 * density, text);
    }

    private void drawGear(Canvas c) {
        float cx = gearRect.centerX(), cy = gearRect.centerY(), r = gearRect.width() * 0.32f;
        edge.setStrokeWidth(2.2f * density);
        edge.setColor(alpha(RED, 1f));
        for (int i = 0; i < 8; i++) {
            double a = Math.PI / 4 * i;
            c.drawLine(cx + (float) Math.cos(a) * r, cy + (float) Math.sin(a) * r,
                    cx + (float) Math.cos(a) * r * 1.35f, cy + (float) Math.sin(a) * r * 1.35f, edge);
        }
        c.drawCircle(cx, cy, r, edge);
        c.drawCircle(cx, cy, r * 0.4f, edge);
    }

    private void drawPerf(Canvas c) {
        float fps, ms;
        int mem;
        try {
            fps = nativeFps();
            ms = nativeFrameMs();
            mem = nativeMemoryMiB();
        } catch (UnsatisfiedLinkError e) {
            return;
        }
        String line = String.format("%.0f FPS  %.1f MS  %d MB", fps, ms, mem);
        text.setTextSize(13 * density);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(fps >= 50 ? 0xff7dff7a : fps >= 28 ? 0xffffd060 : 0xffff5a4a);
        c.drawText(line, 16 * density, 74 * density, text);
        text.setTextAlign(Paint.Align.CENTER);
    }

    private void drawPanel(Canvas c) {
        c.drawColor(0x99000000);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0xf00b0b0b);
        c.drawRect(panel, fill);
        edge.setStrokeWidth(3 * density);
        edge.setColor(0x60e0261e);
        c.drawRect(panel, edge);
        edge.setStrokeWidth(2 * density);
        edge.setColor(RED);
        brackets(c, panel, 22 * density, edge);
        float pad = 22 * density;
        text.setTextSize(20 * density);
        text.setColor(RED);
        c.drawText("SETTINGS", panel.centerX(), panel.top + 36 * density, text);
        // Credit, in the corners beside the title.
        text.setTextSize(11 * density);
        text.setColor(0xff9a8a88);
        text.setTextAlign(Paint.Align.LEFT);
        c.drawText(SetupActivity.CREDIT, panel.left + pad, panel.top + 34 * density, text);
        text.setTextAlign(Paint.Align.RIGHT);
        c.drawText(SetupActivity.CREDIT_URL, panel.right - pad, panel.top + 34 * density, text);
        text.setTextAlign(Paint.Align.CENTER);
        // two columns of settings
        int rows = (settings.length + 1) / 2;
        float rowH = (panel.height() - 116 * density) / rows;
        float colW = (panel.width() - pad * 3) / 2;
        float top = panel.top + 54 * density;
        leftArrows.clear();
        rightArrows.clear();
        text.setTextSize(Math.min(15 * density, rowH * 0.42f));
        for (int i = 0; i < settings.length; i++) {
            Setting s = settings[i];
            float colLeft = panel.left + pad + (i / rows) * (colW + pad), colRight = colLeft + colW;
            float y = top + rowH * (i % rows) + rowH / 2;
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(0xffe8d6d4);
            c.drawText(s.label + (s.engine && !s.key.equals("fps_limit") ? " *" : ""), colLeft, y + 5 * density,
                    text);
            float vx = colRight - 62 * density;
            RectF l = new RectF(vx - 64 * density, y - rowH / 2, vx - 32 * density, y + rowH / 2);
            RectF r = new RectF(vx + 32 * density, y - rowH / 2, vx + 64 * density, y + rowH / 2);
            leftArrows.add(l);
            rightArrows.add(r);
            text.setTextAlign(Paint.Align.CENTER);
            text.setColor(RED);
            c.drawText("‹", l.centerX(), y + 6 * density, text);
            c.drawText("›", r.centerX(), y + 6 * density, text);
            text.setColor(0xffffffff);
            c.drawText(s.names[s.index], vx, y + 5 * density, text);
            edge.setStrokeWidth(0.6f * density);
            edge.setColor(0x40e0261e);
            c.drawLine(colLeft, y + rowH / 2, colRight, y + rowH / 2, edge);
        }
        text.setTextAlign(Paint.Align.CENTER);
        float by = panel.bottom - 36 * density, pillW = 150 * density, gap = 12 * density;
        float cx = panel.centerX();
        layoutRect.set(cx - pillW * 1.5f - gap, by - 18 * density, cx - pillW * 0.5f - gap, by + 18 * density);
        drawPill(c, layoutRect, "LAYOUT", false);
        closeRect.set(cx + pillW * 0.5f + gap, by - 18 * density, cx + pillW * 1.5f + gap, by + 18 * density);
        drawPill(c, closeRect, "CLOSE", false);
        restartRect.set(cx - pillW * 0.5f, by - 18 * density, cx + pillW * 0.5f, by + 18 * density);
        if (restartNeeded) {
            drawPill(c, restartRect, "RESTART", true);
        } else {
            text.setTextSize(11 * density);
            text.setColor(0xff9a8a88);
            c.drawText("* applies after restart", restartRect.centerX(), by + 4 * density, text);
        }
    }

    private void drawEditor(Canvas c) {
        c.drawColor(0x66000000);
        drawStick(c);
        for (Button b : buttons) {
            if (visible(b)) drawButton(c, b);
        }
        float cx = getWidth() * 0.38f, top = 16 * density;  // left of centre: clear of the buttons on the top right
        text.setTextSize(15 * density);
        text.setColor(0xffffe0dc);
        c.drawText("Drag the buttons and the stick, tap one to resize it", cx, top + 70 * density, text);
        resetRect.set(cx - 162 * density, top, cx - 12 * density, top + 38 * density);
        doneRect.set(cx + 12 * density, top, cx + 162 * density, top + 38 * density);
        drawPill(c, resetRect, "RESET", false);
        drawPill(c, doneRect, "DONE", true);
        if (selected != null || selectedStick) {
            float sx = selected != null ? selected.cx : stickHomeX, sy = selected != null ? selected.cy : stickHomeY;
            float sr = selected != null ? selected.r : stickR;
            edge.setStrokeWidth(2.5f * density);
            edge.setColor(0xffffffff);
            c.drawCircle(sx, sy, sr + 6 * density, edge);
            float y = top + 96 * density, size = selected != null ? selected.size : stickSize;
            smallerRect.set(cx - 150 * density, y, cx - 60 * density, y + 38 * density);
            biggerRect.set(cx + 60 * density, y, cx + 150 * density, y + 38 * density);
            drawPill(c, smallerRect, "\u2212", false);
            drawPill(c, biggerRect, "+", false);
            text.setTextSize(15 * density);
            text.setColor(0xffffe0dc);
            c.drawText((selected != null ? selected.label : "STICK") + "  " + Math.round(size * 100) + "%", cx,
                    y + 25 * density, text);
        } else {
            smallerRect.setEmpty();
            biggerRect.setEmpty();
        }
    }

    // Screenshot of this layer for testing (the game's own shot_now trigger only has the game frame): a file named
    // ui_shot_now in the app's files folder makes the controls draw themselves into ui_shot.png there.
    private final Runnable uiShotPoll = new Runnable() {
        @Override public void run() {
            File dir = getContext().getExternalFilesDir(null);
            File trigger = dir == null ? null : new File(dir, "ui_shot_now");
            if (trigger != null && trigger.delete() && getWidth() > 0) {
                Bitmap bmp = Bitmap.createBitmap(getWidth(), getHeight(), Bitmap.Config.ARGB_8888);
                draw(new Canvas(bmp));
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(new File(dir, "ui_shot.png"))) {
                    bmp.compress(Bitmap.CompressFormat.PNG, 100, out);
                } catch (java.io.IOException ignored) {
                    // a test aid only
                }
                bmp.recycle();
            }
            postDelayed(this, 1000);
        }
    };

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        postDelayed(uiShotPoll, 1000);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(uiShotPoll);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (editing) {
            drawEditor(canvas);
            return;
        }
        if (!controlsHidden()) drawPill(canvas, modeRect, gameMode ? "GAME" : "MENU", gameMode);
        drawGear(canvas);
        if (showFps) drawPerf(canvas);
        if (gameMode && !controlsHidden()) drawStick(canvas);
        for (Button b : buttons) {
            if (visible(b)) drawButton(canvas, b);
        }
        if (panelOpen) drawPanel(canvas);
    }
}
