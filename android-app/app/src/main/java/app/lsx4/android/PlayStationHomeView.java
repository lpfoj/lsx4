package app.lsx4.android;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Camera;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.SweepGradient;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.BatteryManager;
import android.os.Build;
import android.os.SystemClock;
import android.text.format.DateFormat;
import android.util.LruCache;
import android.util.TypedValue;
import android.view.GestureDetector;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import java.io.File;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Random;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;








final class PlayStationHomeView extends View {
    static final int HORIZONTAL = 1;
    static final int VERTICAL = 2;
    static final int LIBRARY_HORIZONTAL = 3;
    static final int LIBRARY_VERTICAL = 4;
    static final int TILES_HORIZONTAL = 5;
    static final int TILES_VERTICAL = 6;

    static final int FUNCTION_VIEW = 0;
    static final int FUNCTION_SEARCH = 1;
    static final int FUNCTION_SORT = 2;
    static final int FUNCTION_ADD = 3;
    static final int FUNCTION_SETTINGS = 4;

    static final int STATUS_UNCLASSIFIED = 0;
    static final int STATUS_SUPPORTED = 1;
    static final int STATUS_UNSUPPORTED = 2;

    static final int PARTICLE_CIRCLE = 0;
    static final int PARTICLE_PETAL = 1;
    static final int PARTICLE_LEAF = 2;

    interface Listener {
        void onActivate(Item item);

        void onSettings(Item item);

        default void onSelected(Item item) {}


        void onFunction(int function, RectF bounds);
    }

    static final class Item {
        final String key;
        final String title;
        final String titleId;
        final String platform;
        final File icon;
        final File art;
        final boolean launchable;
        final boolean device;
        final boolean utility;
        final int status;
        final Object tag;

        Item(String key, String title, String titleId, String platform, File icon, File art,
             boolean launchable, boolean device, int status, Object tag) {
            this.key = key;
            this.title = title;
            this.titleId = titleId;
            this.platform = platform;
            this.icon = icon;
            this.art = art;
            this.launchable = launchable;
            this.device = device;
            this.utility = device || key.startsWith("menu:");
            this.status = status;
            this.tag = tag;
        }
    }


    static final class Theme {
        final int name;
        final int gradientStart;
        final int background;
        final int gradientEnd;
        final int primary;
        final int glow;
        final int highlight;
        final int particle;

        Theme(int name, int gradientStart, int background, int gradientEnd, int primary,
              int glow, int highlight, int particle) {
            this.name = name;
            this.gradientStart = gradientStart;
            this.background = background;
            this.gradientEnd = gradientEnd;
            this.primary = primary;
            this.glow = glow;
            this.highlight = highlight;
            this.particle = particle;
        }
    }

    static final Theme[] THEMES = {
            new Theme(R.string.home_theme_playstation, 0xff0f2448, 0xff0a1a35, 0xff03070f,
                    0xff6fb4ff, 0xff6fb4ff, 0xffffffff, PARTICLE_CIRCLE),
            new Theme(R.string.home_theme_classic, 0xff0f0f1a, 0xff0a0a14, 0xff050508,
                    0xff6abba0, 0xff6abba0, 0xffffffff, PARTICLE_CIRCLE),
            new Theme(R.string.home_theme_sakura, 0xff35102a, 0xff1f0815, 0xff12050d,
                    0xffff91a4, 0xffff6b8a, 0xffff91a4, PARTICLE_PETAL),
            new Theme(R.string.home_theme_autumn, 0xff2a1a0f, 0xff1a0f0a, 0xff0a0805,
                    0xffff8c00, 0xffcd853f, 0xffff8c00, PARTICLE_LEAF),
    };

    private static final int PS5_BADGE = 0xff1769aa;
    private static final int PS4_BADGE = 0xff0b2a66;
    private static final int SUPPORTED = 0xff67d391;
    private static final int UNSUPPORTED = 0xffff806f;
    private static final long DEVICE_INFO_REFRESH_MS = 5_000L;
    private static final long SYSTEM_STATUS_REFRESH_MS = 10_000L;
    private static final int PARTICLES = 80;

    private final List<Item> items = new ArrayList<>();
    private final float density;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint titleOutline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint shimmer = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path[] ribbons = {new Path(), new Path(), new Path()};
    private final Path shape = new Path();
    private final AndroidMenuGlyph androidGlyph = new AndroidMenuGlyph();
    private final RectF rect = new RectF();
    private final RectF tile = new RectF();
    private final RectF playRect = new RectF();
    private final RectF settingsRect = new RectF();
    private final Matrix matrix = new Matrix();
    private final Matrix shaderMatrix = new Matrix();
    private final Camera camera = new Camera();
    private final WeakHashMap<Bitmap, BitmapShader> shaders = new WeakHashMap<>();
    private final float[] seedX = new float[PARTICLES];
    private final float[] seedY = new float[PARTICLES];
    private final float[] seedRadius = new float[PARTICLES];
    private final float[] seedSpeed = new float[PARTICLES];
    private final float[] seedDrift = new float[PARTICLES];
    private final float[] seedAlpha = new float[PARTICLES];
    private final float[] seedPhase = new float[PARTICLES];
    private final float[] seedRotation = new float[PARTICLES];
    private final List<RectF> hitRects = new ArrayList<>();
    private final List<Integer> hitIndices = new ArrayList<>();
    private final LruCache<String, Bitmap> bitmaps;
    private final Set<String> pending = new HashSet<>();
    private final ExecutorService loader;
    private final GestureDetector gestures;
    private final Typeface regular = Typeface.create("sans-serif", Typeface.NORMAL);
    private final Typeface light = Typeface.create("sans-serif-light", Typeface.NORMAL);
    private final Typeface medium = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    private final Drawable[] functionIcons = new Drawable[5];
    private final int[] functionLabels = {R.string.home_fn_view, R.string.home_fn_search,
            R.string.home_fn_sort, R.string.home_fn_add, R.string.home_fn_settings};
    private final RectF[] functionRects = {new RectF(), new RectF(), new RectF(), new RectF(),
            new RectF()};

    private boolean sectionMode;
    private boolean menuActive = true;
    private int layout = HORIZONTAL;
    private Theme theme = THEMES[0];
    private Listener listener;
    private MenuSoundEngine sounds;
    private float position;
    private float lastPosition;
    private int target;
    private final TileGridState tileGrid = new TileGridState();
    private boolean dragging;
    private boolean flung;
    private long lastFrameNanos;
    private float time;
    private float tilt;
    private int announcedIndex = -1;
    private String artKey;
    private Bitmap artCurrent;
    private Bitmap artPrevious;
    private float artFade = 1f;
    private float activation;
    private float activationElapsed;
    private boolean activating;
    private boolean quietUntilTarget;
    private List<DeviceInfoSnapshot.Line> deviceLines = Collections.emptyList();
    private long deviceInfoAt;
    private boolean playEnabled;
    private boolean settingsEnabled;
    private float footerTop = Float.MAX_VALUE;
    private int functionFocus = -1;
    private int pressedFunction = -1;
    private String status = "";
    private boolean busy;
    private int insetTop;
    private int insetBottom;
    private float lastFocusX;
    private float lastFocusY;
    private boolean immersive;
    private long systemStatusAt;
    private int batteryLevel = -1;
    private boolean charging;
    private boolean wifi;
    private boolean released;
    private LinearGradient shimmerGradient;
    private SweepGradient glow;

    PlayStationHomeView(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(true);
        setHapticFeedbackEnabled(true);
        int maxBytes = (int) Math.min(Runtime.getRuntime().maxMemory() / 6, 48L * 1024 * 1024);
        bitmaps = new LruCache<String, Bitmap>(Math.max(maxBytes, 8 * 1024 * 1024)) {
            @Override
            protected int sizeOf(String key, Bitmap value) {
                return value.getAllocationByteCount();
            }
        };
        loader = Executors.newSingleThreadExecutor(runnable -> {
            Thread thread = new Thread(runnable, "lsx4-home-art");
            thread.setDaemon(true);
            thread.setPriority(Thread.NORM_PRIORITY - 1);
            return thread;
        });
        Random random = new Random(8);
        for (int i = 0; i < PARTICLES; i++) {
            seedX[i] = random.nextFloat();
            seedY[i] = random.nextFloat();
            seedRadius[i] = random.nextFloat() * 4f + 2f;
            seedSpeed[i] = random.nextFloat() * 18f + 10f;
            seedDrift[i] = random.nextFloat() * 8f - 4f;
            seedAlpha[i] = random.nextFloat() * 0.4f + 0.35f;
            seedPhase[i] = random.nextFloat() * 10f;
            seedRotation[i] = random.nextFloat() * 360f;
        }
        int[] iconIds = {R.drawable.ic_home_layout, R.drawable.ic_search, R.drawable.ic_sort,
                R.drawable.ic_add, R.drawable.ic_tune};
        for (int i = 0; i < iconIds.length; i++) {
            Drawable icon = context.getDrawable(iconIds[i]);
            functionIcons[i] = icon == null ? null : icon.mutate();
        }
        shimmer.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SCREEN));
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        gestures = new GestureDetector(context, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDown(MotionEvent event) {
                flung = false;
                if (isTiles()) tileGrid.stopMotion();
                return true;
            }

            @Override
            public boolean onScroll(MotionEvent down, MotionEvent move, float dx, float dy) {
                if (items.isEmpty() || activating || pressedFunction >= 0) {
                    return false;
                }
                dragging = true;
                if (isTiles()) {
                    tileGrid.scrollBy((isHorizontal() ? dx : dy) / tilePitch());
                    invalidate();
                    return true;
                }
                float delta = (isHorizontal() ? dx : dy) / pitch();
                if (position < 0f || position > items.size() - 1) {
                    delta *= 0.35f;
                }
                position = Math.max(-0.6f, Math.min(items.size() - 0.4f, position + delta));
                invalidate();
                return true;
            }

            @Override
            public boolean onFling(MotionEvent down, MotionEvent up, float vx, float vy) {
                if (items.isEmpty() || activating || pressedFunction >= 0) {
                    return false;
                }
                flung = true;
                dragging = false;
                quietUntilTarget = false;
                if (isTiles()) {
                    tileGrid.fling(-(isHorizontal() ? vx : vy) / tilePitch());
                    invalidate();
                    return true;
                }
                float itemsPerSecond = -(isHorizontal() ? vx : vy) / pitch();
                target = clampIndex(Math.round(position + itemsPerSecond * 0.3f));
                invalidate();
                return true;
            }

            @Override
            public boolean onSingleTapUp(MotionEvent event) {
                handleTap(event.getX(), event.getY());
                return true;
            }

            @Override
            public void onLongPress(MotionEvent event) {
                int index = hitIndex(event.getX(), event.getY());
                if (index >= 0 && listener != null && !items.get(index).utility) {
                    performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
                    select(index, true);
                    listener.onSettings(items.get(index));
                }
            }
        });
    }



    void setListener(Listener listener) {
        this.listener = listener;
    }

    void setSounds(MenuSoundEngine sounds) {
        this.sounds = sounds;
        updateAmbient();
    }

    void setLayoutMode(int mode) {
        int selected = isTiles() ? tileGrid.selected() : clampIndex(Math.round(position));
        boolean wasTiles = isTiles();
        layout = mode >= HORIZONTAL && mode <= TILES_VERTICAL ? mode : HORIZONTAL;
        if (isTiles()) tileGrid.setItems(items.size(), selected, wasTiles);
        position = selected;
        lastPosition = selected;
        target = selected;
        dragging = false;
        functionFocus = -1;
        invalidate();
    }

    void setSectionMode(boolean value) { sectionMode = value; invalidate(); }

    void selectKey(String key) {
        for (int i = 0; i < items.size(); i++) if (items.get(i).key.equals(key)) {
            select(i, false); position = i; lastPosition = i; announcedIndex = -1; return;
        }
    }

    void repeatSelection() { announcedIndex = -1; invalidate(); }

    void updateArtwork(String key, File image) {
        for (int i = 0; i < items.size(); i++) {
            Item item = items.get(i);
            if (!item.key.equals(key)) continue;
            Item replacement = new Item(item.key, item.title, item.titleId, item.platform,
                    image, image, item.launchable, item.device, item.status, item.tag);
            items.set(i, replacement);
            if (currentItem().key.equals(key)) requestArt(replacement);
            invalidate(); return;
        }
    }

    void setMenuActive(boolean value) {
        menuActive = value;
        if (sounds != null) sounds.setAmbientActive(value);
        if (value) invalidate();
    }

    void setTheme(int index) {
        theme = THEMES[Math.max(0, Math.min(THEMES.length - 1, index))];
        rebuildShaders();
        invalidate();
    }


    void setImmersive(boolean value) {
        immersive = value;
        applyImmersive();
    }


    void setStatus(String message, boolean working) {
        status = message == null ? "" : message;
        busy = working;
        invalidate();
    }


    void setItems(List<Item> next) {
        boolean firstFill = items.size() <= 1;
        String selectedKey = firstFill || currentItem() == null ? null : currentItem().key;
        items.clear();
        items.addAll(next);
        int index = 0;
        for (int i = 0; i < items.size(); i++) {
            if (!items.get(i).utility) { index = i; break; }
        }
        if (selectedKey != null) {
            for (int i = 0; i < items.size(); i++) {
                if (selectedKey.equals(items.get(i).key)) {
                    index = i;
                    break;
                }
            }
        }
        index = clampIndex(index);
        position = index;
        lastPosition = index;
        target = index;
        tileGrid.setItems(items.size(), index, !firstFill && selectedKey != null);
        announcedIndex = -1;
        activating = false;
        activation = 0f;
        invalidate();
    }

    void release() {
        released = true;
        loader.shutdownNow();
        bitmaps.evictAll();
    }


    void playActivation() {
        activating = true;
        activation = 0f;
        activationElapsed = 0f;
        invalidate();
    }



    private Item currentItem() {
        if (items.isEmpty()) {
            return null;
        }
        return items.get(isTiles() ? tileGrid.selected() : clampIndex(Math.round(position)));
    }

    private int clampIndex(int index) {
        return items.isEmpty() ? 0 : Math.max(0, Math.min(items.size() - 1, index));
    }

    private float pitch() {
        if (isTiles()) return tilePitch() * 0.5f;
        if (isLibrary()) return isHorizontal() ? dp(150) : dp(102);
        return isHorizontal() ? cardSlot() : dp(78);
    }

    private boolean isHorizontal() {
        return layout == HORIZONTAL || layout == LIBRARY_HORIZONTAL || layout == TILES_HORIZONTAL;
    }

    private boolean isLibrary() { return layout == LIBRARY_HORIZONTAL || layout == LIBRARY_VERTICAL; }
    private boolean isTiles() { return layout == TILES_HORIZONTAL || layout == TILES_VERTICAL; }

    private float tilePitch() { return isHorizontal() ? dp(202) : dp(140); }

    private float cardBaseWidth() {
        return dp(180);
    }

    private float cardBaseHeight() {
        return dp(110);
    }

    private float cardSlot() {
        return cardBaseWidth() * 1.1f;
    }

    private float cardMaxWidth() {
        return cardBaseWidth() * 1.28f;
    }

    private float cardMaxHeight() {
        return cardBaseHeight() * 1.28f;
    }

    private void select(int index, boolean withSound) {
        index = clampIndex(index);
        if (index != target && withSound && sounds != null) {
            sounds.tick();
        }
        quietUntilTarget = index != clampIndex(Math.round(position));
        target = index;
        if (isTiles()) tileGrid.select(index, true);
        dragging = false;
        invalidate();
    }

    private void handleTap(float x, float y) {
        if (activating) {
            return;
        }
        for (int i = 0; i < functionRects.length; i++) {
            if (functionRects[i].contains(x, y)) {
                activateFunction(i);
                return;
            }
        }
        Item current = currentItem();
        if (playEnabled && playRect.contains(x, y) && current != null) {
            activate(current);
            return;
        }
        if (settingsEnabled && settingsRect.contains(x, y) && current != null) {
            if (sounds != null) sounds.tick();
            if (listener != null) listener.onSettings(current);
            return;
        }
        if (y >= footerTop || items.isEmpty()) {
            return;
        }
        int index = hitIndex(x, y);
        if (index < 0) {
            return;
        }
        if (isTiles()) {
            functionFocus = -1;
            if (index == tileGrid.selected()) {
                tileGrid.stopMotion();
                activate(items.get(index));
            } else {
                if (sounds != null) sounds.tick();
                target = index;
                tileGrid.select(index, false);
                invalidate();
            }
            return;
        }
        if (layout != HORIZONTAL) {
            select(index, true);
            activate(items.get(index));
            return;
        }
        if (index == clampIndex(Math.round(position)) && Math.abs(position - index) < 0.2f) {
            activate(items.get(index));
        } else {
            select(index, true);
        }
    }

    private void activateFunction(int function) {
        if (sounds != null) sounds.tick();
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        if (listener != null) {
            listener.onFunction(function, new RectF(functionRects[function]));
        }
    }

    private void activate(Item item) {
        if (!item.utility && !item.launchable) {
            if (sounds != null) sounds.deny();
            if (!item.utility && listener != null) {
                listener.onActivate(item);
            }
            return;
        }
        if (sounds != null) sounds.confirm();
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        if (listener != null) {
            listener.onActivate(item);
        }
    }

    private int hitIndex(float x, float y) {
        for (int i = hitRects.size() - 1; i >= 0; i--) {
            if (hitRects.get(i).contains(x, y)) {
                return hitIndices.get(i);
            }
        }
        return -1;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            pressedFunction = -1;
            for (int i = 0; i < functionRects.length; i++) {
                if (functionRects[i].contains(event.getX(), event.getY())) {
                    pressedFunction = i;
                }
            }
        }
        boolean handled = gestures.onTouchEvent(event);
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            pressedFunction = -1;
            if (dragging && !flung) {
                dragging = false;
                if (!isTiles()) target = clampIndex(Math.round(position));
            }
            if (action == MotionEvent.ACTION_CANCEL && isTiles()) tileGrid.stopMotion();
            invalidate();
        }
        if (action == MotionEvent.ACTION_DOWN && getParent() != null) {
            getParent().requestDisallowInterceptTouchEvent(true);
        }
        return handled || super.onTouchEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (activating) {
            return super.onKeyDown(keyCode, event);
        }
        boolean confirm = keyCode == KeyEvent.KEYCODE_ENTER
                || keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER
                || keyCode == KeyEvent.KEYCODE_DPAD_CENTER
                || keyCode == KeyEvent.KEYCODE_BUTTON_A;

        int towardFunctions = KeyEvent.KEYCODE_DPAD_DOWN;
        int awayFromFunctions = KeyEvent.KEYCODE_DPAD_UP;
        if (keyCode == KeyEvent.KEYCODE_BUTTON_START) {
            functionFocus = functionFocus < 0 ? 0 : -1;
            invalidate();
            return true;
        }
        if (functionFocus >= 0) {
            if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT) {
                moveFunctionFocus(-1);
                return true;
            }
            if (keyCode == KeyEvent.KEYCODE_DPAD_RIGHT) {
                moveFunctionFocus(1);
                return true;
            }
            if (keyCode == awayFromFunctions || keyCode == KeyEvent.KEYCODE_BUTTON_B) {
                functionFocus = -1;
                if (sounds != null) sounds.softTick();
                invalidate();
                return true;
            }
            if (confirm) {
                activateFunction(functionFocus);
                return true;
            }
            return super.onKeyDown(keyCode, event);
        }
        if (keyCode == towardFunctions && !isTiles()
                && (isHorizontal() || items.isEmpty() || target == items.size() - 1)) {
            functionFocus = 0;
            if (sounds != null) sounds.softTick();
            invalidate();
            return true;
        }
        if (items.isEmpty()) {
            return super.onKeyDown(keyCode, event);
        }
        if (isTiles()) {
            int step = tileNavigationStep(keyCode, target);
            if (step != 0) { select(target + step, true); return true; }
            if (keyCode == KeyEvent.KEYCODE_DPAD_DOWN && target == items.size() - 1) {
                functionFocus = 0;
                invalidate();
                return true;
            }
        }
        boolean previous = !isTiles() && (isHorizontal() ? keyCode == KeyEvent.KEYCODE_DPAD_LEFT
                : keyCode == KeyEvent.KEYCODE_DPAD_UP);
        boolean next = !isTiles() && (isHorizontal() ? keyCode == KeyEvent.KEYCODE_DPAD_RIGHT
                : keyCode == KeyEvent.KEYCODE_DPAD_DOWN);
        if (previous) {
            select(target - 1, true);
            return true;
        }
        if (next) {
            select(target + 1, true);
            return true;
        }
        if (confirm) {
            activate(items.get(clampIndex(target)));
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_Y || keyCode == KeyEvent.KEYCODE_MENU) {
            Item item = items.get(clampIndex(target));
            if (!item.utility && listener != null) {
                listener.onSettings(item);
            }
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    private void moveFunctionFocus(int delta) {
        int next = Math.max(0, Math.min(functionRects.length - 1, functionFocus + delta));
        if (next != functionFocus) {
            functionFocus = next;
            if (sounds != null) sounds.softTick();
            invalidate();
        }
    }



    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        lastFrameNanos = 0L;
        updateAmbient();
        invalidate();
    }

    @Override
    protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        lastFrameNanos = 0L;
        updateAmbient();
        invalidate();
    }

    @Override
    public void onWindowFocusChanged(boolean hasWindowFocus) {
        super.onWindowFocusChanged(hasWindowFocus);
        if (hasWindowFocus) {
            applyImmersive();
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        if (sounds != null) {
            sounds.setAmbientActive(false);
        }
    }

    private void applyImmersive() {
        if (!immersive || !isShown()) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindowInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
            }
        } else {
            setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    private void updateAmbient() {
        if (sounds != null) {
            sounds.setAmbientActive(menuActive && isShown() && getWindowVisibility() == VISIBLE && !released);
        }
    }

    private void updateInsets() {
        WindowInsets insets = getRootWindowInsets();
        if (insets == null) {
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            android.graphics.Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            insetTop = bars.top;
            insetBottom = bars.bottom;
        } else {
            insetTop = insets.getSystemWindowInsetTop();
            insetBottom = insets.getSystemWindowInsetBottom();
        }
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        super.onSizeChanged(w, h, oldW, oldH);
        rebuildShaders();
    }

    private void rebuildShaders() {
        shimmerGradient = new LinearGradient(0, 0, dp(90), dp(140),
                new int[]{0x00ffffff, 0x66ffffff, 0x00ffffff},
                new float[]{0f, 0.5f, 1f}, Shader.TileMode.CLAMP);
        glow = new SweepGradient(0, 0,
                new int[]{0x00ffffff, theme.glow, 0xffffffff, theme.glow, 0x00ffffff},
                new float[]{0f, 0.18f, 0.25f, 0.32f, 1f});
    }



    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        long now = System.nanoTime();
        float dt = lastFrameNanos == 0L ? 0.016f
                : Math.min(0.05f, (now - lastFrameNanos) / 1_000_000_000f);
        lastFrameNanos = now;
        time += dt;
        if (time > 10_000f) {
            time -= 10_000f;
        }
        updateInsets();
        refreshSystemStatus(false);
        step(dt);

        int w = getWidth();
        int h = getHeight();
        drawBackground(canvas, w, h);
        hitRects.clear();
        hitIndices.clear();
        playEnabled = false;
        settingsEnabled = false;
        footerTop = Float.MAX_VALUE;
        if (items.isEmpty()) {
            text.setTypeface(light);
            text.setTextSize(sp(16));
            text.setColor(0xccffffff);
            text.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(status.isEmpty()
                            ? getContext().getString(R.string.library_empty_status) : status,
                    w * 0.5f, h * 0.45f, text);
            text.setTextAlign(Paint.Align.LEFT);
        } else if (isLibrary()) {
            drawLibrary(canvas, w, h);
        } else if (isTiles()) {
            drawTiles(canvas, w, h);
        } else if (layout == HORIZONTAL) {
            drawHorizontal(canvas, w, h);
        } else {
            drawVertical(canvas, w, h);
        }
        if (!sectionMode) drawDock(canvas, w, h);
        drawSystemStatus(canvas, w, insetTop + dp(18));
        if (activating) {
            drawActivation(canvas, w, h);
        }
        if (!released && menuActive && isShown() && getWindowVisibility() == VISIBLE) {
            postInvalidateOnAnimation();
        }
    }

    private void step(float dt) {
        if (isTiles()) {
            if (!dragging) tileGrid.step(dt);
            position = tileGrid.selected();
        } else if (!dragging) {
            float k = 1f - (float) Math.exp(-dt * 11f);
            position += (target - position) * k;
            if (Math.abs(target - position) < 0.0005f) {
                position = target;
            }
        }
        float velocity = dt > 0f ? (position - lastPosition) / dt : 0f;
        lastPosition = position;
        float tiltTarget = Math.max(-12f, Math.min(12f, -velocity * 4.5f));
        tilt += (tiltTarget - tilt) * (1f - (float) Math.exp(-dt * 9f));
        if (activating) {
            activation = Math.min(1f, activation + dt / 0.42f);
            activationElapsed += dt;
            if (activationElapsed > 1.6f) {

                activating = false;
                activation = 0f;
            }
        }
        if (!items.isEmpty()) {
            int index = isTiles() ? tileGrid.selected() : clampIndex(Math.round(position));
            if (index != announcedIndex) {
                if (announcedIndex >= 0 && !quietUntilTarget && sounds != null) {
                    sounds.tick();
                }
                if (index == target) {
                    quietUntilTarget = false;
                }
                announcedIndex = index;
                Item item = items.get(index);
                setContentDescription(item.title);
                requestArt(item);
                if (listener != null) listener.onSelected(item);
                if (item.device) {
                    refreshDeviceInfo(false);
                }
            }
        }
        artFade = Math.min(1f, artFade + dt / 0.45f);
    }



    private void drawBackground(Canvas canvas, int w, int h) {
        if (hasGameBackground()) {


            canvas.drawColor(0xff000000);
            float zoom = 1.08f + 0.04f * (float) Math.sin(time * 0.22f);
            float panX = (float) Math.sin(time * 0.11f) * w * 0.02f;
            fill.setShader(null);
            if (artPrevious != null && artFade < 1f) {
                drawCover(canvas, artPrevious, w, h, zoom, panX, 1f);
            }
            if (artCurrent != null) {
                drawCover(canvas, artCurrent, w, h, zoom, panX, artFade);
            }
            fill.setShader(new LinearGradient(0, 0, 0, h,
                    new int[]{0x26000000, 0x40000000, 0x80000000}, null,
                    Shader.TileMode.CLAMP));
            canvas.drawRect(0, 0, w, h, fill);
            fill.setShader(null);
            return;
        }
        fill.setShader(new LinearGradient(0, 0, 0, h,
                new int[]{withAlpha(theme.gradientStart, 0.8f), theme.background,
                        theme.gradientEnd, theme.background, withAlpha(theme.gradientStart, 0.8f)},
                null, Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, fill);
        fill.setShader(new RadialGradient(w * 0.5f, h * 0.5f, Math.max(w, h) * 0.7f,
                withAlpha(theme.primary, 0.15f), 0x00000000, Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, fill);
        fill.setShader(null);

        fill.setShader(new LinearGradient(0, 0, 0, h,
                new int[]{0x1a000000, 0x33000000, 0x66000000},
                null, Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, fill);
        fill.setShader(new LinearGradient(0, 0, 0, h,
                new int[]{withAlpha(theme.primary, 0.06f), withAlpha(theme.background, 0.12f),
                        withAlpha(theme.primary, 0.08f)}, null, Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, fill);
        fill.setShader(null);

        drawRibbons(canvas, w, h);
        drawParticles(canvas, w, h);

        fill.setShader(new RadialGradient(w * 0.5f, h * 0.5f, Math.max(w, h) * 0.9f,
                0x00000000, 0x66000000, Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, h, fill);
        fill.setShader(null);
    }

    private int tileNavigationStep(int key, int index) {
        int backward = isHorizontal() ? KeyEvent.KEYCODE_DPAD_LEFT : KeyEvent.KEYCODE_DPAD_UP;
        int forward = isHorizontal() ? KeyEvent.KEYCODE_DPAD_RIGHT : KeyEvent.KEYCODE_DPAD_DOWN;
        int laneBack = isHorizontal() ? KeyEvent.KEYCODE_DPAD_UP : KeyEvent.KEYCODE_DPAD_LEFT;
        int laneForward = isHorizontal() ? KeyEvent.KEYCODE_DPAD_DOWN : KeyEvent.KEYCODE_DPAD_RIGHT;
        if (key == backward) return index >= 2 ? -2 : 0;
        if (key == forward) return index + 2 < items.size() ? 2 : 0;
        if (key == laneBack) return index % 2 == 1 ? -1 : 0;
        if (key == laneForward) return index % 2 == 0 && index + 1 < items.size() ? 1 : 0;
        return 0;
    }

    private boolean hasGameBackground() {
        return currentItem() != null && !currentItem().utility
                && (artCurrent != null || (artPrevious != null && artFade < 1f));
    }

    private void drawCover(Canvas canvas, Bitmap bitmap, int w, int h, float zoom, float panX,
                           float alpha) {
        float scale = Math.max((float) w / bitmap.getWidth(), (float) h / bitmap.getHeight())
                * zoom;
        float bw = bitmap.getWidth() * scale;
        float bh = bitmap.getHeight() * scale;
        rect.set((w - bw) * 0.5f + panX, (h - bh) * 0.5f, (w + bw) * 0.5f + panX,
                (h + bh) * 0.5f);
        fill.setAlpha((int) (255 * Math.max(0f, Math.min(1f, alpha))));
        canvas.drawBitmap(bitmap, null, rect, fill);
        fill.setAlpha(255);
    }


    private void drawRibbons(Canvas canvas, int w, int h) {
        float[] baseY = {0.48f, 0.52f, 0.56f};
        float[] amplitude = {0.08f, 0.10f, 0.07f};
        float[] frequency = {1.0f, 0.8f, 1.2f};
        float[] speed = {0.35f, 0.30f, 0.40f};
        float[] width = {dp(27), dp(33), dp(20)};
        float[] alpha = {0.25f, 0.20f, 0.22f};
        float[] widths = {2.5f, 1.5f, 0.8f, 0.3f};
        float[] alphas = {0.15f * 0.4f, 0.25f * 0.6f, 0.4f * 0.8f, 0.3f};
        int segments = 50;
        float segment = (w + dp(66)) / segments;
        for (int r = 0; r < ribbons.length; r++) {
            Path ribbon = ribbons[r];
            ribbon.reset();
            float phase = time * speed[r];
            for (int i = 0; i <= segments; i++) {
                float x = -dp(33) + i * segment;
                float nx = x / w;
                float y = baseY[r] * h
                        + (float) Math.sin(nx * frequency[r] * 6.283f + phase) * amplitude[r] * h
                        + (float) Math.sin(nx * frequency[r] * 3.1415f + phase * 1.3f)
                        * amplitude[r] * h * 0.3f;
                if (i == 0) {
                    ribbon.moveTo(x, y);
                } else {
                    ribbon.lineTo(x, y);
                }
            }
            for (int s = 0; s < widths.length; s++) {
                stroke.setStrokeWidth(width[r] * widths[s]);
                stroke.setColor(theme.primary);
                stroke.setAlpha((int) (255 * alpha[r] * alphas[s]));
                canvas.drawPath(ribbon, stroke);
            }
        }
        stroke.setAlpha(255);
    }


    private void drawParticles(Canvas canvas, int w, int h) {
        float multiplier = 2f;
        float scale = density * 0.5f;
        for (int i = 0; i < PARTICLES; i++) {
            float radius = seedRadius[i] * multiplier * scale;
            float t = time + seedPhase[i];
            float x = wrap(seedX[i] * w + t * seedDrift[i] * scale, w + radius * 2f) - radius;
            float y = wrap(seedY[i] * h + t * seedSpeed[i] * scale, h + radius * 2f) - radius;
            float twinkle = 0.7f + 0.3f * (float) Math.sin(t * 0.8f);
            float rotation = seedRotation[i] + t * 20f;
            androidGlyph.draw(canvas, x, y, radius * 3f, rotation,
                    withAlpha(theme.primary, seedAlpha[i] * twinkle * 0.45f));
        }
        fill.setAlpha(255);
    }

    private void buildParticleShape(float cx, float cy, float size) {
        shape.reset();
        if (theme.particle == PARTICLE_PETAL) {
            shape.moveTo(cx, cy - size);
            shape.quadTo(cx + size * 0.8f, cy - size * 0.3f, cx, cy + size * 0.5f);
            shape.quadTo(cx - size * 0.8f, cy - size * 0.3f, cx, cy - size);
            shape.close();
            return;
        }
        shape.moveTo(cx, cy - size);
        shape.lineTo(cx + size * 0.25f, cy - size * 0.6f);
        shape.lineTo(cx + size * 0.7f, cy - size * 0.5f);
        shape.lineTo(cx + size * 0.4f, cy - size * 0.2f);
        shape.lineTo(cx + size * 0.8f, cy + size * 0.1f);
        shape.lineTo(cx + size * 0.3f, cy + size * 0.4f);
        shape.lineTo(cx + size * 0.1f, cy + size * 0.7f);
        shape.lineTo(cx, cy + size * 0.9f);
        shape.lineTo(cx - size * 0.1f, cy + size * 0.7f);
        shape.lineTo(cx - size * 0.3f, cy + size * 0.4f);
        shape.lineTo(cx - size * 0.8f, cy + size * 0.1f);
        shape.lineTo(cx - size * 0.4f, cy - size * 0.2f);
        shape.lineTo(cx - size * 0.7f, cy - size * 0.5f);
        shape.lineTo(cx - size * 0.25f, cy - size * 0.6f);
        shape.close();
    }



    private void drawHorizontal(Canvas canvas, int w, int h) {
        float baseW = cardBaseWidth();
        float baseH = cardBaseHeight();
        float maxW = cardMaxWidth();
        float maxH = cardMaxHeight();
        float slot = cardSlot();
        float focusX = dp(130) + slot * 0.5f;
        float rowY = Math.max(insetTop + maxH * 0.5f + dp(14), h * 0.5f - dp(100));
        lastFocusX = focusX;
        lastFocusY = rowY;


        float bandStart = focusX - dp(60);
        if (!hasGameBackground()) {
            fill.setShader(new LinearGradient(bandStart, 0, bandStart + slot * 0.9f, 0,
                    new int[]{0x00000000, withAlpha(theme.primary, 0.22f), 0x00000000}, null,
                    Shader.TileMode.CLAMP));
            canvas.drawRect(bandStart, 0, bandStart + slot * 0.9f, h, fill);
            fill.setShader(null);
        }

        int first = Math.max(0, (int) Math.floor(position - focusX / slot) - 2);
        int last = Math.min(items.size() - 1, (int) Math.ceil(position + (w - focusX) / slot) + 2);
        int focused = clampIndex(Math.round(position));
        for (int i = first; i <= last; i++) {
            if (i != focused) {
                drawCardAt(canvas, i, baseW, baseH, maxW, maxH, slot, focusX, rowY, w);
            }
        }
        drawCardAt(canvas, focused, baseW, baseH, maxW, maxH, slot, focusX, rowY, w);

        float settle = 1f - Math.min(1f, Math.abs(position - focused) * 3f);
        float infoLeft = focusX - maxW * 0.5f;
        float infoTop = rowY + maxH * 0.5f + dp(22);
        float infoWidth = w - infoLeft - dp(24);
        Item item = items.get(focused);
        if (item.device) {
            drawDeviceInfoColumns(canvas, infoLeft, infoTop, infoWidth, settle);
        } else if (!sectionMode && !item.utility) {
            drawGameDetails(canvas, item, infoLeft, infoTop, infoWidth, settle);
        }
    }

    private void drawCardAt(Canvas canvas, int index, float baseW, float baseH, float maxW,
                            float maxH, float slot, float focusX, float rowY, int w) {
        float d = index - position;
        float s = selection(d);
        float cardW = lerp(baseW, maxW, s);
        float cardH = lerp(baseH, maxH, s);
        float x = focusX + d * slot + clamp(d, -1f, 1f) * (maxW - slot) * 0.5f;
        if (x + cardW < -dp(40) || x - cardW > w + dp(40)) {
            return;
        }
        float pulse = 1f + 0.012f * s * (0.5f + 0.5f * (float) Math.sin(time * 2.85f));
        float rotateY = -clamp(d, -1f, 1f) * 7f * s + tilt * 0.6f;
        float rotateX = (float) Math.sin(time * 1.5f) * 1.5f * s;
        drawCard(canvas, items.get(index), x, rowY, cardW * pulse, cardH * pulse, s, rotateY,
                rotateX, -clamp(d, -1f, 1f));
        rect.set(x - cardW * 0.5f, rowY - cardH * 0.5f, x + cardW * 0.5f, rowY + cardH * 0.5f);
        hitRects.add(new RectF(rect));
        hitIndices.add(index);
    }


    private void drawCard(Canvas canvas, Item item, float cx, float cy, float cardW, float cardH,
                          float s, float rotateY, float rotateX, float focusOffset) {
        float radius = dp(6);
        canvas.save();
        applyTilt(canvas, cx, cy, rotateY, rotateX);
        tile.set(cx - cardW * 0.5f, cy - cardH * 0.5f, cx + cardW * 0.5f, cy + cardH * 0.5f);
        if (s > 0.02f) {
            fill.setColor(Color.BLACK);
            fill.setAlpha((int) (120 * s));
            canvas.drawRoundRect(tile.left + dp(3), tile.top + dp(8), tile.right - dp(3),
                    tile.bottom + dp(14), radius, radius, fill);
            stroke.setColor(theme.highlight);
            stroke.setStrokeWidth(dp(2));
            stroke.setAlpha((int) (255 * 0.18f * s
                    * (0.7f + 0.3f * (0.5f + 0.5f * (float) Math.sin(time * 1.43f)))));
            canvas.drawRoundRect(tile.left - dp(3), tile.top - dp(3), tile.right + dp(3),
                    tile.bottom + dp(3), radius + dp(3), radius + dp(3), stroke);
            stroke.setAlpha(255);
        }
        float alpha = 0.6f + 0.4f * s;
        if (item.utility) {
            drawUtilityCardFace(canvas, item, tile, radius, alpha);
        } else {
            Bitmap cover = coverFor(item);
            if (cover != null) {
                float parallax = dp(10) * focusOffset * s;
                drawRoundedBitmap(canvas, cover, tile, radius, alpha, 1f + 0.07f * s, parallax);
            } else {
                drawPlaceholder(canvas, item, tile, radius, alpha);
            }
            if (item.platform != null) {
                drawBadge(canvas, item.platform, tile.right - dp(6), tile.bottom - dp(6),
                        Paint.Align.RIGHT, alpha, 0.85f);
            }
        }
        if (s > 0.05f) {
            float cycle = (time % 2.6f) / 2.6f;
            float sweep = cardW * 0.9f;
            matrix.reset();
            matrix.setScale(sweep / dp(90), cardH / dp(140));
            matrix.postTranslate(tile.left - sweep + cycle * (cardW + sweep), tile.top);
            shimmerGradient.setLocalMatrix(matrix);
            shimmer.setShader(shimmerGradient);
            shimmer.setAlpha((int) (255 * (0.16f + 0.12f * Math.min(1f, Math.abs(tilt) / 8f)) * s));
            canvas.drawRoundRect(tile, radius, radius, shimmer);
            shimmer.setShader(null);

            matrix.reset();
            matrix.setRotate((time * 70f) % 360f);
            matrix.postTranslate(cx, cy);
            glow.setLocalMatrix(matrix);
            stroke.setShader(glow);
            stroke.setStrokeWidth(dp(2));
            stroke.setAlpha((int) (255 * s));
            canvas.drawRoundRect(tile, radius, radius, stroke);
            stroke.setShader(null);
            stroke.setAlpha(255);
        } else {
            stroke.setColor(0x26ffffff);
            stroke.setStrokeWidth(dp(1));
            canvas.drawRoundRect(tile, radius, radius, stroke);
        }
        if (!item.utility && item.status != STATUS_UNCLASSIFIED) {
            fill.setColor(item.status == STATUS_SUPPORTED ? SUPPORTED : UNSUPPORTED);
            fill.setAlpha((int) (255 * alpha));
            canvas.drawCircle(tile.right - dp(8), tile.top + dp(8), dp(3.5f), fill);
            fill.setAlpha(255);
        }
        canvas.restore();
    }

    private void drawUtilityCardFace(Canvas canvas, Item item, RectF bounds, float radius, float alpha) {
        fill.setShader(new LinearGradient(bounds.left, bounds.top, bounds.right, bounds.bottom,
                blend(theme.gradientStart, 0xff3a2a7a, 0.55f), 0xff120d24, Shader.TileMode.CLAMP));
        fill.setAlpha((int) (255 * alpha));
        canvas.drawRoundRect(bounds, radius, radius, fill);
        fill.setShader(null);
        float circle = bounds.height() * 0.2f;
        float cx = bounds.centerX();
        float cy = bounds.top + bounds.height() * 0.4f;
        fill.setColor(0xff7752c8);
        fill.setAlpha((int) (255 * alpha));
        androidGlyph.draw(canvas, cx, cy, circle * 2f, 0f, withAlpha(0xffa4d879, alpha));
        text.setColor(0xffffffff);
        text.setAlpha((int) (255 * alpha));
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(regular);
        text.setTextSize(Math.min(sp(15), bounds.height() * 0.13f));
        canvas.drawText(ellipsize(item.title, text,
                bounds.width() - dp(12)), cx, bounds.top + bounds.height() * 0.8f, text);
        text.setTextAlign(Paint.Align.LEFT);
        fill.setAlpha(255);
    }

    private void drawGameDetails(Canvas canvas, Item item, float left, float top, float width,
                                 float alpha) {
        int a = (int) (255 * alpha);
        text.setTypeface(medium);
        float titleSize = sp(18);
        text.setTextSize(titleSize);
        while (text.measureText(item.title) > width && titleSize > sp(14)) {
            titleSize -= sp(1);
            text.setTextSize(titleSize);
        }
        text.setColor(0xffffffff);
        text.setAlpha(a);
        float y = top + titleSize;
        canvas.drawText(ellipsize(item.title, text, width), left, y, text);
        y += dp(10);
        float x = left;
        if (item.platform != null) {
            x = drawBadge(canvas, item.platform, x, y + dp(16), Paint.Align.LEFT, alpha, 1f)
                    + dp(8);
        }
        text.setTypeface(regular);
        text.setTextSize(sp(13));
        text.setColor(0xb3ffffff);
        text.setAlpha((int) (180 * alpha));
        canvas.drawText(item.titleId, x, y + dp(12), text);
        x += text.measureText(item.titleId) + dp(12);

        String statusLabel = item.status == STATUS_SUPPORTED
                ? getContext().getString(R.string.game_status_supported)
                : item.status == STATUS_UNSUPPORTED
                ? getContext().getString(R.string.game_status_unsupported) : "";
        if (!item.launchable) {
            String metadata = getContext().getString(R.string.metadata_only);
            statusLabel = statusLabel.isEmpty() ? metadata : statusLabel + " · " + metadata;
        }
        if (!statusLabel.isEmpty()) {
            int statusColor = item.status == STATUS_SUPPORTED ? SUPPORTED
                    : item.status == STATUS_UNSUPPORTED ? UNSUPPORTED : 0xff9aa6b2;
            fill.setColor(statusColor);
            fill.setAlpha(a);
            canvas.drawCircle(x + dp(4), y + dp(8), dp(3.5f), fill);
            text.setColor(statusColor);
            text.setAlpha(a);
            canvas.drawText(ellipsize(statusLabel, text, left + width - x - dp(14)), x + dp(13),
                    y + dp(12), text);
        }
        y += dp(26);
        float buttonHeight = dp(30);
        playRect.set(left, y, left + dp(96), y + buttonHeight);
        settingsRect.set(playRect.right + dp(8), y, playRect.right + dp(8) + buttonHeight,
                y + buttonHeight);
        drawPlayButton(canvas, playRect, item.launchable, alpha);
        drawRoundButton(canvas, settingsRect, "☰", alpha);
        playEnabled = alpha > 0.5f;
        settingsEnabled = alpha > 0.5f;
    }

    private void drawDeviceInfoColumns(Canvas canvas, float left, float top, float width,
                                       float alpha) {
        int columns = width > dp(560) ? 3 : 2;
        float gap = dp(28);
        float columnWidth = (width - gap) / columns;
        int perColumn = (deviceLines.size() + columns - 1) / columns;
        for (int c = 0; c < columns; c++) {
            int from = c * perColumn;
            int to = Math.min(deviceLines.size(), from + perColumn);
            if (from < to) {
                drawInfoLines(canvas, deviceLines.subList(from, to),
                        left + c * (columnWidth + gap), top - dp(12), columnWidth, alpha);
            }
        }
    }



    private void drawLibrary(Canvas canvas, int w, int h) {
        float bottom = h - insetBottom - dp(sectionMode ? 8 : 84);
        boolean horizontal = isHorizontal();
        float side = horizontal ? dp(94) : dp(18);
        float heroTop = contentTop() + dp(8);
        float heroHeight = horizontal ? dp(78) : dp(104);
        int focused = clampIndex(Math.round(position));
        if (horizontal) {
            fill.setColor(0xc0101822);
            canvas.drawRect(0, 0, side - dp(14), h, fill);
            androidGlyph.draw(canvas, dp(34), contentTop() + dp(30), dp(32), 0, 0xff80c7b5);
            text.setTypeface(medium);
            text.setTextSize(sp(10));
            text.setColor(0xffd8e5ee);
            canvas.drawText("LSX4", dp(18), contentTop() + dp(66), text);
        }
        drawHeroText(canvas, items.get(focused), side, heroTop, w - side - dp(24), heroHeight,
                R.string.home_style_library);
        float top = heroTop + heroHeight + dp(16);
        canvas.save();
        canvas.clipRect(side, top, w, bottom);
        if (horizontal) {
            float cardH = Math.max(dp(72), Math.min(dp(176), bottom - top - dp(10)));
            float cardW = dp(136);
            float center = side + cardW * 0.5f + dp(8);
            for (int i = 0; i < items.size(); i++) {
                float x = center + (i - position) * pitch();
                if (x + cardW * 0.5f < side || x - cardW * 0.5f > w) continue;
                RectF bounds = new RectF(x - cardW * 0.5f, top + dp(4), x + cardW * 0.5f,
                        top + dp(4) + cardH);
                drawMenuPanel(canvas, items.get(i), bounds, i == focused, false);
                recordHit(bounds, i, side, top, w, bottom);
                if (i == focused) lastFocusX = x;
            }
            lastFocusY = top + cardH * 0.5f;
        } else {
            float focusY = top + dp(46);
            for (int i = 0; i < items.size(); i++) {
                float y = focusY + (i - position) * pitch();
                if (y + dp(46) < top || y - dp(46) > bottom) continue;
                RectF bounds = new RectF(side, y - dp(44), w - dp(18), y + dp(44));
                fill.setColor(i == focused ? 0xdd182938 : 0xa6101822);
                canvas.drawRoundRect(bounds, dp(8), dp(8), fill);
                drawTile(canvas, items.get(i), side + dp(40), y, dp(72), i == focused ? 1 : 0,
                        0, 0);
                text.setTypeface(i == focused ? medium : regular);
                text.setTextSize(sp(16));
                text.setColor(0xffffffff);
                float titleLeft = side + dp(86);
                float titleWidth = bounds.right - titleLeft - (i == focused ? dp(48) : dp(8));
                List<String> lines = wrap(items.get(i).title, text, titleWidth, 2);
                float baseline = y - (lines.size() - 1) * sp(9) + sp(5);
                for (String line : lines) { drawReadableTitle(canvas, line, titleLeft, baseline); baseline += sp(19); }
                if (i == focused) {
                    lastFocusX = side + dp(40);
                    lastFocusY = y;
                    drawPanelSettings(canvas, items.get(i), bounds);
                }
                recordHit(bounds, i, side, top, w, bottom);
            }
        }
        canvas.restore();
    }

    private void drawTiles(Canvas canvas, int w, int h) {
        float left = dp(22);
        float top = contentTop() + dp(60);
        float bottom = h - insetBottom - dp(84);
        int focused = tileGrid.selected();
        drawHeroText(canvas, items.get(focused), left, contentTop(), w - left - dp(20), dp(50),
                R.string.home_style_tiles);
        float width = isHorizontal() ? dp(190) : (w - left * 2f - dp(12)) * 0.5f;
        float height = isHorizontal() ? Math.min(dp(124), (bottom - top - dp(12)) * 0.5f)
                : dp(128);
        tileGrid.setViewport((isHorizontal() ? w - left : bottom - top) / tilePitch(),
                (isHorizontal() ? width : height) / tilePitch());
        float group = tileGrid.offset();
        canvas.save();
        canvas.clipRect(left, top, w, bottom);
        for (int i = 0; i < items.size(); i++) {
            float along = (i / 2 - group) * tilePitch();
            float x = isHorizontal() ? left + along : left + (i % 2) * (width + dp(12));
            float y = isHorizontal() ? top + (i % 2) * (height + dp(12)) : top + along;
            if (x + width < left || x > w || y + height < top || y > bottom) continue;
            RectF bounds = new RectF(x, y, x + width, y + height);
            drawMenuPanel(canvas, items.get(i), bounds, i == focused, true);
            recordHit(bounds, i, left, top, w, bottom);
            if (i == focused) { lastFocusX = bounds.centerX(); lastFocusY = bounds.centerY(); }
        }
        canvas.restore();
        if (settingsEnabled) settingsEnabled = settingsRect.intersect(left, top, w, bottom);
    }

    private void drawHeroText(Canvas canvas, Item item, float left, float top, float width,
                              float height, int label) {
        fill.setColor(0xa6000000);
        canvas.drawRoundRect(left - dp(8), top - dp(4), left + width + dp(8), top + height,
                dp(8), dp(8), fill);
        text.setTypeface(medium);
        text.setTextSize(sp(11));
        text.setColor(isTiles() ? 0xffa9dc6e : 0xffa8d8ef);
        canvas.drawText(getContext().getString(label).toUpperCase(Locale.getDefault()), left,
                top + sp(12), text);
        text.setTextSize(sp(isHorizontal() ? 23 : 20));
        text.setColor(0xffffffff);
        drawReadableTitle(canvas, ellipsize(item.title, text, width), left, top + sp(39));
        if (height > dp(65)) {
            text.setTypeface(regular);
            text.setTextSize(sp(12));
            text.setColor(0xffc7d2db);
            String detail = item.device ? getContext().getString(R.string.home_device_subtitle)
                    : item.titleId;
            if (!item.utility && item.status != STATUS_UNCLASSIFIED) {
                detail += " · " + getContext().getString(item.status == STATUS_SUPPORTED
                        ? R.string.game_status_supported : R.string.game_status_unsupported);
            }
            canvas.drawText(ellipsize(detail, text, width), left, top + sp(59), text);
        }
    }

    private void drawMenuPanel(Canvas canvas, Item item, RectF bounds, boolean selected,
                               boolean dashboard) {
        float radius = dp(dashboard ? 3 : 8);
        Bitmap cover = item.utility ? null : coverFor(item);
        if (item.utility) drawUtilityCardFace(canvas, item, bounds, radius, 1f);
        else if (cover != null) drawRoundedBitmap(canvas, cover, bounds, radius, 1f, 1f, 0f);
        else drawPlaceholder(canvas, item, bounds, radius, 1f);
        fill.setColor(0xcc000000);
        float captionH = Math.min(bounds.height() * 0.48f, dp(46));
        canvas.drawRect(bounds.left, bounds.bottom - captionH, bounds.right, bounds.bottom, fill);
        text.setTypeface(selected ? medium : regular);
        text.setTextSize(sp(12));
        text.setColor(0xffffffff);
        float titleWidth = bounds.width() - dp(selected && !item.utility ? 48 : 12);
        List<String> lines = wrap(item.title, text, titleWidth, 2);
        float y = bounds.bottom - captionH + sp(13);
        for (String line : lines) { drawReadableTitle(canvas, line, bounds.left + dp(6), y); y += sp(14); }
        if (!item.utility && item.platform != null) drawBadge(canvas, item.platform,
                bounds.left + dp(5), bounds.top + dp(23), Paint.Align.LEFT, 1f, 0.8f);
        if (selected) {
            stroke.setShader(null);
            stroke.setColor(dashboard ? 0xff9bdd48 : 0xffb2e8ff);
            stroke.setAlpha(255);
            stroke.setStrokeWidth(dp(2));
            canvas.drawRoundRect(bounds, radius, radius, stroke);
            drawPanelSettings(canvas, item, bounds);
        }
    }

    private void drawPanelSettings(Canvas canvas, Item item, RectF bounds) {
        if (item.utility || sectionMode) return;
        settingsRect.set(bounds.right - dp(40), bounds.bottom - dp(40),
                bounds.right - dp(4), bounds.bottom - dp(4));
        drawRoundButton(canvas, settingsRect, "☰", 1f);
        settingsEnabled = true;
    }

    private void recordHit(RectF bounds, int index, float left, float top, float right, float bottom) {
        RectF hit = new RectF(bounds);
        if (hit.intersect(left, top, right, bottom)) {
            hitRects.add(hit);
            hitIndices.add(index);
        }
    }


    private void drawReadableTitle(Canvas canvas, String value, float x, float baseline) {
        titleOutline.set(text);
        titleOutline.setColor(Color.BLACK);
        titleOutline.setAlpha(text.getAlpha());
        titleOutline.setStyle(Paint.Style.STROKE);
        titleOutline.setStrokeJoin(Paint.Join.ROUND);
        titleOutline.setStrokeWidth(dp(2.5f));
        canvas.drawText(value, x, baseline, titleOutline);
        canvas.drawText(value, x, baseline, text);
    }



    private float contentTop() {
        return insetTop + dp(40);
    }

    private void drawVertical(Canvas canvas, int w, int h) {
        float baseIcon = dp(54);
        float selIcon = dp(86);
        float pitch = pitch();
        float focusY = contentTop() + dp(64);
        float left = dp(22);
        lastFocusX = left + selIcon * 0.5f;
        lastFocusY = focusY;
        int focused = clampIndex(Math.round(position));
        float settle = 1f - Math.min(1f, Math.abs(position - focused) * 3f);

        int deviceIndex = -1;
        for (int i = 0; i < items.size(); i++) {
            if (items.get(i).device) {
                deviceIndex = i;
                break;
            }
        }
        float devicePanel = deviceIndex >= 0
                ? (Math.min(3, deviceLines.size()) * dp(30) + dp(20)) * selection(deviceIndex - position)
                : 0f;
        float footerHeight = dp(82);
        footerTop = h - insetBottom - footerHeight;
        canvas.save();
        canvas.clipRect(0, contentTop(), w, footerTop);

        for (int i = 0; i < items.size(); i++) {
            float d = i - position;
            float s = selection(d);
            float y = focusY + d * pitch + clamp(d, -1f, 1f) * (selIcon - baseIcon) * 0.5f;
            if (deviceIndex >= 0 && i > deviceIndex) {
                y += devicePanel;
            }
            float size = lerp(baseIcon, selIcon, s);
            if (y + size < contentTop() - dp(60)
                    || y - size > h - insetBottom - footerHeight + dp(60)) {
                continue;
            }
            Item item = items.get(i);
            float distanceAlpha = Math.abs(d) < 0.5f ? 1f : Math.abs(d) < 1.5f ? 0.94f : 0.86f;
            float rowAlpha = lerp(distanceAlpha, 1f, s);

            if (s > 0.02f) {
                drawSelectionBand(canvas, w, y, size, s);
            }
            float iconX = left + s * dp(6) + size * 0.5f;
            float pulse = 1f + 0.012f * s * (0.5f + 0.5f * (float) Math.sin(time * 2.85f));
            drawTile(canvas, item, iconX, y, size * pulse, s, tilt * 0.4f * s,
                    (float) Math.sin(time * 1.5f) * 1.2f * s);
            rect.set(0, y - pitch * 0.5f, w, y + pitch * 0.5f);
            hitRects.add(new RectF(rect));
            hitIndices.add(i);

            float textLeft = iconX + size * 0.5f + dp(16);
            boolean rowSettings = !item.utility && !sectionMode && i == focused && s > 0.5f;
            float textWidth = w - textLeft - (rowSettings ? dp(62) : dp(16));
            if (rowSettings) {
                settingsRect.set(w - dp(50), y - dp(18), w - dp(14), y + dp(18));
                drawRoundButton(canvas, settingsRect, "☰", rowAlpha);
                settingsEnabled = true;
            }
            String title = item.device
                    ? getContext().getString(R.string.home_device_title) : item.title;
            text.setTypeface(s > 0.5f ? medium : regular);
            float titleSize = lerp(sp(16), sp(20), s);
            text.setTextSize(titleSize);
            List<String> lines;
            if (s > 0.5f) {
                while (text.measureText(title) > textWidth && titleSize > sp(16)) {
                    titleSize -= sp(1);
                    text.setTextSize(titleSize);
                }
                lines = wrap(title, text, textWidth, 2);
            } else {
                lines = Collections.singletonList(ellipsize(title, text, textWidth));
            }
            text.setColor(0xffffffff);
            text.setAlpha((int) (255 * rowAlpha));
            float lineHeight = titleSize * 1.18f;
            float blockHeight = lines.size() * lineHeight + (s > 0.05f ? dp(22) * s : 0f);
            float titleY = y - blockHeight * 0.5f + titleSize * 0.92f;
            for (String line : lines) {
                drawReadableTitle(canvas, line, textLeft, titleY);
                titleY += lineHeight;
            }
            if (s > 0.05f) {
                drawRowSubtitle(canvas, item, textLeft, titleY + dp(4), textWidth, s);
            }
            if (item.device && s > 0.02f) {
                drawInfoLines(canvas, deviceLines.subList(0, Math.min(3, deviceLines.size())),
                        left + dp(4), y + size * 0.5f + dp(4),
                        w - left - dp(26), s);
            }
        }
        canvas.restore();
    }

    private void drawSelectionBand(Canvas canvas, int w, float y, float size, float s) {
        float top = y - size * 0.5f - dp(10);
        float bottom = y + size * 0.5f + dp(10);
        fill.setShader(new LinearGradient(0, 0, w, 0,
                hasGameBackground() ? new int[]{0x30000000, 0x10000000, 0x00000000}
                        : new int[]{withAlpha(theme.primary, 0.3f), withAlpha(theme.primary, 0.1f),
                                withAlpha(theme.primary, 0f)}, new float[]{0f, 0.55f, 1f},
                Shader.TileMode.CLAMP));
        fill.setAlpha((int) (255 * s));
        canvas.drawRect(0, top, w, bottom, fill);
        fill.setShader(new LinearGradient(0, 0, w, 0,
                new int[]{hasGameBackground() ? 0x66ffffff : withAlpha(theme.primary, 0.6f),
                        0x00ffffff}, null,
                Shader.TileMode.CLAMP));
        canvas.drawRect(0, top, w, top + dp(1), fill);
        canvas.drawRect(0, bottom - dp(1), w, bottom, fill);
        fill.setShader(null);
        fill.setAlpha(255);
    }

    private void drawRowSubtitle(Canvas canvas, Item item, float left, float baseline,
                                 float width, float s) {
        float x = left;
        text.setTypeface(regular);
        text.setTextSize(sp(13));
        if (item.utility) {
            text.setColor(0xb3ffffff);
            text.setAlpha((int) (200 * s));
            canvas.drawText(ellipsize(getContext().getString(item.device ? R.string.home_device_subtitle : R.string.menu_section_open),
                    text, width), x, baseline, text);
            return;
        }
        if (item.platform != null && layout == HORIZONTAL) {
            x = drawBadge(canvas, item.platform, x, baseline + dp(4), Paint.Align.LEFT, s, 1f)
                    + dp(8);
        }
        text.setTypeface(regular);
        text.setTextSize(sp(13));
        text.setColor(0xb3ffffff);
        text.setAlpha((int) (200 * s));
        String detail = item.titleId;
        if (sectionMode && item.status == STATUS_SUPPORTED) {
            detail = item.platform + " · Playable";
        } else if (!item.launchable) {
            detail += " · " + getContext().getString(R.string.metadata_only);
        }
        canvas.drawText(ellipsize(detail, text, left + width - x), x, baseline, text);
    }


    private void drawTile(Canvas canvas, Item item, float cx, float cy, float size, float s,
                          float rotateY, float rotateX) {
        float half = size * 0.5f;
        float radius = dp(10) * (size / dp(120));
        canvas.save();
        applyTilt(canvas, cx, cy, rotateY, rotateX);
        tile.set(cx - half, cy - half, cx + half, cy + half);
        if (s > 0.02f) {
            for (int g = 3; g >= 1; g--) {
                float grow = dp(4) * g;
                fill.setColor(theme.glow);
                fill.setAlpha((int) (22 * s / g));
                canvas.drawRoundRect(tile.left - grow, tile.top - grow, tile.right + grow,
                        tile.bottom + grow, radius + grow, radius + grow, fill);
            }
            fill.setColor(Color.BLACK);
            fill.setAlpha((int) (110 * s));
            canvas.drawRoundRect(tile.left + dp(2), tile.top + dp(10), tile.right - dp(2),
                    tile.bottom + dp(14), radius, radius, fill);
            fill.setAlpha(255);
        }
        float alpha = 0.55f + 0.45f * s;
        if (item.utility) {
            drawUtilityCardFace(canvas, item, tile, radius, alpha);
        } else {
            Bitmap icon = iconFor(item);
            if (icon != null) {
                drawContainedBitmap(canvas, icon, tile, radius, alpha);
            } else {
                drawPlaceholder(canvas, item, tile, radius, alpha);
            }
        }
        if (s > 0.05f) {
            matrix.reset();
            matrix.setRotate((time * 70f) % 360f);
            matrix.postTranslate(cx, cy);
            glow.setLocalMatrix(matrix);
            stroke.setShader(glow);
            stroke.setStrokeWidth(dp(2));
            stroke.setAlpha((int) (255 * s));
            canvas.drawRoundRect(tile, radius, radius, stroke);
            stroke.setShader(null);
            stroke.setAlpha(255);
        } else {
            stroke.setColor(0x33ffffff);
            stroke.setStrokeWidth(dp(1));
            canvas.drawRoundRect(tile, radius, radius, stroke);
        }
        float right = tile.right;
        float top = tile.top;
        float bottom = tile.bottom;
        if (!item.utility && item.status != STATUS_UNCLASSIFIED) {
            fill.setColor(item.status == STATUS_SUPPORTED ? SUPPORTED : UNSUPPORTED);
            canvas.drawCircle(right - dp(9), top + dp(9), dp(4), fill);
        }
        if (!item.utility && item.platform != null && size > dp(40)) {
            drawBadge(canvas, item.platform, right - dp(4), bottom - dp(4),
                    Paint.Align.RIGHT, 0.75f + 0.25f * s, s > 0.5f ? 1f : 0.72f);
        }
        canvas.restore();
    }


    private float drawFooter(Canvas canvas, int w, int h, int hint, Item actions, float alpha) {
        boolean buttons = actions != null && !actions.utility;
        float height = buttons ? dp(112) : dp(76);
        float bottomEdge = h - insetBottom;
        float top = bottomEdge - height;
        footerTop = top;
        fill.setShader(new LinearGradient(0, top - dp(40), 0, h,
                hasGameBackground() ? new int[]{0x00000000, 0xd9000000, 0xf2000000}
                        : new int[]{0x00020610, 0xd9020610, 0xf2020610}, new float[]{0f, 0.35f, 1f},
                Shader.TileMode.CLAMP));
        canvas.drawRect(0, top - dp(40), w, h, fill);
        fill.setShader(null);
        if (buttons) {
            float buttonHeight = dp(34);
            float playWidth = dp(112);
            float total = playWidth + dp(10) + buttonHeight;
            float x = (w - total) * 0.5f;
            float y = top + dp(14);
            playRect.set(x, y, x + playWidth, y + buttonHeight);
            settingsRect.set(playRect.right + dp(10), y, playRect.right + dp(10) + buttonHeight,
                    y + buttonHeight);
            drawPlayButton(canvas, playRect, actions.launchable, alpha);
            drawRoundButton(canvas, settingsRect, "☰", alpha);
            playEnabled = alpha > 0.5f;
            settingsEnabled = alpha > 0.5f;
        }
        text.setTypeface(light);
        text.setTextSize(sp(12));
        text.setColor(0x99ffffff);
        text.setTextAlign(Paint.Align.CENTER);
        List<String> lines = wrap(getContext().getString(hint), text, w - dp(32), 2);
        float lineHeight = sp(16);
        float baseline = bottomEdge - dp(14) - (lines.size() - 1) * lineHeight;
        for (String line : lines) {
            canvas.drawText(line, w * 0.5f, baseline, text);
            baseline += lineHeight;
        }
        text.setTextAlign(Paint.Align.LEFT);
        return top;
    }




    private void drawDock(Canvas canvas, int w, int h) {
        float slot = isHorizontal() ? dp(52) : Math.min(dp(68), (w - dp(16)) / functionIcons.length);
        float centerY = h - insetBottom - dp(36);
        fill.setShader(new LinearGradient(0, centerY - dp(50), 0, h,
                new int[]{0x00000000, 0x80000000}, null, Shader.TileMode.CLAMP));
        canvas.drawRect(0, centerY - dp(50), w, h, fill);
        fill.setShader(null);
        int count = functionIcons.length;
        for (int i = 0; i < count; i++) {
            float cx = isHorizontal() ? w - dp(16) - slot * (count - i - 0.5f)
                    : w * 0.5f + slot * (i - (count - 1) * 0.5f);
            drawFunctionIcon(canvas, i, cx, centerY - dp(6), slot, dp(20), sp(9));
        }
        if (isHorizontal()) drawLibraryStatus(canvas, dp(24), h - insetBottom - dp(16), w * 0.35f,
                Paint.Align.LEFT);
        footerTop = centerY - dp(32);
    }


    private void drawTopFunctionArea(Canvas canvas, int w) {
        float bottom = contentTop();
        fill.setShader(new LinearGradient(0, 0, 0, bottom + dp(24),
                hasGameBackground() ? new int[]{0xf2000000, 0xb3000000, 0x00000000}
                        : new int[]{0xf2020610, 0xb3020610, 0x00020610}, new float[]{0f, 0.7f, 1f},
                Shader.TileMode.CLAMP));
        canvas.drawRect(0, 0, w, bottom + dp(24), fill);
        fill.setShader(null);
        float statusWidth = dp(110);
        float slot = Math.min(dp(64), (w - dp(14) - statusWidth) / functionIcons.length);
        float centerY = insetTop + dp(36);
        for (int i = 0; i < functionIcons.length; i++) {
            float cx = dp(14) + slot * (i + 0.5f);
            drawFunctionIcon(canvas, i, cx, centerY, slot, dp(24), sp(10));
        }
    }

    private void drawFunctionIcon(Canvas canvas, int i, float cx, float cy, float slot,
                                  float iconSize, float labelSize) {
        functionRects[i].set(cx - slot * 0.5f, cy - iconSize - dp(6), cx + slot * 0.5f,
                cy + iconSize + dp(10));
        boolean focused = functionFocus == i;
        boolean pressed = pressedFunction == i;
        float ring = iconSize * 0.95f;
        if (focused || pressed) {
            fill.setColor(focused ? theme.glow : 0xffffffff);
            fill.setAlpha(focused ? 70 : 40);
            canvas.drawCircle(cx, cy, ring, fill);
            if (focused) {
                stroke.setColor(theme.glow);
                stroke.setStrokeWidth(dp(1.5f));
                stroke.setAlpha(220);
                canvas.drawCircle(cx, cy, ring, stroke);
                stroke.setAlpha(255);
            }
        }
        Drawable icon = functionIcons[i];
        if (icon != null) {
            icon.setBounds((int) (cx - iconSize * 0.5f), (int) (cy - iconSize * 0.5f),
                    (int) (cx + iconSize * 0.5f), (int) (cy + iconSize * 0.5f));
            icon.setAlpha(focused || pressed ? 255 : 215);
            icon.draw(canvas);
        }
        text.setTypeface(regular);
        text.setTextSize(labelSize);
        text.setColor(0xffffffff);
        text.setAlpha(focused ? 255 : 150);
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(ellipsize(getContext().getString(functionLabels[i]), text, slot - dp(4)),
                cx, cy + iconSize * 0.5f + labelSize + dp(4), text);
        text.setTextAlign(Paint.Align.LEFT);
    }


    private void drawSystemStatus(Canvas canvas, int w, float baseline) {
        Calendar now = Calendar.getInstance();
        String clock = DateFormat.is24HourFormat(getContext())
                ? String.format(Locale.ROOT, "%02d:%02d",
                        now.get(Calendar.HOUR_OF_DAY), now.get(Calendar.MINUTE))
                : DateFormat.format("h:mm", now).toString();
        float right = w - dp(18);
        text.setTypeface(light);
        text.setTextSize(sp(14));
        text.setColor(0xffffffff);
        text.setAlpha(230);
        text.setTextAlign(Paint.Align.RIGHT);
        canvas.drawText(clock, right, baseline, text);
        float x = right - text.measureText(clock) - dp(10);
        text.setTextAlign(Paint.Align.LEFT);
        float iconY = baseline - sp(5);
        if (batteryLevel >= 0) {
            float bw = dp(16);
            float bh = dp(8);
            rect.set(x - bw, iconY - bh * 0.5f, x, iconY + bh * 0.5f);
            stroke.setColor(0xffffffff);
            stroke.setStrokeWidth(dp(1f));
            stroke.setAlpha(210);
            canvas.drawRoundRect(rect, dp(1.5f), dp(1.5f), stroke);
            fill.setColor(0xffffffff);
            fill.setAlpha(210);
            canvas.drawRect(rect.right, iconY - bh * 0.22f, rect.right + dp(1.5f),
                    iconY + bh * 0.22f, fill);
            fill.setColor(batteryLevel <= 15 && !charging ? UNSUPPORTED
                    : charging ? SUPPORTED : 0xffffffff);
            fill.setAlpha(220);
            float inner = (bw - dp(3)) * batteryLevel / 100f;
            canvas.drawRect(rect.left + dp(1.5f), rect.top + dp(1.5f),
                    rect.left + dp(1.5f) + inner, rect.bottom - dp(1.5f), fill);
            stroke.setAlpha(255);
            fill.setAlpha(255);
            x = rect.left - dp(9);
        }
        if (wifi) {
            stroke.setColor(0xffffffff);
            stroke.setStrokeWidth(dp(1.2f));
            stroke.setAlpha(210);
            float cx = x - dp(7);
            float cy = iconY + dp(4.5f);
            for (int arc = 1; arc <= 3; arc++) {
                float r = dp(3.3f) * arc;
                rect.set(cx - r, cy - r, cx + r, cy + r);
                canvas.drawArc(rect, 225f, 90f, false, stroke);
            }
            stroke.setAlpha(255);
            fill.setColor(0xffffffff);
            canvas.drawCircle(cx, cy - dp(0.8f), dp(1.2f), fill);
        }
        if (!isHorizontal()) {
            drawLibraryStatus(canvas, right, baseline + dp(16), w * 0.4f, Paint.Align.RIGHT);
        }
    }


    private void drawLibraryStatus(Canvas canvas, float anchorX, float baseline, float maxWidth,
                                   Paint.Align align) {
        if (!busy) {
            return;
        }
        text.setTypeface(regular);
        text.setTextSize(sp(10));
        text.setColor(0xffffffff);
        text.setAlpha(150);
        String shown = ellipsize(status, text, maxWidth);
        float radius = dp(6);
        float spinner = radius * 2f + dp(6);
        float textLeft = align == Paint.Align.RIGHT
                ? anchorX - text.measureText(shown) : anchorX + spinner;
        canvas.drawText(shown, textLeft, baseline, text);
        float cx = textLeft - spinner + radius;
        float cy = baseline - dp(4);
        stroke.setColor(theme.glow);
        stroke.setStrokeWidth(dp(2));
        rect.set(cx - radius, cy - radius, cx + radius, cy + radius);
        canvas.drawArc(rect, (time * 360f) % 360f, 270f, false, stroke);
    }

    private void refreshSystemStatus(boolean force) {
        long now = SystemClock.uptimeMillis();
        if (!force && now - systemStatusAt < SYSTEM_STATUS_REFRESH_MS) {
            return;
        }
        systemStatusAt = now;
        Context context = getContext();
        Intent battery = context.registerReceiver(null,
                new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery != null) {
            int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            batteryLevel = level >= 0 && scale > 0 ? Math.round(level * 100f / scale) : -1;
            int plugged = battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0);
            charging = plugged != 0;
        }
        try {
            ConnectivityManager connectivity = context.getSystemService(ConnectivityManager.class);
            Network network = connectivity == null ? null : connectivity.getActiveNetwork();
            NetworkCapabilities capabilities = network == null ? null
                    : connectivity.getNetworkCapabilities(network);
            wifi = capabilities != null
                    && capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI);
        } catch (SecurityException ignored) {
            wifi = false;
        }
    }



    private void applyTilt(Canvas canvas, float cx, float cy, float rotateY, float rotateX) {
        if (Math.abs(rotateY) <= 0.05f && Math.abs(rotateX) <= 0.05f) {
            return;
        }
        camera.save();
        camera.setLocation(0f, 0f, -12f * density);
        camera.rotateY(rotateY);
        camera.rotateX(rotateX);
        camera.getMatrix(matrix);
        camera.restore();
        matrix.preTranslate(-cx, -cy);
        matrix.postTranslate(cx, cy);
        canvas.concat(matrix);
    }

    private void drawPlayButton(Canvas canvas, RectF bounds, boolean enabled, float alpha) {
        float radius = bounds.height() * 0.5f;
        float breath = enabled ? 0.5f + 0.5f * (float) Math.sin(time * 2.4f) : 0f;
        if (enabled) {
            fill.setColor(theme.glow);
            fill.setAlpha((int) (55 * alpha * (0.6f + 0.4f * breath)));
            float grow = dp(4);
            canvas.drawRoundRect(bounds.left - grow, bounds.top - grow, bounds.right + grow,
                    bounds.bottom + grow, radius + grow, radius + grow, fill);
        }
        fill.setColor(enabled ? 0xfff2f5f8 : 0x33ffffff);
        fill.setAlpha((int) ((enabled ? 255 : 90) * alpha));
        canvas.drawRoundRect(bounds, radius, radius, fill);
        text.setTypeface(medium);
        text.setTextSize(Math.min(sp(15), bounds.height() * 0.4f));
        text.setColor(enabled ? 0xff0b1a33 : 0xff9aa6b2);
        text.setAlpha((int) (255 * alpha));
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("▶  " + getContext().getString(R.string.home_play),
                bounds.centerX(), bounds.centerY() + text.getTextSize() * 0.36f, text);
        text.setTextAlign(Paint.Align.LEFT);
    }

    private void drawRoundButton(Canvas canvas, RectF bounds, String glyph, float alpha) {
        fill.setShader(null);
        fill.setColor(0xff0a1018);
        fill.setAlpha((int) (220 * alpha));
        canvas.drawOval(bounds, fill);
        stroke.setShader(null);
        stroke.setColor(0xffffffff);
        stroke.setAlpha((int) (245 * alpha));
        stroke.setStrokeWidth(dp(1.5f));
        canvas.drawOval(bounds, stroke);
        stroke.setAlpha(255);
        text.setTypeface(regular);
        text.setTextSize(bounds.height() * 0.42f);
        text.setColor(0xffffffff);
        text.setAlpha((int) (255 * alpha));
        text.setTextAlign(Paint.Align.CENTER);
        drawReadableTitle(canvas, glyph, bounds.centerX(), bounds.centerY() + text.getTextSize() * 0.36f);
        text.setTextAlign(Paint.Align.LEFT);
        fill.setAlpha(255);
    }

    private void drawInfoLines(Canvas canvas, List<DeviceInfoSnapshot.Line> lines, float left,
                               float top, float width, float alpha) {
        float y = top;
        float labelWidth = width * 0.38f;
        for (DeviceInfoSnapshot.Line line : lines) {
            y += dp(28);
            fill.setColor(0xffffffff);
            fill.setAlpha((int) (26 * alpha));
            canvas.drawRect(left, y + dp(9), left + width, y + dp(10), fill);
            text.setTypeface(regular);
            text.setTextSize(sp(12));
            text.setColor(0xffb3bdc9);
            text.setAlpha((int) (255 * alpha));
            canvas.drawText(ellipsize(line.label, text, labelWidth - dp(8)), left, y, text);
            text.setTypeface(medium);
            text.setTextSize(sp(13));
            text.setColor(0xffeef2f6);
            text.setAlpha((int) (255 * alpha));
            canvas.drawText(ellipsize(line.value, text, width - labelWidth), left + labelWidth,
                    y, text);
        }
        fill.setAlpha(255);
    }

    private float drawBadge(Canvas canvas, String platform, float anchorX, float bottom,
                            Paint.Align align, float alpha, float scale) {
        text.setTypeface(Typeface.DEFAULT_BOLD);
        text.setTextSize(sp(10) * scale);
        float textWidth = text.measureText(platform);
        float width = textWidth + dp(12) * scale;
        float height = dp(17) * scale;
        float left = align == Paint.Align.RIGHT ? anchorX - width : anchorX;
        rect.set(left, bottom - height, left + width, bottom);
        boolean ps5 = "PS5".equals(platform);
        fill.setColor(ps5 ? PS5_BADGE : PS4_BADGE);
        fill.setAlpha((int) (235 * alpha));
        canvas.drawRoundRect(rect, dp(6), dp(6), fill);
        stroke.setColor(ps5 ? 0xff89c8ff : 0xff6f9fff);
        stroke.setStrokeWidth(dp(1));
        stroke.setAlpha((int) (200 * alpha));
        canvas.drawRoundRect(rect, dp(6), dp(6), stroke);
        stroke.setAlpha(255);
        text.setColor(0xffffffff);
        text.setAlpha((int) (255 * alpha));
        canvas.drawText(platform, rect.left + dp(6) * scale, rect.bottom - dp(4.5f) * scale, text);
        fill.setAlpha(255);
        return rect.right;
    }

    private void drawPlaceholder(Canvas canvas, Item item, RectF bounds, float radius,
                                 float alpha) {
        fill.setShader(new LinearGradient(bounds.left, bounds.top, bounds.right, bounds.bottom,
                blend(theme.gradientStart, 0xff243246, 0.5f), 0xff10161f, Shader.TileMode.CLAMP));
        fill.setAlpha((int) (255 * alpha));
        canvas.drawRoundRect(bounds, radius, radius, fill);
        fill.setShader(null);
        fill.setAlpha(255);
        String initial = item.title.isEmpty() ? "?" : item.title.substring(0, 1);
        text.setTypeface(light);
        text.setTextSize(Math.min(bounds.width(), bounds.height()) * 0.42f);
        text.setColor(0xffffffff);
        text.setAlpha((int) (200 * alpha));
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(initial, bounds.centerX(), bounds.centerY() + text.getTextSize() * 0.35f,
                text);
        text.setTextAlign(Paint.Align.LEFT);
    }


    private void drawContainedBitmap(Canvas canvas, Bitmap bitmap, RectF bounds, float radius,
                                     float alpha) {
        fill.setShader(null);
        fill.setColor(0xff101318);
        fill.setAlpha((int)(255 * alpha));
        canvas.drawRoundRect(bounds, radius, radius, fill);
        float scale = Math.min(bounds.width() / bitmap.getWidth(), bounds.height() / bitmap.getHeight());
        float width = bitmap.getWidth() * scale;
        float height = bitmap.getHeight() * scale;
        rect.set(bounds.centerX() - width * 0.5f, bounds.centerY() - height * 0.5f,
                bounds.centerX() + width * 0.5f, bounds.centerY() + height * 0.5f);
        shape.reset();
        shape.addRoundRect(bounds, radius, radius, Path.Direction.CW);
        canvas.save();
        canvas.clipPath(shape);
        canvas.drawBitmap(bitmap, null, rect, fill);
        canvas.restore();
        fill.setAlpha(255);
    }


    private void drawRoundedBitmap(Canvas canvas, Bitmap bitmap, RectF bounds, float radius,
                                   float alpha, float zoom, float offsetX) {
        BitmapShader shader = shaders.get(bitmap);
        if (shader == null) {
            shader = new BitmapShader(bitmap, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
            shaders.put(bitmap, shader);
        }
        float scale = Math.max(bounds.width() / bitmap.getWidth(),
                bounds.height() / bitmap.getHeight()) * zoom;
        shaderMatrix.setScale(scale, scale);
        shaderMatrix.postTranslate(
                bounds.centerX() - bitmap.getWidth() * scale * 0.5f + offsetX,
                bounds.centerY() - bitmap.getHeight() * scale * 0.5f);
        shader.setLocalMatrix(shaderMatrix);
        fill.setShader(shader);
        fill.setAlpha((int) (255 * alpha));
        canvas.drawRoundRect(bounds, radius, radius, fill);
        fill.setShader(null);
        fill.setAlpha(255);
    }

    private void drawActivation(Canvas canvas, int w, int h) {
        float t = activation;
        float eased = 1f - (1f - t) * (1f - t);
        fill.setColor(0xffffffff);
        fill.setAlpha((int) (140 * (1f - t) * eased * 2f));
        canvas.drawCircle(lastFocusX, lastFocusY, Math.max(w, h) * eased, fill);
        fill.setColor(0xff000000);
        fill.setAlpha((int) (200 * eased));
        canvas.drawRect(0, 0, w, h, fill);
        fill.setAlpha(255);
    }



    private Bitmap iconFor(Item item) {
        return item.icon == null ? null : cached("icon:", item.icon, 384);
    }


    private Bitmap coverFor(Item item) {
        if (item.art != null) {
            Bitmap art = cached("card:", item.art, 640);
            if (art != null) {
                return art;
            }
        }
        return iconFor(item);
    }

    private Bitmap cached(String prefix, File file, int maxPixels) {
        String key = prefix + file.getAbsolutePath();
        Bitmap bitmap = bitmaps.get(key);
        if (bitmap == null) {
            load(key, file, maxPixels, false);
        }
        return bitmap;
    }

    private void requestArt(Item item) {
        File source = item.device ? null
                : item.art != null && item.art.isFile() ? item.art : item.icon;
        String key = source == null ? null : "art:" + source.getAbsolutePath();
        if (key != null && key.equals(artKey)) {
            return;
        }
        artKey = key;
        artPrevious = artCurrent;
        artCurrent = key == null ? null : bitmaps.get(key);
        artFade = 0f;
        if (key != null && artCurrent == null) {
            load(key, source, 960, true);
        }
    }

    private void load(String key, File file, int maxPixels, boolean art) {
        if (released || !pending.add(key)) {
            return;
        }
        try {
            loader.execute(() -> {
                Bitmap decoded = decode(file, maxPixels);
                post(() -> {
                    pending.remove(key);
                    if (decoded == null || released) {
                        return;
                    }
                    bitmaps.put(key, decoded);
                    if (art && key.equals(artKey)) {
                        artCurrent = decoded;
                        artFade = Math.min(artFade, 0.05f);
                    }
                    invalidate();
                });
            });
        } catch (java.util.concurrent.RejectedExecutionException ignored) {
            pending.remove(key);
        }
    }

    private static Bitmap decode(File file, int maxPixels) {
        if (file == null || !file.isFile()) {
            return null;
        }
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(file.getAbsolutePath(), bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
                return null;
            }
            int sample = 1;
            while (bounds.outWidth / (sample * 2) >= maxPixels
                    || bounds.outHeight / (sample * 2) >= maxPixels) {
                sample *= 2;
            }
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            options.inPreferredConfig = Bitmap.Config.RGB_565;
            return BitmapFactory.decodeFile(file.getAbsolutePath(), options);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private void refreshDeviceInfo(boolean force) {
        long now = SystemClock.uptimeMillis();
        if (!force && now - deviceInfoAt < DEVICE_INFO_REFRESH_MS && !deviceLines.isEmpty()) {
            return;
        }
        deviceInfoAt = now;
        Context context = getContext();
        try {
            loader.execute(() -> {
                List<DeviceInfoSnapshot.Line> lines = DeviceInfoSnapshot.collect(context);
                post(() -> {
                    deviceLines = lines;
                    invalidate();
                });
            });
        } catch (java.util.concurrent.RejectedExecutionException ignored) {
        }
    }



    private static float selection(float distance) {
        float raw = clamp(1f - Math.abs(distance) / 0.9f, 0f, 1f);
        float eased = 1f - (1f - raw) * (1f - raw) * (1f - raw);
        float normalized = clamp((eased - 0.3f) / 0.7f, 0f, 1f);
        return normalized * normalized * (3f - 2f * normalized);
    }

    private static float clamp(float value, float minimum, float maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }

    private static float lerp(float from, float to, float t) {
        return from + (to - from) * t;
    }

    private static float wrap(float value, float max) {
        float m = value % max;
        return m < 0f ? m + max : m;
    }

    private static int withAlpha(int color, float alpha) {
        return (Math.round(Math.max(0f, Math.min(1f, alpha)) * 255f) << 24) | (color & 0x00ffffff);
    }

    private static int blend(int a, int b, float t) {
        int r = Math.round(Color.red(a) + (Color.red(b) - Color.red(a)) * t);
        int g = Math.round(Color.green(a) + (Color.green(b) - Color.green(a)) * t);
        int bl = Math.round(Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t);
        return Color.rgb(r, g, bl);
    }

    private static String ellipsize(String value, Paint paint, float width) {
        if (value == null || width <= 0f) {
            return "";
        }
        if (paint.measureText(value) <= width) {
            return value;
        }
        float ellipsis = paint.measureText("…");
        int count = paint.breakText(value, true, Math.max(0f, width - ellipsis), null);
        return value.substring(0, Math.max(0, count)) + "…";
    }


    private static List<String> wrap(String value, Paint paint, float width, int maxLines) {
        List<String> lines = new ArrayList<>();
        String rest = value == null ? "" : value.trim();
        while (!rest.isEmpty() && lines.size() < maxLines) {
            if (paint.measureText(rest) <= width || lines.size() == maxLines - 1) {
                lines.add(ellipsize(rest, paint, width));
                break;
            }
            int count = paint.breakText(rest, true, width, null);
            int cut = rest.lastIndexOf(' ', Math.max(0, count));
            if (cut <= 0) {
                cut = count;
            }
            lines.add(rest.substring(0, cut).trim());
            rest = rest.substring(cut).trim();
            if (rest.startsWith("·")) {
                rest = rest.substring(1).trim();
            }
        }
        return lines;
    }

    private float dp(float value) {
        return value * density;
    }

    private float sp(float value) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, value,
                getResources().getDisplayMetrics());
    }
}
