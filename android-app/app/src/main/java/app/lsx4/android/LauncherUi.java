package app.lsx4.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.LocaleList;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.DragEvent;
import android.view.HapticFeedbackConstants;
import android.view.Menu;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.apache.commons.compress.archivers.sevenz.SevenZArchiveEntry;
import org.apache.commons.compress.archivers.sevenz.SevenZFile;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class LauncherUi {
    private static final String TAG = "LSX4.Library";
    public static final int REQUEST_IMPORT_EXTRACTED_GAME = 4101;
    public static final int REQUEST_IMPORT_PKG = 4102;

    private static final int MENU_IMPORT_DIRECTORY = 5101;
    private static final int MENU_IMPORT_PKG = 5102;
    private static final int MENU_SORT_MANUAL = 5201;
    private static final int MENU_SORT_REVERSE = 5202;
    private static final int MENU_SORT_PLAYABLE = 5203;
    private static final int MENU_SORT_UNPLAYABLE = 5204;
    private static final int MENU_STYLE_LIST = 5301;
    private static final int MENU_STYLE_HORIZONTAL = 5302;
    private static final int MENU_STYLE_VERTICAL = 5303;
    private static final int MENU_SOUND_EFFECTS = 5304;
    private static final int MENU_MUSIC = 5305;
    private static final int MENU_THEME = 5306;
    private static final int MENU_THEME_FIRST = 5310;
    private static final int MENU_STYLE_LIBRARY_HORIZONTAL = 5320;
    private static final int MENU_STYLE_LIBRARY_VERTICAL = 5321;
    private static final int MENU_STYLE_TILES_HORIZONTAL = 5322;
    private static final int MENU_STYLE_TILES_VERTICAL = 5323;
    private static final int MENU_GROUP_STYLE = 1;
    private static final int MENU_GROUP_THEME = 2;
    private static final int STYLE_LIST = 0;
    private static final int STYLE_HORIZONTAL = 1;
    private static final int STYLE_VERTICAL = 2;
    private static final int STYLE_LIBRARY_HORIZONTAL = 3;
    private static final int STYLE_LIBRARY_VERTICAL = 4;
    private static final int STYLE_TILES_HORIZONTAL = 5;
    private static final int STYLE_TILES_VERTICAL = 6;
    private static final long HOME_LAUNCH_DELAY_MS = 380L;
    private static final String EXTRA_EMBEDDED_AARCH64_JIT_BACKEND =
            "embedded_aarch64_jit_backend";
    private static final String EXTRA_GAME_PLATFORM = "game_platform";
    private static final String PLATFORM_PS4 = "ps4";
    private static final String PLATFORM_PS5 = "ps5";
    private static final String LIBRARY_LAYOUT_PREFS = "lsx4_library_layout";
    private static final String KEY_GAME_ORDER = "game_order_v1";
    private static final String KEY_SORT_MODE = "game_sort_mode_v1";
    private static final String KEY_SORT_REVERSED = "game_sort_reversed_v1";
    private static final String KEY_HOLD_HINT_HIDDEN = "hold_hint_hidden_v1";
    private static final String KEY_HOME_STYLE = "launcher_home_style_v1";
    private static final String KEY_HOME_THEME = "launcher_home_theme_v1";
    private static final Pattern TITLE_ID = Pattern.compile("[A-Z0-9]{9}");
    private static final int MAX_SFO_BYTES = 1024 * 1024;
    private static final int COPY_BUFFER_BYTES = 1024 * 1024;
    private static final int MAX_TREE_DEPTH = 64;
    private static final int MAX_IMPORT_FILES = 200_000;
    private static final int MAX_IMPORT_NODES = 200_000;
    private static final int MAX_DIRECTORY_CHILDREN = 20_000;
    private static final int MAX_DOCUMENT_NAME_LENGTH = 255;
    private static final int MAX_DOCUMENT_ID_LENGTH = 4096;
    private static final int MAX_TREE_URI_LENGTH = 8192;
    private static final long MAX_DOCUMENT_METADATA_CHARS = 8L * 1024L * 1024L;
    private static final long FREE_SPACE_RESERVE_BYTES = 512L * 1024L * 1024L;
    private static final long PKG_EXTRACTION_RESERVE_BYTES = 128L * 1024L * 1024L;
    private static final Object IMPORT_LOCK = new Object();
    private static final AtomicBoolean IMPORT_ACTIVE = new AtomicBoolean();

    private final MainActivity activity;
    private final AtomicBoolean workRunning = new AtomicBoolean();
    private final GameCompatibilityRepository compatibilityRepository;
    private final SharedPreferences libraryLayoutPreferences;
    private final Handler holdHandler;
    private final int touchSlop;
    private LinearLayout gameList;
    private ScrollView gameScroll;
    private LinearLayout searchRow;
    private EditText searchInput;
    private int sortMode;
    private boolean sortReversed;
    private String searchQuery = "";
    private TextView status;
    private ProgressBar progress;
    private ImageView removeAction;
    private GameEntry selectedGame;
    private int homeStyle;
    private int homeTheme;
    private PlayStationHomeView homeView;
    private MenuSoundEngine menuSounds;
    private View gestureHintView;
    private boolean homeLaunchPending;
    private View appBarView;
    private FrameLayout screenView;
    private View functionAnchor;
    private List<GameEntry> latestGames = Collections.emptyList();
    private View rootView;
    private volatile boolean destroyed;
    private final Runnable applySearch = () -> {
        if (!destroyed) {
            renderVisibleGames();
            scrollGamesToTop();
        }
    };

    public LauncherUi(MainActivity activity) {
        if (activity == null) {
            throw new IllegalArgumentException("activity == null");
        }
        this.activity = activity;
        compatibilityRepository = new GameCompatibilityRepository(activity);
        libraryLayoutPreferences = activity.getSharedPreferences(
                LIBRARY_LAYOUT_PREFS, Activity.MODE_PRIVATE);
        sortMode = GameLibraryView.validMode(libraryLayoutPreferences.getInt(
                KEY_SORT_MODE, GameLibraryView.MANUAL));
        sortReversed = libraryLayoutPreferences.getBoolean(KEY_SORT_REVERSED, false);
        homeStyle = validHomeStyle(libraryLayoutPreferences.getInt(KEY_HOME_STYLE, STYLE_LIST));
        homeTheme = validHomeTheme(libraryLayoutPreferences.getInt(KEY_HOME_THEME, 0));
        holdHandler = new Handler(Looper.getMainLooper());
        touchSlop = ViewConfiguration.get(activity).getScaledTouchSlop();
    }

    public View buildView() {
        activity.setRequestedOrientation(requestedOrientation());

        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xff111417);
        root.setFocusableInTouchMode(true);
        UiChrome.applyInsets(root, dp(16), dp(12), dp(16), dp(14));

        LinearLayout appBar = new LinearLayout(activity);
        appBar.setOrientation(LinearLayout.HORIZONTAL);
        appBar.setGravity(Gravity.CENTER_VERTICAL);
        appBar.setPadding(dp(14), dp(9), dp(6), dp(9));
        GradientDrawable appBarBackground = new GradientDrawable();
        appBarBackground.setColor(0xff1b2026);
        appBarBackground.setCornerRadius(dp(16));
        appBarBackground.setStroke(dp(1), 0xff2a313a);
        appBar.setBackground(appBarBackground);
        appBar.setElevation(dp(4));

        LinearLayout titleColumn = new LinearLayout(activity);
        titleColumn.setOrientation(LinearLayout.VERTICAL);
        titleColumn.addView(label("LSX4", 23, 0xffeef2f6), matchWrap());
        status = label(activity.getString(R.string.library_scanning), 12, 0xff9aa6b2);
        titleColumn.addView(status, matchWrap());
        appBar.addView(titleColumn, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        progress = new ProgressBar(activity);
        progress.setIndeterminate(true);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressLp = new LinearLayout.LayoutParams(dp(24), dp(24));
        progressLp.setMargins(dp(8), 0, dp(6), 0);
        appBar.addView(progress, progressLp);

        removeAction = new ImageView(activity);
        removeAction.setImageResource(R.drawable.ic_delete);
        removeAction.setPadding(dp(12), dp(12), dp(12), dp(12));
        removeAction.setContentDescription(activity.getString(R.string.menu_remove_game));
        removeAction.setClickable(true);
        removeAction.setFocusable(true);
        removeAction.setVisibility(View.GONE);
        removeAction.setOnClickListener(view -> confirmRemoveSelectedGame());
        appBar.addView(removeAction, new LinearLayout.LayoutParams(dp(48), dp(48)));

        ImageView homeLayout = new ImageView(activity);
        homeLayout.setImageResource(R.drawable.ic_home_layout);
        homeLayout.setPadding(dp(12), dp(12), dp(12), dp(12));
        homeLayout.setContentDescription(activity.getString(R.string.home_layout_action));
        homeLayout.setTooltipText(activity.getString(R.string.home_layout_action));
        homeLayout.setClickable(true);
        homeLayout.setFocusable(true);
        homeLayout.setOnClickListener(this::showHomeStyleMenu);
        appBar.addView(homeLayout, new LinearLayout.LayoutParams(dp(48), dp(48)));

        ImageView sort = new ImageView(activity);
        sort.setImageResource(R.drawable.ic_sort);
        sort.setPadding(dp(12), dp(12), dp(12), dp(12));
        sort.setContentDescription(activity.getString(R.string.library_sort_action));
        sort.setTooltipText(activity.getString(R.string.library_sort_action));
        sort.setClickable(true);
        sort.setFocusable(true);
        sort.setOnClickListener(this::showSortMenu);
        sort.setOnLongClickListener(view -> {
            toggleSearch();
            return true;
        });
        appBar.addView(sort, new LinearLayout.LayoutParams(dp(48), dp(48)));

        TextView add = toolbarAction("+", activity.getString(R.string.menu_add));
        add.setOnClickListener(this::showAddMenu);
        appBar.addView(add, new LinearLayout.LayoutParams(dp(48), dp(48)));

        ImageView settings = new ImageView(activity);
        settings.setImageResource(R.drawable.ic_tune);
        settings.setPadding(dp(12), dp(12), dp(12), dp(12));
        settings.setContentDescription(activity.getString(R.string.menu_settings));
        settings.setClickable(true);
        settings.setFocusable(true);
        settings.setOnClickListener(view -> openMenuSettings());
        appBar.addView(settings, new LinearLayout.LayoutParams(dp(48), dp(48)));
        root.addView(appBar, matchWrap());
        appBarView = appBar;
        buildSearchRow(root);

        if (!libraryLayoutPreferences.getBoolean(KEY_HOLD_HINT_HIDDEN, false)) {
            LinearLayout gestureHint = new LinearLayout(activity);
            gestureHint.setOrientation(LinearLayout.HORIZONTAL);
            gestureHint.setGravity(Gravity.CENTER_VERTICAL);
            gestureHint.setPadding(dp(12), dp(7), dp(4), dp(7));
            gestureHint.setBackground(roundedBackground(0xd91a2027, 0xff34404c, 12));
            TextView hintText = label(activity.getString(R.string.library_hold_hint),
                    10, 0xffaeb8c3);
            gestureHint.addView(hintText, new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            TextView dismissHint = toolbarAction("\u00d7",
                    activity.getString(R.string.library_hold_hint_dismiss));
            dismissHint.setTextSize(20);
            dismissHint.setOnClickListener(view -> {
                libraryLayoutPreferences.edit()
                        .putBoolean(KEY_HOLD_HINT_HIDDEN, true).apply();
                root.removeView(gestureHint);
            });
            gestureHint.addView(dismissHint,
                    new LinearLayout.LayoutParams(dp(36), dp(36)));
            LinearLayout.LayoutParams hintLp = matchWrap();
            hintLp.setMargins(0, dp(8), 0, 0);
            root.addView(gestureHint, hintLp);
            gestureHintView = gestureHint;
        }

        gameList = new LinearLayout(activity);
        gameList.setOrientation(LinearLayout.VERTICAL);
        gameScroll = new ScrollView(activity);
        gameScroll.setFillViewport(true);
        gameScroll.addView(gameList);
        LinearLayout.LayoutParams scrollLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        scrollLp.setMargins(0, dp(10), 0, 0);
        root.addView(gameScroll, scrollLp);

        FrameLayout screen = new FrameLayout(activity);
        screen.setBackgroundColor(0xff111417);
        homeView = new PlayStationHomeView(activity);
        homeView.setTheme(homeTheme);
        homeView.setListener(new PlayStationHomeView.Listener() {
            @Override
            public void onActivate(PlayStationHomeView.Item item) {
                launchFromHome(item);
            }

            @Override
            public void onSettings(PlayStationHomeView.Item item) {
                if (item != null && item.tag instanceof GameEntry) {
                    openGameSettings((GameEntry) item.tag);
                }
            }

            @Override
            public void onFunction(int function, android.graphics.RectF bounds) {
                runHomeFunction(function, bounds);
            }
        });
        screen.addView(homeView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        screen.addView(root, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        functionAnchor = new View(activity);
        functionAnchor.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        screen.addView(functionAnchor, new FrameLayout.LayoutParams(1, 1));

        rootView = root;
        screenView = screen;
        applyHomeStyle(false);
        refresh();
        return screen;
    }


    private void runHomeFunction(int function, android.graphics.RectF bounds) {
        if (functionAnchor == null) {
            return;
        }
        FrameLayout.LayoutParams anchorLp = new FrameLayout.LayoutParams(
                Math.max(1, Math.round(bounds.width())), Math.max(1, Math.round(bounds.height())));
        anchorLp.leftMargin = Math.round(bounds.left);
        anchorLp.topMargin = Math.round(bounds.top);
        functionAnchor.setLayoutParams(anchorLp);
        functionAnchor.post(() -> {
            if (destroyed || functionAnchor == null) {
                return;
            }
            switch (function) {
            case PlayStationHomeView.FUNCTION_VIEW:
                showHomeStyleMenu(functionAnchor);
                break;
            case PlayStationHomeView.FUNCTION_SEARCH:
                toggleSearch();
                break;
            case PlayStationHomeView.FUNCTION_SORT:
                showSortMenu(functionAnchor);
                break;
            case PlayStationHomeView.FUNCTION_ADD:
                showAddMenu(functionAnchor);
                break;
            case PlayStationHomeView.FUNCTION_SETTINGS:
                openMenuSettings();
                break;
            default:
                break;
            }
        });
    }

    private static int validHomeStyle(int style) {
        return style >= STYLE_LIST && style <= STYLE_TILES_VERTICAL ? style : STYLE_LIST;
    }

    private static int validHomeTheme(int theme) {
        return theme >= 0 && theme < PlayStationHomeView.THEMES.length ? theme : 0;
    }


    int requestedOrientation() {
        return homeStyle == STYLE_HORIZONTAL || homeStyle == STYLE_LIBRARY_HORIZONTAL
                || homeStyle == STYLE_TILES_HORIZONTAL
                ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                : ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
    }


    void applyWindowMode() {
        homeTheme = validHomeTheme(libraryLayoutPreferences.getInt(KEY_HOME_THEME, 0));
        if (homeView != null) homeView.setTheme(homeTheme);
        boolean home = homeStyle != STYLE_LIST;
        activity.setRequestedOrientation(requestedOrientation());
        android.view.Window window = activity.getWindow();
        window.setNavigationBarColor(home ? android.graphics.Color.TRANSPARENT : 0xff111417);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            window.setNavigationBarContrastEnforced(!home);
        }
        if (homeView != null) {
            homeView.setImmersive(home);
        }
        if (!home && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            android.view.WindowInsetsController controller =
                    window.getDecorView().getWindowInsetsController();
            if (controller != null) {
                controller.show(android.view.WindowInsets.Type.systemBars());
            }
        }
    }

    private MenuSoundEngine ensureMenuSounds() {
        if (menuSounds == null) {
            menuSounds = new MenuSoundEngine(activity, libraryLayoutPreferences);
        }
        return menuSounds;
    }

    void setMenuActive(boolean active) {
        if (homeView != null) homeView.setMenuActive(active);
        if (!active && menuSounds != null) menuSounds.setAmbientActive(false);
    }

    private void showHomeStyleMenu(View anchor) {
        PopupMenu popup = new PopupMenu(activity, anchor, Gravity.END);
        Menu menu = popup.getMenu();
        menu.add(MENU_GROUP_STYLE, MENU_STYLE_LIST, 0, R.string.home_layout_list)
                .setChecked(homeStyle == STYLE_LIST);
        menu.add(MENU_GROUP_STYLE, MENU_STYLE_HORIZONTAL, 1, R.string.home_layout_horizontal)
                .setChecked(homeStyle == STYLE_HORIZONTAL);
        menu.add(MENU_GROUP_STYLE, MENU_STYLE_VERTICAL, 2, R.string.home_layout_vertical)
                .setChecked(homeStyle == STYLE_VERTICAL);
        menu.add(MENU_GROUP_STYLE, MENU_STYLE_LIBRARY_HORIZONTAL, 3, R.string.home_layout_library_horizontal)
                .setChecked(homeStyle == STYLE_LIBRARY_HORIZONTAL);
        menu.add(MENU_GROUP_STYLE, MENU_STYLE_LIBRARY_VERTICAL, 4, R.string.home_layout_library_vertical)
                .setChecked(homeStyle == STYLE_LIBRARY_VERTICAL);
        menu.add(MENU_GROUP_STYLE, MENU_STYLE_TILES_HORIZONTAL, 5, R.string.home_layout_tiles_horizontal)
                .setChecked(homeStyle == STYLE_TILES_HORIZONTAL);
        menu.add(MENU_GROUP_STYLE, MENU_STYLE_TILES_VERTICAL, 6, R.string.home_layout_tiles_vertical)
                .setChecked(homeStyle == STYLE_TILES_VERTICAL);
        menu.setGroupCheckable(MENU_GROUP_STYLE, true, true);
        MenuSoundEngine sounds = ensureMenuSounds();
        menu.add(Menu.NONE, MENU_SOUND_EFFECTS, 7, R.string.home_sound_effects)
                .setCheckable(true).setChecked(sounds.effectsEnabled());
        menu.add(Menu.NONE, MENU_MUSIC, 8, R.string.home_music)
                .setCheckable(true).setChecked(sounds.musicEnabled());
        popup.setOnMenuItemClickListener(item -> {
            int themeIndex = item.getItemId() - MENU_THEME_FIRST;
            if (themeIndex >= 0 && themeIndex < PlayStationHomeView.THEMES.length) {
                setHomeTheme(themeIndex);
                return true;
            }
            switch (item.getItemId()) {
            case MENU_STYLE_LIST:
                setHomeStyle(STYLE_LIST);
                return true;
            case MENU_STYLE_HORIZONTAL:
                setHomeStyle(STYLE_HORIZONTAL);
                return true;
            case MENU_STYLE_VERTICAL:
                setHomeStyle(STYLE_VERTICAL);
                return true;
            case MENU_STYLE_LIBRARY_HORIZONTAL:
                setHomeStyle(STYLE_LIBRARY_HORIZONTAL);
                return true;
            case MENU_STYLE_LIBRARY_VERTICAL:
                setHomeStyle(STYLE_LIBRARY_VERTICAL);
                return true;
            case MENU_STYLE_TILES_HORIZONTAL:
                setHomeStyle(STYLE_TILES_HORIZONTAL);
                return true;
            case MENU_STYLE_TILES_VERTICAL:
                setHomeStyle(STYLE_TILES_VERTICAL);
                return true;
            case MENU_SOUND_EFFECTS:
                sounds.setEffectsEnabled(!sounds.effectsEnabled());
                return true;
            case MENU_MUSIC:
                sounds.setMusicEnabled(!sounds.musicEnabled());
                return true;
            default:
                return false;
            }
        });
        popup.show();
    }

    private void setHomeTheme(int theme) {
        homeTheme = validHomeTheme(theme);
        libraryLayoutPreferences.edit().putInt(KEY_HOME_THEME, homeTheme).apply();
        if (homeView != null) {
            homeView.setTheme(homeTheme);
        }
    }

    private void setHomeStyle(int style) {
        int next = validHomeStyle(style);
        if (next == homeStyle) {
            return;
        }
        homeStyle = next;
        libraryLayoutPreferences.edit().putInt(KEY_HOME_STYLE, homeStyle).apply();
        applyHomeStyle(true);
    }


    private void applyHomeStyle(boolean animate) {
        if (homeView == null || gameScroll == null) {
            return;
        }
        boolean home = homeStyle != STYLE_LIST;
        gameScroll.setVisibility(home ? View.GONE : View.VISIBLE);
        if (gestureHintView != null) {
            gestureHintView.setVisibility(home ? View.GONE : View.VISIBLE);
        }
        if (appBarView != null) {
            appBarView.setVisibility(home ? View.GONE : View.VISIBLE);
        }
        if (rootView != null) {
            rootView.setBackgroundColor(home ? android.graphics.Color.TRANSPARENT : 0xff111417);
        }

        applyWindowMode();
        if (home) {
            selectedGame = null;
            updateRemoveAction();

            homeView.setLayoutMode(homeStyle);
            homeView.setSounds(ensureMenuSounds());
            homeView.setVisibility(View.VISIBLE);
            if (animate) {
                homeView.setAlpha(0f);
                homeView.animate().alpha(1f).setDuration(260L).start();
            }
            homeView.requestFocus();
        } else {
            homeView.setVisibility(View.GONE);
            if (menuSounds != null) {
                menuSounds.setAmbientActive(false);
            }
        }
        renderVisibleGames();
    }

    private List<PlayStationHomeView.Item> homeItems(List<GameEntry> games) {
        List<PlayStationHomeView.Item> result = new ArrayList<>(games.size() + 1);
        result.add(new PlayStationHomeView.Item("device",
                activity.getString(R.string.home_device_title), "", null, null, null,
                false, true, PlayStationHomeView.STATUS_UNCLASSIFIED, null));
        result.add(new PlayStationHomeView.Item("menu:compatibility",
                activity.getString(R.string.menu_compatibility_card), "", null, null, null,
                true, false, PlayStationHomeView.STATUS_UNCLASSIFIED, "compatibility"));

        for (String platform : new String[]{PLATFORM_PS4, PLATFORM_PS5}) {
            for (GameEntry game : games) {
                if (!platform.equals(game.platform)) {
                    continue;
                }
                GameCompatibilityStatus compatibility =
                        compatibilityRepository.statusOf(game.titleId);
                int status = compatibility == GameCompatibilityStatus.SUPPORTED
                        ? PlayStationHomeView.STATUS_SUPPORTED
                        : compatibility == GameCompatibilityStatus.UNSUPPORTED
                        ? PlayStationHomeView.STATUS_UNSUPPORTED
                        : PlayStationHomeView.STATUS_UNCLASSIFIED;
                result.add(new PlayStationHomeView.Item(game.titleId, game.title, game.titleId,
                        PLATFORM_PS5.equals(game.platform) ? "PS5" : "PS4", game.icon,
                        homeArt(game), game.launchable, false, status, game));
            }
        }
        return result;
    }

    private static File homeArt(GameEntry game) {
        File sceSys = new File(game.directory, "sce_sys");
        for (String name : new String[]{"pic1.png", "pic0.png"}) {
            File candidate = new File(sceSys, name);
            if (safeRegularFileWithin(candidate, game.directory)) {
                return candidate;
            }
        }
        return null;
    }

    private void openMenuSettings() {
        if (!destroyed) {
            activity.startActivity(new Intent(activity, MenuSectionActivity.class)
                    .putExtra(MenuSectionActivity.EXTRA_SECTION, "settings")
                    .putExtra(MenuSectionActivity.EXTRA_STYLE, homeStyle));
        }
    }

    private void launchFromHome(PlayStationHomeView.Item item) {
        if (item != null && item.utility && !item.device && !destroyed) {
            activity.startActivity(new Intent(activity, MenuSectionActivity.class)
                    .putExtra(MenuSectionActivity.EXTRA_SECTION, (String) item.tag)
                    .putExtra(MenuSectionActivity.EXTRA_STYLE, homeStyle));
            return;
        }
        if (item != null && item.device && !destroyed) {
            activity.startActivity(new Intent(activity, DeviceInfoActivity.class)
                    .putExtra(DeviceInfoActivity.EXTRA_THEME, homeTheme));
            return;
        }
        if (item == null || !(item.tag instanceof GameEntry) || homeLaunchPending) {
            return;
        }
        GameEntry game = (GameEntry) item.tag;
        if (!game.launchable) {
            toast(activity.getString(R.string.metadata_only));
            return;
        }
        if (workRunning.get() || IMPORT_ACTIVE.get()) {
            toast(activity.getString(R.string.import_wait_before_launch));
            return;
        }
        homeLaunchPending = true;
        if (homeView != null) {
            homeView.playActivation();
        }
        holdHandler.postDelayed(() -> {
            homeLaunchPending = false;
            if (!destroyed) {
                launch(game);
            }
        }, HOME_LAUNCH_DELAY_MS);
    }

    private TextView toolbarAction(String glyph, String description) {
        TextView action = label(glyph, 30, 0xffeef2f6);
        action.setGravity(Gravity.CENTER);
        action.setContentDescription(description);
        action.setClickable(true);
        action.setFocusable(true);
        return action;
    }

    private void showAddMenu(View anchor) {
        PopupMenu popup = new PopupMenu(activity, anchor, Gravity.END);
        Menu menu = popup.getMenu();
        menu.add(Menu.NONE, MENU_IMPORT_DIRECTORY, 0,
                activity.getString(R.string.menu_import_directory));
        menu.add(Menu.NONE, MENU_IMPORT_PKG, 1,
                activity.getString(R.string.menu_import_pkg));
        popup.setOnMenuItemClickListener(item -> {
            switch (item.getItemId()) {
            case MENU_IMPORT_DIRECTORY:
                pickExtractedGameTree();
                return true;
            case MENU_IMPORT_PKG:
                pickPkg();
                return true;
            default:
                return false;
            }
        });
        popup.show();
    }

    private void showSortMenu(View anchor) {
        PopupMenu popup = new PopupMenu(activity, anchor, Gravity.END);
        Menu menu = popup.getMenu();
        menu.add(Menu.NONE, MENU_SORT_MANUAL, 0, R.string.library_sort_manual)
                .setCheckable(true).setChecked(sortMode == GameLibraryView.MANUAL && !sortReversed);
        menu.add(Menu.NONE, MENU_SORT_REVERSE, 1, R.string.library_sort_reverse)
                .setCheckable(true).setChecked(sortReversed);
        menu.add(Menu.NONE, MENU_SORT_PLAYABLE, 2, R.string.library_sort_playable)
                .setCheckable(true).setChecked(sortMode == GameLibraryView.PLAYABLE_FIRST && !sortReversed);
        menu.add(Menu.NONE, MENU_SORT_UNPLAYABLE, 3, R.string.library_sort_unplayable)
                .setCheckable(true).setChecked(sortMode == GameLibraryView.UNPLAYABLE_FIRST && !sortReversed);
        popup.setOnMenuItemClickListener(item -> {
            switch (item.getItemId()) {
            case MENU_SORT_MANUAL:
                sortMode = GameLibraryView.MANUAL;
                sortReversed = false;
                break;
            case MENU_SORT_REVERSE:
                sortReversed = !sortReversed;
                break;
            case MENU_SORT_PLAYABLE:
                sortMode = GameLibraryView.PLAYABLE_FIRST;
                sortReversed = false;
                break;
            case MENU_SORT_UNPLAYABLE:
                sortMode = GameLibraryView.UNPLAYABLE_FIRST;
                sortReversed = false;
                break;
            default:
                return false;
            }
            libraryLayoutPreferences.edit()
                    .putInt(KEY_SORT_MODE, sortMode)
                    .putBoolean(KEY_SORT_REVERSED, sortReversed).apply();
            selectedGame = null;
            updateRemoveAction();
            renderVisibleGames();
            scrollGamesToTop();
            return true;
        });
        popup.show();
    }

    private void buildSearchRow(LinearLayout root) {
        searchRow = new LinearLayout(activity);
        searchRow.setOrientation(LinearLayout.HORIZONTAL);
        searchRow.setGravity(Gravity.CENTER_VERTICAL);
        searchRow.setPadding(dp(12), 0, 0, 0);
        searchRow.setBackground(roundedBackground(0xff1b2026, 0xff34404c, 12));
        searchRow.setVisibility(View.GONE);

        searchInput = new EditText(activity);
        searchInput.setSingleLine(true);
        searchInput.setTextSize(16);
        searchInput.setTextColor(0xffeef2f6);
        searchInput.setHintTextColor(0xff9aa6b2);
        searchInput.setHint(R.string.library_search_hint);
        searchInput.setContentDescription(activity.getString(R.string.library_search_hint));
        searchInput.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        searchInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);

        searchInput.setImeHintLocales(new LocaleList(Locale.ENGLISH));
        searchInput.setImeOptions(EditorInfo.IME_ACTION_DONE
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_FORCE_ASCII);
        searchInput.setOnEditorActionListener((view, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_DONE) {
                hideSearchKeyboard();
                return true;
            }
            return false;
        });
        searchInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence text, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence text, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable text) {
                searchQuery = text.toString();
                selectedGame = null;
                updateRemoveAction();
                holdHandler.removeCallbacks(applySearch);
                holdHandler.postDelayed(applySearch, 150L);
            }
        });
        searchRow.addView(searchInput, new LinearLayout.LayoutParams(0, dp(48), 1f));
        TextView close = toolbarAction("\u00d7", activity.getString(R.string.library_search_hide));
        close.setOnClickListener(view -> hideSearch());
        searchRow.addView(close, new LinearLayout.LayoutParams(dp(48), dp(48)));
        LinearLayout.LayoutParams searchLp = matchWrap();
        searchLp.setMargins(0, dp(8), 0, 0);
        root.addView(searchRow, searchLp);
    }

    private void toggleSearch() {
        if (searchRow == null) {
            return;
        }
        if (searchRow.getVisibility() == View.VISIBLE) {
            hideSearch();
            return;
        }
        searchRow.setVisibility(View.VISIBLE);
        searchInput.requestFocus();
        searchInput.post(() -> {
            if (!destroyed && searchRow != null && searchRow.getVisibility() == View.VISIBLE) {
                InputMethodManager ime = activity.getSystemService(InputMethodManager.class);
                if (ime != null) {
                    ime.showSoftInput(searchInput, InputMethodManager.SHOW_IMPLICIT);
                }
            }
        });
    }

    private void hideSearchKeyboard() {
        InputMethodManager ime = activity.getSystemService(InputMethodManager.class);
        if (ime != null && searchInput != null) {
            ime.hideSoftInputFromWindow(searchInput.getWindowToken(), 0);
        }
    }

    private void hideSearch() {
        hideSearchKeyboard();
        searchRow.setVisibility(View.GONE);
        searchInput.clearFocus();
        if (homeView != null && homeStyle != STYLE_LIST) {
            homeView.requestFocus();
        } else if (rootView != null) {
            rootView.requestFocus();
        }
        searchInput.setText("");
        searchQuery = "";
        holdHandler.removeCallbacks(applySearch);
        renderVisibleGames();
        scrollGamesToTop();
    }

    private void scrollGamesToTop() {
        if (gameScroll != null) {
            gameScroll.scrollTo(0, 0);
        }
    }

    public View createView() {
        return buildView();
    }

    public View getView() {
        return screenView;
    }

    public void destroy() {
        destroyed = true;
        holdHandler.removeCallbacksAndMessages(null);
        if (homeView != null) {
            homeView.release();
            homeView = null;
        }
        if (menuSounds != null) {
            menuSounds.release();
            menuSounds = null;
        }
        gestureHintView = null;
        appBarView = null;
        functionAnchor = null;
        screenView = null;
        rootView = null;
        gameList = null;
        gameScroll = null;
        searchRow = null;
        searchInput = null;
        status = null;
        progress = null;
        removeAction = null;
    }

    public boolean handleActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_IMPORT_EXTRACTED_GAME && requestCode != REQUEST_IMPORT_PKG) {
            return false;
        }
        if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null) {
            setStatus(activity.getString(R.string.import_cancelled));
            return true;
        }

        Uri uri = data.getData();
        takeReadPermission(uri, data.getFlags());
        if (requestCode == REQUEST_IMPORT_EXTRACTED_GAME) {
            runJob(activity.getString(R.string.import_checking_directory), () -> {
                try {
                    return importExtractedTree(uri);
                } finally {
                    releaseReadPermission(uri);
                }
            });
        } else {
            runJob(activity.getString(R.string.import_copying_pkg), () -> {
                try {
                    return importSelectedFile(uri);
                } finally {
                    releaseReadPermission(uri);
                }
            });
        }
        return true;
    }

    public void refresh() {
        if (!workRunning.compareAndSet(false, true)) {
            setStatus(activity.getString(R.string.operation_wait));
            return;
        }
        setBusy(true, activity.getString(R.string.library_scanning));
        Thread worker = new Thread(() -> {
            List<GameEntry> games;
            String message;
            try {
                synchronized (IMPORT_LOCK) {
                    games = scanInstalledGames();
                }
                message = games.isEmpty()
                        ? activity.getString(R.string.library_empty_status)
                        : activity.getString(R.string.library_found, games.size());
            } catch (Exception e) {
                games = Collections.emptyList();
                Log.e(TAG, activity.getString(R.string.scan_error)
                        + " [" + e.getClass().getSimpleName() + "]");
                message = activity.getString(R.string.scan_error);
            }
            workRunning.set(false);
            finishUiWork(games, message);
        }, "ps4-library-scan");
        worker.setDaemon(true);
        worker.start();
    }

    private void pickExtractedGameTree() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        try {
            activity.startActivityForResult(intent, REQUEST_IMPORT_EXTRACTED_GAME);
        } catch (ActivityNotFoundException e) {
            toast(activity.getString(R.string.folder_picker_unavailable));
        }
    }

    private void pickPkg() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            activity.startActivityForResult(intent, REQUEST_IMPORT_PKG);
        } catch (ActivityNotFoundException e) {
            toast(activity.getString(R.string.file_picker_unavailable));
        }
    }

    private void takeReadPermission(Uri uri, int returnedFlags) {
        int flags = returnedFlags & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        flags &= Intent.FLAG_GRANT_READ_URI_PERMISSION;
        if (flags == 0) {
            return;
        }
        try {
            activity.getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (SecurityException | UnsupportedOperationException ignored) {
        }
    }

    private void releaseReadPermission(Uri uri) {
        try {
            activity.getContentResolver().releasePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException | UnsupportedOperationException ignored) {
        }
    }

    private void runJob(String initialStatus, ImportJob job) {
        if (!workRunning.compareAndSet(false, true)) {
            setStatus(activity.getString(R.string.operation_wait));
            return;
        }
        if (!IMPORT_ACTIVE.compareAndSet(false, true)) {
            workRunning.set(false);
            setStatus(activity.getString(R.string.import_already_running));
            return;
        }
        setBusy(true, initialStatus);
        Thread worker = new Thread(() -> {
            String message;
            List<GameEntry> games;
            try {
                synchronized (IMPORT_LOCK) {
                    try {
                        checkCancelled();
                        message = job.run();
                    } catch (Exception e) {
                        Log.e(TAG, activity.getString(R.string.import_error)
                                + " [" + e.getClass().getSimpleName() + "]");
                        message = activity.getString(R.string.import_error);
                    }
                    try {
                        games = scanInstalledGames();
                    } catch (Exception ignored) {
                        games = Collections.emptyList();
                    }
                }
            } finally {
                IMPORT_ACTIVE.set(false);
                workRunning.set(false);
            }
            finishUiWork(games, message);
        }, "ps4-game-import");
        worker.setDaemon(true);
        worker.start();
    }

    private String importExtractedTree(Uri treeUri) throws Exception {
        ContentResolver resolver = activity.getContentResolver();
        if (treeUri.toString().length() > MAX_TREE_URI_LENGTH) {
            throw new IOException("слишком длинный URI выбранной папки");
        }
        String rootId = DocumentsContract.getTreeDocumentId(treeUri);
        if (rootId == null || rootId.length() > MAX_DOCUMENT_ID_LENGTH) {
            throw new IOException("некорректный document ID корневой папки");
        }
        Uri rootDocument = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootId);

        SafNode eboot = findDirectChild(treeUri, rootDocument, "eboot.bin", false);
        SafNode sceSys = findDirectChild(treeUri, rootDocument, "sce_sys", true);
        if (eboot == null || sceSys == null) {
            throw new IOException("выбранная папка должна содержать eboot.bin и sce_sys");
        }
        SafNode paramSfo = findDirectChild(treeUri, sceSys.uri, "param.sfo", false);
        SafNode paramJson = findDirectChild(treeUri, sceSys.uri, "param.json", false);
        if (paramSfo == null && paramJson == null) {
            throw new IOException("в sce_sys отсутствует param.sfo или param.json");
        }
        if (eboot.size >= 0 && eboot.size < 4) {
            throw new IOException("eboot.bin пуст или обрезан");
        }

        String platform;
        String titleId;
        String title;
        Ps5Metadata metadata = paramJson != null ? readPs5Metadata(
                readUriLimited(resolver, paramJson.uri, MAX_SFO_BYTES)) : null;
        if (metadata != null) {
            platform = PLATFORM_PS5;
            titleId = metadata.titleId;
            title = metadata.title;
        } else {
            if (paramSfo == null) throw new IOException("нет метаданных PS4 или PS5");
            byte[] sourceSfo = readUriLimited(resolver, paramSfo.uri, MAX_SFO_BYTES);
            platform = PLATFORM_PS4;
            titleId = normalizeTitleId(PkgInstaller.sfoString(sourceSfo, "TITLE_ID"));
            title = cleanSfoString(PkgInstaller.sfoString(sourceSfo, "TITLE"));
        }
        if (!isValidTitleId(titleId)) {
            throw new IOException("некорректный title ID");
        }

        File home = homeDir();
        File appRoot = appInstallRoot();
        ensureDirectory(appRoot);
        File destination = checkedChild(appRoot, titleId);
        if (destination.exists() || Files.isSymbolicLink(destination.toPath())) {
            throw new IOException("игра " + titleId
                    + " уже установлена; автоматическое слияние отключено");
        }
        File stagingRoot = checkedChild(home, "import-staging");
        ensureDirectory(stagingRoot);
        File staging = checkedChild(stagingRoot,
                titleId + "-" + Long.toUnsignedString(System.nanoTime()));
        ensureDirectory(staging);

        CopyProgress copied = new CopyProgress(availableImportBudget(stagingRoot));
        boolean installed = false;
        try {
            copySafDirectory(treeUri, rootDocument, staging, staging, 0, copied,
                    new HashSet<>());

            File stagedEboot = checkedChild(staging, "eboot.bin");
            File stagedSceSys = checkedChild(staging, "sce_sys");
            File stagedParam = checkedChild(
                    stagedSceSys,
                    PLATFORM_PS5.equals(platform) ? "param.json" : "param.sfo");
            if (!isPlausibleEboot(stagedEboot, staging)
                    || !safeRegularFileWithin(stagedParam, staging)) {
                throw new IOException("копия игры не прошла проверку структуры");
            }
            Ps5Metadata copiedPs5 = PLATFORM_PS5.equals(platform)
                    ? readPs5Metadata(readFileLimited(stagedParam, MAX_SFO_BYTES)) : null;
            String copiedId = PLATFORM_PS5.equals(platform)
                    ? (copiedPs5 == null ? null : copiedPs5.titleId)
                    : readSfoValue(stagedParam, "TITLE_ID");
            if (!titleId.equals(copiedId)) {
                throw new IOException("title ID изменился во время копирования");
            }

            writeInstallManifest(staging, titleId, title, treeUri.toString(), true,
                    "saf-extracted-tree", platform);
            installStagingTree(staging, destination, stagingRoot);
            installed = true;
        } finally {
            if (!installed && staging.exists()) {
                deleteTreeWithin(staging, stagingRoot);
            }
        }

        return activity.getString(R.string.import_directory_success,
                displayTitle(title, titleId), titleId, copied.files, formatBytes(copied.bytes));
    }

    private String importSelectedFile(Uri uri) throws Exception {
        String displayName = queryDisplayName(uri);
        String lowerName = displayName == null ? "" : displayName.toLowerCase(Locale.ROOT);
        if (lowerName.endsWith(".rar") || lowerName.endsWith(".7z")
                || lowerName.endsWith(".zip")) {
            return importArchive(uri, displayName, lowerName);
        }
        return importPkg(uri);
    }

    private String importArchive(Uri uri, String displayName, String lowerName) throws Exception {
        File home = homeDir();
        File inbox = checkedChild(home, "archive-inbox");
        File stagingRoot = checkedChild(home, "import-staging");
        ensureDirectory(inbox);
        ensureDirectory(stagingRoot);

        String fallback = lowerName.endsWith(".rar") ? "import.rar"
                : lowerName.endsWith(".7z") ? "import.7z" : "import.zip";
        String safeName = safeFileName(displayName, fallback);
        File archiveFile = uniqueChild(inbox, safeName);
        File extractionRoot = checkedChild(stagingRoot,
                "archive-" + Long.toUnsignedString(System.nanoTime()));
        ensureDirectory(extractionRoot);

        long copied = 0;
        boolean installed = false;
        try {
            long declaredSize = querySize(uri);
            long copyBudget = availableImportBudget(inbox);
            if (declaredSize > copyBudget) {
                throw new IOException("недостаточно свободного места для архива (нужно "
                        + formatBytes(declaredSize) + ")");
            }
            copied = copyUriToFile(uri, archiveFile, copyBudget);
            postStatus("Распаковка " + safeName + "…");

            CopyProgress extracted = new CopyProgress(availableImportBudget(stagingRoot));
            if (lowerName.endsWith(".rar")) {
                extractRar(archiveFile, extractionRoot, extracted);
            } else if (lowerName.endsWith(".7z")) {
                extractSevenZip(archiveFile, extractionRoot, extracted);
            } else {
                extractZip(archiveFile, extractionRoot, extracted);
            }

            File gameRoot = findExtractedGameRoot(extractionRoot, 0);
            if (gameRoot == null) {
                throw new IOException("архив не содержит eboot.bin и sce_sys/param.json");
            }
            File eboot = checkedChild(gameRoot, "eboot.bin");
            File sceSys = checkedChild(gameRoot, "sce_sys");
            File paramJson = checkedChild(sceSys, "param.json");
            File paramSfo = checkedChild(sceSys, "param.sfo");
            Ps5Metadata metadata = safeRegularFileWithin(paramJson, gameRoot)
                    ? readPs5Metadata(readFileLimited(paramJson, MAX_SFO_BYTES)) : null;
            boolean ps5 = metadata != null;
            if (!isPlausibleEboot(eboot, gameRoot)
                    || (!ps5 && !safeRegularFileWithin(paramSfo, gameRoot))) {
                throw new IOException("распакованная игра не прошла проверку структуры");
            }

            String platform;
            String titleId;
            String title;
            if (ps5) {
                platform = PLATFORM_PS5;
                titleId = metadata.titleId;
                title = metadata.title;
            } else {
                platform = PLATFORM_PS4;
                titleId = readSfoValue(paramSfo, "TITLE_ID");
                title = readSfoValue(paramSfo, "TITLE");
            }
            if (!isValidTitleId(titleId)) {
                throw new IOException("некорректный title ID в архиве");
            }

            File appRoot = appInstallRoot();
            ensureDirectory(appRoot);
            File destination = checkedChild(appRoot, titleId);
            if (destination.exists() || Files.isSymbolicLink(destination.toPath())) {
                throw new IOException("игра " + titleId + " уже установлена");
            }
            writeInstallManifest(gameRoot, titleId, title, uri.toString(), true,
                    "archive-import", platform);
            installStagingTree(gameRoot, destination, stagingRoot);
            installed = true;

            return "Импортирована " + displayTitle(title, titleId) + " (" + titleId + "), "
                    + extracted.files + " файлов, " + formatBytes(extracted.bytes)
                    + " распаковано из " + formatBytes(copied);
        } finally {
            if (archiveFile.exists() && !archiveFile.delete()) {
                Log.w(TAG, "Не удалось удалить временный архив " + archiveFile.getName());
            }
            if (extractionRoot.exists()) {
                try {
                    deleteTreeWithin(extractionRoot, stagingRoot);
                } catch (IOException cleanupError) {
                    if (!installed) {
                        throw cleanupError;
                    }
                    Log.w(TAG, "Не удалось очистить staging архива", cleanupError);
                }
            }
        }
    }

    private void extractRar(File archiveFile, File destination, CopyProgress progress)
            throws Exception {
        checkCancelled();
        int result = RuntimeBridge.extractRarArchive(
                archiveFile.getAbsolutePath(), destination.getAbsolutePath(),
                progress.maxBytes, MAX_IMPORT_FILES);
        if (result != 0) {
            throw new IOException("UnRAR завершился с кодом " + result);
        }
        accountExtractedTree(destination, destination, progress, 0);
    }

    private void accountExtractedTree(File path, File boundary, CopyProgress progress, int depth)
            throws Exception {
        checkCancelled();
        if (depth > MAX_TREE_DEPTH) {
            throw new IOException("слишком глубокий путь после распаковки");
        }
        ensureWithin(path, boundary);
        if (Files.isSymbolicLink(path.toPath())) {
            throw new IOException("символические ссылки в архиве запрещены");
        }
        if (path.isFile()) {
            progress.reserveNode(false);
            progress.reserveFile(path.length());
            progress.addExtracted(path.length());
            return;
        }
        if (!path.isDirectory()) {
            throw new IOException("некорректный объект после распаковки");
        }
        if (!path.equals(boundary)) {
            progress.reserveNode(true);
        }
        File[] children = path.listFiles();
        if (children == null) {
            throw new IOException("не удалось прочитать распакованный каталог");
        }
        for (File child : children) {
            accountExtractedTree(child, boundary, progress, depth + 1);
        }
    }

    private void extractSevenZip(File archiveFile, File destination, CopyProgress progress)
            throws Exception {
        try (SevenZFile archive = new SevenZFile(archiveFile)) {
            SevenZArchiveEntry entry;
            byte[] buffer = new byte[COPY_BUFFER_BYTES];
            while ((entry = archive.getNextEntry()) != null) {
                checkCancelled();
                File output = archiveOutput(destination, entry.getName(),
                        entry.isDirectory(), progress);
                if (entry.isDirectory()) {
                    ensureDirectory(output);
                    continue;
                }
                long declared = entry.getSize();
                progress.reserveFile(declared);
                ensureDirectory(output.getParentFile());
                try (OutputStream stream = new BudgetOutputStream(
                        new FileOutputStream(output), progress, declared)) {
                    int count;
                    while ((count = archive.read(buffer)) != -1) {
                        stream.write(buffer, 0, count);
                    }
                } catch (Exception error) {
                    output.delete();
                    throw error;
                }
            }
        }
    }

    private void extractZip(File archiveFile, File destination, CopyProgress progress)
            throws Exception {
        try (ZipInputStream archive = new ZipInputStream(new FileInputStream(archiveFile))) {
            byte[] buffer = new byte[COPY_BUFFER_BYTES];
            ZipEntry entry;
            while ((entry = archive.getNextEntry()) != null) {
                checkCancelled();
                File output = archiveOutput(destination, entry.getName(),
                        entry.isDirectory(), progress);
                if (entry.isDirectory()) {
                    ensureDirectory(output);
                    continue;
                }
                long declared = entry.getSize();
                progress.reserveFile(declared);
                ensureDirectory(output.getParentFile());
                try (OutputStream stream = new BudgetOutputStream(
                        new FileOutputStream(output), progress, declared)) {
                    int count;
                    while ((count = archive.read(buffer)) != -1) {
                        stream.write(buffer, 0, count);
                    }
                } catch (Exception error) {
                    output.delete();
                    throw error;
                }
                archive.closeEntry();
            }
        }
    }

    private File archiveOutput(File root, String rawName, boolean directory,
                               CopyProgress progress) throws Exception {
        if (rawName == null || rawName.isEmpty() || rawName.length() > MAX_DOCUMENT_ID_LENGTH) {
            throw new IOException("некорректное имя записи архива");
        }
        String normalized = rawName.replace('\\', '/');
        if (normalized.startsWith("/") || normalized.matches("^[A-Za-z]:.*")) {
            throw new IOException("абсолютный путь в архиве запрещён");
        }
        String[] segments = normalized.split("/");
        File current = root;
        int depth = 0;
        for (String segment : segments) {
            if (segment.isEmpty()) {
                continue;
            }
            if (++depth > MAX_TREE_DEPTH) {
                throw new IOException("слишком глубокий путь в архиве");
            }
            validateDocumentName(segment);
            if (segment.length() > MAX_DOCUMENT_NAME_LENGTH) {
                throw new IOException("слишком длинное имя в архиве");
            }
            current = checkedChild(current, segment);
        }
        if (current.equals(root)) {
            throw new IOException("пустой путь записи архива");
        }
        ensureWithin(current, root);
        progress.reserveNode(directory);
        return current;
    }

    private File findExtractedGameRoot(File directory, int depth) throws IOException {
        ensureWithin(directory, directory);
        if (depth > MAX_TREE_DEPTH || Files.isSymbolicLink(directory.toPath())) {
            return null;
        }
        File eboot = new File(directory, "eboot.bin");
        File sceSys = new File(directory, "sce_sys");
        if (isPlausibleEboot(eboot, directory) && sceSys.isDirectory()
                && !Files.isSymbolicLink(sceSys.toPath())) {
            File paramJson = new File(sceSys, "param.json");
            File paramSfo = new File(sceSys, "param.sfo");
            if (safeRegularFileWithin(paramJson, directory)
                    || safeRegularFileWithin(paramSfo, directory)) {
                return directory;
            }
        }
        File[] children = directory.listFiles();
        if (children == null) {
            return null;
        }
        File found = null;
        for (File child : children) {
            if (!child.isDirectory() || Files.isSymbolicLink(child.toPath())) {
                continue;
            }
            File candidate = findExtractedGameRoot(child, depth + 1);
            if (candidate != null) {
                if (found != null) {
                    throw new IOException("архив содержит несколько корней игр");
                }
                found = candidate;
            }
        }
        return found;
    }

    private String importPkg(Uri uri) throws Exception {
        File home = homeDir();
        File inbox = checkedChild(home, "pkg-inbox");
        ensureDirectory(inbox);
        String displayName = queryDisplayName(uri);
        cleanupLegacyPkgInbox(inbox);
        StatFs storage = new StatFs(inbox.getAbsolutePath());
        long available = storage.getAvailableBytes();
        if (available <= PKG_EXTRACTION_RESERVE_BYTES) {
            throw new IOException("недостаточно места для распаковки PKG; свободно "
                    + formatBytes(available));
        }
        try {
            File metadataStagingRoot = checkedChild(home, "pkg-install-staging");
            ensureDirectory(metadataStagingRoot);
            File stagingHome = checkedChild(metadataStagingRoot,
                    "pkg-" + Long.toUnsignedString(System.nanoTime()));
            ensureDirectory(stagingHome);
            try {
                long declaredSize = querySize(uri);
                postStatus(declaredSize > 0
                        ? "Распаковка PKG/PFS " + formatBytes(declaredSize) + "…"
                        : "Распаковка PKG/PFS…");
                try (ParcelFileDescriptor descriptor = activity.getContentResolver()
                        .openFileDescriptor(uri, "r")) {
                    if (descriptor == null) {
                        throw new IOException("провайдер не открыл выбранный PKG");
                    }
                    String extractionError = RuntimeBridge.extractPs4PkgFd(
                            descriptor.getFd(), stagingHome.getAbsolutePath());
                    if (extractionError == null || !extractionError.isEmpty()) {
                        throw new IOException(extractionError == null
                                ? "нативный PKG/PFS extractor не ответил"
                                : extractionError);
                    }
                }
                File stagedDirectory = findExtractedGameRoot(stagingHome, 0);
                if (stagedDirectory == null) {
                    throw new IOException("PKG/PFS распакован без исполняемого eboot.bin");
                }
                File stagedSfo = new File(new File(stagedDirectory, "sce_sys"), "param.sfo");
                if (!safeRegularFileWithin(stagedSfo, stagedDirectory)) {
                    throw new IOException("PKG/PFS не содержит sce_sys/param.sfo");
                }
                String extractedTitleId = normalizeTitleId(
                        PkgInstaller.sfoString(readFileLimited(stagedSfo, MAX_SFO_BYTES),
                                "TITLE_ID"));
                if (!isValidTitleId(extractedTitleId)) {
                    throw new IOException("некорректный TITLE_ID в распакованном PKG");
                }
                String extractedTitle = cleanSfoString(
                        PkgInstaller.sfoString(readFileLimited(stagedSfo, MAX_SFO_BYTES),
                                "TITLE"));
                File appRoot = appInstallRoot();
                ensureDirectory(appRoot);
                File finalDirectory = checkedChild(appRoot, extractedTitleId);
                writeInstallManifest(stagedDirectory, extractedTitleId, extractedTitle,
                        uri.toString(), true, "pkg-pfs-import", PLATFORM_PS4);
                installPkgStagingTree(stagedDirectory, finalDirectory, metadataStagingRoot);
                return activity.getString(R.string.import_pkg_registered,
                        displayTitle(extractedTitle, extractedTitleId));
            } finally {
                if (stagingHome.exists()) {
                    deleteTreeWithin(stagingHome, metadataStagingRoot);
                }
            }
        } finally {
            cleanupLegacyPkgInbox(inbox);
        }
    }

    private static void cleanupLegacyPkgInbox(File inbox) {
        File[] files = inbox.listFiles();
        if (files == null) {
            return;
        }
        for (File file : files) {
            if (file.isFile() && !Files.isSymbolicLink(file.toPath()) && !file.delete()) {
                Log.w(TAG, "Cannot remove legacy temporary PKG " + file);
            }
        }
    }

    private List<GameEntry> scanInstalledGames() throws IOException {
        File home = homeDir();
        List<File> roots = new ArrayList<>();
        if (SharedGameStorage.hasAccess()) {
            roots.add(SharedGameStorage.gamesDirectory());
        }
        roots.add(appInstallRoot());
        roots.add(checkedChild(home, "games"));
        List<GameEntry> merged = new ArrayList<>();
        for (int rootPriority = 0; rootPriority < roots.size(); rootPriority++) {
            File scanRoot = roots.get(rootPriority);
            File[] children = scanRoot.listFiles();
            if (children == null) {
                continue;
            }
            for (File directory : children) {
                if (!directory.isDirectory() || Files.isSymbolicLink(directory.toPath())) {
                    continue;
                }
                try {
                    ensureWithin(directory, scanRoot);
                } catch (IOException escapedRoot) {
                    continue;
                }
                GameEntry candidate = readGameEntry(directory, rootPriority, scanRoot);
                if (candidate == null) {
                    continue;
                }
                int duplicate = findDuplicate(merged, candidate);
                if (duplicate < 0) {
                    merged.add(candidate);
                } else if (prefer(candidate, merged.get(duplicate))) {
                    merged.set(duplicate, candidate);
                }
            }
        }




        merged.sort((left, right) -> GameLibraryOrder.compareInsertion(
                left.addedAtMillis, left.titleId,
                right.addedAtMillis, right.titleId));
        return merged;
    }

    private GameEntry readGameEntry(File directory, int rootPriority, File scanRoot) {
        try {
            ensureWithin(directory, scanRoot);
            File expectedEboot = new File(directory, "eboot.bin");
            File discoveredEboot = findInstalledEboot(directory);
            File eboot = discoveredEboot == null ? expectedEboot : discoveredEboot;
            File sceSys = new File(directory, "sce_sys");
            File sfo = new File(sceSys, "param.sfo");
            File paramJson = new File(sceSys, "param.json");
            File manifest = new File(directory, "executor-installed.json");
            boolean hasEboot = discoveredEboot != null;
            boolean hasSfo = safeRegularFileWithin(sfo, directory);
            boolean hasParamJson = safeRegularFileWithin(paramJson, directory);
            boolean hasManifest = safeRegularFileWithin(manifest, directory);
            if (!hasEboot && !hasSfo && !hasParamJson && !hasManifest) {
                return null;
            }
            if (!hasEboot && hasManifest && isOrphanedMetadataInstall(manifest)) {
                Log.w(TAG, "Ignoring incomplete restored game " + directory.getName());
                return null;
            }

            Ps5Metadata ps5Metadata = hasParamJson
                    ? readPs5Metadata(readFileLimited(paramJson, MAX_SFO_BYTES))
                    : null;
            String platform = ps5Metadata != null ? PLATFORM_PS5 : PLATFORM_PS4;
            String titleId = ps5Metadata != null
                    ? ps5Metadata.titleId
                    : hasSfo ? readSfoValue(sfo, "TITLE_ID") : null;
            if (!isValidTitleId(titleId) && isValidTitleId(directory.getName())) {
                titleId = directory.getName();
            }
            if (!isValidTitleId(titleId)) {
                return null;
            }
            String title = ps5Metadata != null
                    ? ps5Metadata.title
                    : hasSfo ? readSfoValue(sfo, "TITLE") : null;
            if (title == null || title.isEmpty()) {
                title = titleId;
            }
            File icon = new File(sceSys, "icon0.png");
            long addedAtMillis = hasManifest ? manifest.lastModified() : directory.lastModified();
            if (addedAtMillis <= 0L) {
                addedAtMillis = Long.MAX_VALUE;
            }
            return new GameEntry(directory, eboot,
                    safeRegularFileWithin(icon, directory) ? icon : null,
                    titleId, title, platform, hasEboot, rootPriority, addedAtMillis);
        } catch (Exception ignored) {
            Log.w(TAG, "Skipping unreadable game directory " + directory, ignored);
            return null;
        }
    }

    private static int findDuplicate(List<GameEntry> entries, GameEntry candidate) {
        for (int i = 0; i < entries.size(); i++) {
            GameEntry existing = entries.get(i);
            if (candidate.titleId.equals(existing.titleId)) {
                return i;
            }
        }
        return -1;
    }

    private static boolean prefer(GameEntry candidate, GameEntry existing) {
        if (candidate.launchable != existing.launchable) {
            return candidate.launchable;
        }
        if (candidate.rootPriority != existing.rootPriority) {
            return candidate.rootPriority < existing.rootPriority;
        }
        if ((candidate.icon != null) != (existing.icon != null)) {
            return candidate.icon != null;
        }
        return candidate.directory.lastModified() > existing.directory.lastModified();
    }

    private List<String> savedGameOrder() {
        List<String> order = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        String encoded = libraryLayoutPreferences.getString(KEY_GAME_ORDER, "");
        if (encoded == null || encoded.isEmpty()) {
            return order;
        }
        try {
            JSONArray array = new JSONArray(encoded);
            int count = Math.min(array.length(), 10_000);
            for (int index = 0; index < count; ++index) {
                String titleId = array.optString(index, "");
                if (isValidTitleId(titleId) && seen.add(titleId)) {
                    order.add(titleId);
                }
            }
        } catch (Exception error) {
            Log.w(TAG, "Ignoring invalid saved library order", error);
        }
        return order;
    }

    private void applySavedGameOrder(List<GameEntry> games) {
        List<String> discovered = new ArrayList<>(games.size());
        java.util.Map<String, GameEntry> byTitleId = new java.util.HashMap<>();
        for (GameEntry game : games) {
            discovered.add(game.titleId);
            byTitleId.put(game.titleId, game);
        }
        List<String> merged = GameLibraryOrder.mergeRemembered(
                discovered, savedGameOrder());
        games.clear();
        for (String titleId : merged) {
            GameEntry game = byTitleId.get(titleId);
            if (game != null) {
                games.add(game);
            }
        }


        persistGameOrder(games);
    }

    private void persistGameOrder(List<GameEntry> games) {
        JSONArray array = new JSONArray();
        for (GameEntry game : games) {
            if (game != null && isValidTitleId(game.titleId)) {
                array.put(game.titleId);
            }
        }
        libraryLayoutPreferences.edit().putString(KEY_GAME_ORDER, array.toString()).apply();
    }

    private static boolean sameGame(GameEntry left, GameEntry right) {
        return left != null && right != null && left.titleId.equals(right.titleId);
    }

    private static GameEntry findGame(List<GameEntry> games, String titleId) {
        if (titleId == null) {
            return null;
        }
        for (GameEntry game : games) {
            if (titleId.equals(game.titleId)) {
                return game;
            }
        }
        return null;
    }

    private void renderGameList(List<GameEntry> games) {
        if (gameList == null) {
            return;
        }
        List<GameEntry> orderedGames = new ArrayList<>(games);
        applySavedGameOrder(orderedGames);
        latestGames = orderedGames;
        SharedPreferences libraryPreferences = SettingsActivity.prefs(activity);
        List<GameProfileSettings.GameTitle> installedTitles =
                new ArrayList<>(orderedGames.size());
        for (GameEntry game : orderedGames) {
            installedTitles.add(new GameProfileSettings.GameTitle(
                    game.titleId, game.title));
        }
        GameProfileSettings.replaceInstalledTitles(libraryPreferences, installedTitles);
        if (selectedGame != null) {
            selectedGame = findGame(orderedGames, selectedGame.titleId);
            if (selectedGame == null) {
                updateRemoveAction();
            }
        }
        renderVisibleGames();
    }

    private void renderVisibleGames() {
        if (gameList == null) {
            return;
        }
        List<GameEntry> visibleGames = GameLibraryView.visible(latestGames,
                sortMode, sortReversed, searchQuery, game -> game.title,
                game -> compatibilityRepository.statusOf(game.titleId));
        if (homeView != null) {
            homeView.setItems(homeItems(visibleGames));
            if (homeStyle != STYLE_LIST) {
                return;
            }
        }
        gameList.removeAllViews();
        for (PlayStationHomeView.Item item : homeItems(Collections.emptyList())) {
            if (item.device) continue;
            gameList.addView(utilityRow(item), matchWrapWithMargins(0, 0, 0, 8));
        }
        if (visibleGames.isEmpty()) {
            TextView empty = label(activity.getString(latestGames.isEmpty()
                            ? R.string.library_empty : R.string.library_search_empty),
                    14, 0xffc5cbd3);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(dp(22), dp(34), dp(22), dp(34));
            empty.setBackground(roundedBackground(0xff171b20, 0xff2a313a, 18));
            gameList.addView(empty, matchWrap());
            return;
        }
        for (GameEntry game : visibleGames) {
            gameList.addView(gameRow(game), matchWrapWithMargins(0, 0, 0, 10));
        }
    }

    private View utilityRow(PlayStationHomeView.Item item) {
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        row.setMinimumHeight(dp(64));
        row.setBackground(roundedBackground(0xff1b2026, 0xff2a313a, 18));
        ImageView icon = new ImageView(activity);
        icon.setImageResource(item.device ? R.drawable.ic_device : R.drawable.ic_compatibility);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        icon.setPadding(dp(8), dp(8), dp(8), dp(8));
        icon.setBackground(roundedBackground(0xff263543, 0xff45627a, 12));
        icon.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        LinearLayout.LayoutParams iconLp = new LinearLayout.LayoutParams(dp(44), dp(44));
        iconLp.setMargins(0, 0, dp(14), 0);
        row.addView(icon, iconLp);
        TextView title = label(item.title, 14, 0xffeef2f6);
        title.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        row.addView(title, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.setContentDescription(item.title);
        row.setClickable(true);
        row.setFocusable(true);
        row.setOnClickListener(v -> launchFromHome(item));
        return row;
    }

    private View gameRow(GameEntry game) {
        AccessibleGameRow row = new AccessibleGameRow(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(12), dp(12), dp(12));
        boolean selected = sameGame(selectedGame, game);
        GameCompatibilityStatus compatibility = compatibilityRepository.statusOf(game.titleId);
        int fill = game.launchable ? 0xff1b2026 : 0xff171b20;
        int stroke = 0xff2a313a;
        if (compatibility == GameCompatibilityStatus.SUPPORTED) {
            fill = 0xff192820;
            stroke = 0xff397352;
        } else if (compatibility == GameCompatibilityStatus.UNSUPPORTED) {
            fill = 0xff2b201d;
            stroke = 0xff8c4f43;
        }
        row.setBackground(roundedBackground(
                selected ? 0xff352844 : fill,
                selected ? 0xffc18cff : stroke, 18));
        row.setElevation(dp(2));

        FrameLayout iconHost = new FrameLayout(activity);
        ImageView icon = new ImageView(activity);
        icon.setScaleType(ImageView.ScaleType.CENTER_CROP);
        LinearLayout.LayoutParams iconLp = new LinearLayout.LayoutParams(dp(82), dp(82));
        iconLp.setMargins(0, 0, dp(16), 0);
        iconHost.addView(icon, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        if (PLATFORM_PS5.equals(game.platform)) {
            TextView badge = label("PS5", 10, 0xffffffff);
            badge.setTypeface(android.graphics.Typeface.DEFAULT,
                    android.graphics.Typeface.BOLD);
            badge.setGravity(Gravity.CENTER);
            badge.setPadding(dp(5), dp(2), dp(5), dp(2));
            badge.setBackground(roundedBackground(0xff1769aa, 0xff89c8ff, 7));
            FrameLayout.LayoutParams badgeLp = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.END | Gravity.BOTTOM);
            badgeLp.setMargins(dp(3), dp(3), dp(3), dp(3));
            iconHost.addView(badge, badgeLp);
        }
        row.addView(iconHost, iconLp);
        Bitmap bitmap = decodeIcon(game.icon, 192);
        if (bitmap != null) {
            icon.setImageBitmap(bitmap);
        } else {
            icon.setBackgroundColor(0xff2a313a);
        }

        LinearLayout textColumn = new LinearLayout(activity);
        textColumn.setOrientation(LinearLayout.VERTICAL);
        TextView title = label(game.title, 16,
                game.launchable ? 0xfff2f5f8 : 0xff9aa6b2);
        title.setTypeface(android.graphics.Typeface.DEFAULT,
                android.graphics.Typeface.BOLD);
        textColumn.addView(title);
        textColumn.addView(label(game.titleId, 12, 0xff7f8a97));
        ProgressBar holdProgress = new ProgressBar(
                activity, null, android.R.attr.progressBarStyleHorizontal);
        holdProgress.setMax((int) GameHoldAction.MARK_SUPPORTED_MS);
        holdProgress.setProgress(0);
        holdProgress.setProgressTintList(
                android.content.res.ColorStateList.valueOf(0xff6fb4ff));
        holdProgress.setProgressBackgroundTintList(
                android.content.res.ColorStateList.valueOf(0xff303741));
        holdProgress.setVisibility(View.GONE);
        LinearLayout.LayoutParams holdProgressLp =
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(4));
        holdProgressLp.setMargins(0, dp(5), 0, 0);
        textColumn.addView(holdProgress, holdProgressLp);
        if (!game.launchable) {
            TextView metadata = label(activity.getString(R.string.metadata_only),
                    11, 0xffffad66);
            metadata.setPadding(0, dp(3), 0, 0);
            textColumn.addView(metadata);
        }
        row.addView(textColumn, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        FrameLayout endHost = new FrameLayout(activity);
        LinearLayout.LayoutParams endHostLp = new LinearLayout.LayoutParams(dp(42), dp(42));
        endHostLp.setMargins(dp(8), 0, 0, 0);
        if (compatibility != GameCompatibilityStatus.UNCLASSIFIED) {
            TextView compatibilityMark = label(
                    compatibility == GameCompatibilityStatus.SUPPORTED ? "\u2713" : "\u00d7",
                    20,
                    compatibility == GameCompatibilityStatus.SUPPORTED
                            ? 0xff67d391 : 0xffff806f);
            compatibilityMark.setGravity(Gravity.CENTER);
            compatibilityMark.setContentDescription(activity.getString(
                    compatibility == GameCompatibilityStatus.SUPPORTED
                            ? R.string.game_status_supported
                            : R.string.game_status_unsupported));
            endHost.addView(compatibilityMark, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        }
        TextView holdCountdown = label("3", 17, 0xffffffff);
        holdCountdown.setGravity(Gravity.CENTER);
        holdCountdown.setAlpha(0.78f);
        holdCountdown.setBackground(roundedBackground(0xb8242b34, 0x996f7c8b, 21));
        holdCountdown.setVisibility(View.GONE);
        endHost.addView(holdCountdown, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        row.addView(endHost, endHostLp);

        TextView gameSettings = label("\u2630", 22, 0xffb8c5d4);
        gameSettings.setGravity(Gravity.CENTER);
        gameSettings.setMinWidth(dp(44));
        gameSettings.setMinHeight(dp(44));
        gameSettings.setPadding(dp(8), 0, dp(4), 0);
        gameSettings.setClickable(true);
        gameSettings.setFocusable(true);
        gameSettings.setContentDescription(activity.getString(
                R.string.game_individual_settings_open, game.title));
        gameSettings.setOnClickListener(view -> openGameSettings(game));
        row.addView(gameSettings, new LinearLayout.LayoutParams(dp(48), dp(48)));

        row.setClickable(true);
        row.setFocusable(true);
        row.setOnClickListener(view -> handleGameTap(game));
        row.setOnDragListener((view, event) -> handleGameDragTarget(
                view, event, game));
        int statusText = compatibility == GameCompatibilityStatus.SUPPORTED
                ? R.string.game_status_supported
                : compatibility == GameCompatibilityStatus.UNSUPPORTED
                ? R.string.game_status_unsupported
                : R.string.game_status_unclassified;
        row.setContentDescription(activity.getString(R.string.game_item_accessibility,
                game.title, game.titleId, activity.getString(statusText)));
        row.setOnTouchListener(new GameHoldTouchListener(
                game, holdProgress, holdCountdown));
        return row;
    }

    private AlertDialog openGameSettings(GameEntry game) {
        if (game == null) return null;
        boolean classified = compatibilityRepository.statusOf(game.titleId)
                != GameCompatibilityStatus.UNCLASSIFIED;
        String[] actions = classified
                ? new String[]{activity.getString(R.string.report_game_settings),
                        activity.getString(R.string.report_action)}
                : new String[]{activity.getString(R.string.report_game_settings)};
        return new AlertDialog.Builder(activity).setTitle(game.title)
                .setItems(actions, (dialog, which) -> {
                    if (which == 0) openIndividualGameSettings(game);
                    else if (compatibilityRepository.statusOf(game.titleId)
                            != GameCompatibilityStatus.UNCLASSIFIED) {
                        activity.startActivity(new Intent(activity, CompatibilityReportActivity.class)
                                .putExtra("title", game.title).putExtra("title_id", game.titleId)
                                .putExtra("platform", PLATFORM_PS5.equals(game.platform) ? "PS5" : "PS4"));
                    }
                }).show();
    }

    private void openIndividualGameSettings(GameEntry game) {
        if (game == null) {
            return;
        }
        GameProfileSettings.rememberTitle(
                SettingsActivity.prefs(activity), game.titleId, game.title);
        Intent intent = new Intent(activity, SettingsActivity.class);
        intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE_ID, game.titleId);
        intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE, game.title);
        activity.startActivity(intent);
    }

    private void handleGameTap(GameEntry game) {
        if (game == null) {
            return;
        }
        if (selectedGame != null) {
            moveSelectedGameTo(game);
            return;
        }
        if (game.launchable) {
            launch(game);
        }
    }

    private void moveSelectedGameTo(GameEntry target) {
        GameEntry source = selectedGame;
        if (source == null || target == null) {
            return;
        }
        if (sameGame(source, target)) {
            return;
        }
        if (sortMode != GameLibraryView.MANUAL || sortReversed || !searchQuery.trim().isEmpty()) {
            selectedGame = null;
            updateRemoveAction();
            toast(activity.getString(R.string.library_move_manual_only));
            renderVisibleGames();
            return;
        }
        List<String> currentOrder = new ArrayList<>(latestGames.size());
        java.util.Map<String, GameEntry> byTitleId = new java.util.HashMap<>();
        for (GameEntry game : latestGames) {
            currentOrder.add(game.titleId);
            byTitleId.put(game.titleId, game);
        }
        List<String> movedOrder = GameLibraryOrder.moveBefore(
                currentOrder, source == null ? null : source.titleId,
                target == null ? null : target.titleId);
        List<GameEntry> reordered = new ArrayList<>(movedOrder.size());
        for (String titleId : movedOrder) {
            GameEntry game = byTitleId.get(titleId);
            if (game != null) {
                reordered.add(game);
            }
        }
        selectedGame = null;
        updateRemoveAction();
        persistGameOrder(reordered);
        toast(activity.getString(R.string.game_move_completed, source.title));
        renderGameList(reordered);
    }

    private boolean handleGameDragTarget(View view, DragEvent event, GameEntry target) {
        if (selectedGame == null) {
            return false;
        }
        switch (event.getAction()) {
        case DragEvent.ACTION_DRAG_STARTED:
            return event.getClipDescription() != null
                    && selectedGame.titleId.equals(event.getLocalState());
        case DragEvent.ACTION_DRAG_ENTERED:
            if (!sameGame(selectedGame, target)) {
                view.setAlpha(0.72f);
            }
            return true;
        case DragEvent.ACTION_DRAG_EXITED:
        case DragEvent.ACTION_DRAG_ENDED:
            view.setAlpha(1f);
            return true;
        case DragEvent.ACTION_DROP:
            view.setAlpha(1f);
            moveSelectedGameTo(target);
            return true;
        default:
            return true;
        }
    }

    private void handleGameHoldAction(GameEntry game, GameHoldAction action) {
        GameCompatibilityStatus status;
        switch (action) {
        case TAP:
            return;
        case SELECT:
            selectedGame = game;
            updateRemoveAction();
            toast(activity.getString(R.string.game_move_selected, game.title));
            renderGameList(latestGames);
            return;
        case CLEAR_SELECTION:
            if (selectedGame != null) {
                selectedGame = null;
                updateRemoveAction();
                renderGameList(latestGames);
            }
            return;
        case MARK_SUPPORTED:
            status = GameCompatibilityStatus.SUPPORTED;
            break;
        case MARK_UNSUPPORTED:
            status = GameCompatibilityStatus.UNSUPPORTED;
            break;
        default:
            return;
        }
        compatibilityRepository.setStatus(game.titleId, status);
        selectedGame = null;
        updateRemoveAction();
        toast(activity.getString(status == GameCompatibilityStatus.SUPPORTED
                ? R.string.game_marked_supported
                : R.string.game_marked_unsupported, game.title));
        renderGameList(latestGames);
    }

    private final class GameHoldTouchListener implements View.OnTouchListener {
        private final GameEntry game;
        private final ProgressBar holdProgress;
        private final TextView holdCountdown;
        private View pressedView;
        private android.graphics.drawable.Drawable restingBackground;
        private long downAtMs;
        private float downX;
        private float downY;
        private boolean active;

        private final Runnable elapsedTicker = new Runnable() {
            @Override
            public void run() {
                if (!active || pressedView == null) {
                    return;
                }
                long elapsedMs = Math.max(0L,
                        android.os.SystemClock.uptimeMillis() - downAtMs);
                holdProgress.setProgress((int) Math.min(
                        elapsedMs, GameHoldAction.MARK_SUPPORTED_MS));
                if (elapsedMs >= GameHoldAction.CLEAR_SELECTION_MS) {
                    int targetSecond = elapsedMs < GameHoldAction.MARK_UNSUPPORTED_MS
                            ? 3 : elapsedMs < GameHoldAction.MARK_SUPPORTED_MS ? 4 : 5;
                    holdCountdown.setText(Integer.toString(targetSecond));
                    holdCountdown.setContentDescription(activity.getString(
                            R.string.game_hold_countdown, targetSecond));
                    holdCountdown.setVisibility(View.VISIBLE);
                } else {
                    holdCountdown.setVisibility(View.GONE);
                }
                holdHandler.postDelayed(this, 100L);
            }
        };

        private final Runnable selectionCue = () -> {
            onSelectionThreshold();
        };
        private final Runnable clearSelectionCue = () -> {
            onClearSelectionThreshold();
        };
        private final Runnable unsupportedCue = () -> {
            onCompatibilityThreshold(GameCompatibilityStatus.UNSUPPORTED,
                    0xff2b201d, 0xffb85d50, HapticFeedbackConstants.CLOCK_TICK);
        };
        private final Runnable supportedCue = () -> {
            onCompatibilityThreshold(GameCompatibilityStatus.SUPPORTED,
                    0xff192820, 0xff4f9a6c, HapticFeedbackConstants.LONG_PRESS);
        };

        GameHoldTouchListener(GameEntry game, ProgressBar holdProgress,
                              TextView holdCountdown) {
            this.game = game;
            this.holdProgress = holdProgress;
            this.holdCountdown = holdCountdown;
        }

        private void onSelectionThreshold() {
            if (!active) {
                return;
            }
            selectedGame = game;
            updateRemoveAction();
            showHoldStage(0xff203754, 0xff6fb4ff,
                    HapticFeedbackConstants.CLOCK_TICK);
        }

        private void onClearSelectionThreshold() {
            if (!active) {
                return;
            }
            if (sameGame(selectedGame, game)) {
                selectedGame = null;
                updateRemoveAction();
            }
            showHoldStage(0xff252a31, 0xff65717e,
                    HapticFeedbackConstants.CLOCK_TICK);
        }

        private void onCompatibilityThreshold(GameCompatibilityStatus status,
                                              int fillColor, int strokeColor,
                                              int feedbackConstant) {
            if (!active) {
                return;
            }
            compatibilityRepository.setStatus(game.titleId, status);
            showHoldStage(fillColor, strokeColor, feedbackConstant);
        }

        @Override
        public boolean onTouch(View view, MotionEvent event) {
            switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                cancel();
                pressedView = view;
                restingBackground = view.getBackground();
                downAtMs = android.os.SystemClock.uptimeMillis();
                downX = event.getX();
                downY = event.getY();
                active = true;
                if (view.getParent() != null) {
                    view.getParent().requestDisallowInterceptTouchEvent(true);
                }
                holdProgress.setProgress(0);
                holdProgress.setProgressTintList(
                        android.content.res.ColorStateList.valueOf(0xff6fb4ff));
                holdProgress.setVisibility(View.VISIBLE);
                elapsedTicker.run();
                holdHandler.postDelayed(selectionCue,
                        GameHoldAction.SELECT_MS);
                holdHandler.postDelayed(clearSelectionCue,
                        GameHoldAction.CLEAR_SELECTION_MS);
                holdHandler.postDelayed(unsupportedCue,
                        GameHoldAction.MARK_UNSUPPORTED_MS);
                holdHandler.postDelayed(supportedCue,
                        GameHoldAction.MARK_SUPPORTED_MS);
                return true;
            case MotionEvent.ACTION_MOVE:
                long moveElapsedMs = android.os.SystemClock.uptimeMillis() - downAtMs;
                boolean moved = Math.abs(event.getX() - downX) > touchSlop
                        || Math.abs(event.getY() - downY) > touchSlop;
                if (active && moved && sameGame(selectedGame, game)) {
                    ClipData data = ClipData.newPlainText(
                            "lsx4-game-title-id", game.titleId);
                    View.DragShadowBuilder shadow = new View.DragShadowBuilder(view);
                    boolean started;
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                        started = view.startDragAndDrop(data, shadow, game.titleId, 0);
                    } else {
                        started = view.startDrag(data, shadow, game.titleId, 0);
                    }
                    if (started) {
                        cancel();
                    }
                } else if (active && moveElapsedMs < GameHoldAction.SELECT_MS
                        && moved) {
                    cancel();
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (!active) {
                    cancel();
                    return true;
                }
                long durationMs = Math.max(0L,
                        android.os.SystemClock.uptimeMillis() - downAtMs);
                GameHoldAction action = GameHoldAction.fromDuration(durationMs);
                cancel();
                if (action == GameHoldAction.TAP) {
                    view.performClick();
                } else {
                    handleGameHoldAction(game, action);
                }
                return true;
            case MotionEvent.ACTION_CANCEL:
            case MotionEvent.ACTION_POINTER_DOWN:
                cancel();
                return true;
            default:
                return true;
            }
        }

        private void cue(int feedbackConstant) {
            if (active && pressedView != null) {
                pressedView.performHapticFeedback(feedbackConstant);
            }
        }

        private void showHoldStage(int fillColor, int strokeColor,
                                   int feedbackConstant) {
            if (!active || pressedView == null) {
                return;
            }
            holdProgress.setProgressTintList(android.content.res.ColorStateList.valueOf(
                    strokeColor));
            pressedView.setBackground(roundedBackground(fillColor, strokeColor, 18));
            cue(feedbackConstant);
        }

        private void cancel() {
            active = false;
            holdHandler.removeCallbacks(selectionCue);
            holdHandler.removeCallbacks(clearSelectionCue);
            holdHandler.removeCallbacks(unsupportedCue);
            holdHandler.removeCallbacks(supportedCue);
            holdHandler.removeCallbacks(elapsedTicker);
            holdProgress.setProgress(0);
            holdProgress.setVisibility(View.GONE);
            holdCountdown.setVisibility(View.GONE);
            if (pressedView != null && restingBackground != null) {
                pressedView.setBackground(restingBackground);
            }
            if (pressedView != null && pressedView.getParent() != null) {
                pressedView.getParent().requestDisallowInterceptTouchEvent(false);
            }
            pressedView = null;
            restingBackground = null;
        }
    }

    private static final class AccessibleGameRow extends LinearLayout {
        AccessibleGameRow(android.content.Context context) {
            super(context);
        }

        @Override
        public boolean performClick() {
            return super.performClick();
        }
    }

    private void updateRemoveAction() {
        if (removeAction != null) {
            removeAction.setVisibility(selectedGame == null ? View.GONE : View.VISIBLE);
        }
    }

    private void confirmRemoveSelectedGame() {
        final GameEntry game = selectedGame;
        if (game == null) {
            return;
        }
        new AlertDialog.Builder(activity)
                .setTitle(R.string.game_remove_title)
                .setMessage(activity.getString(R.string.game_remove_message, game.title))
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.game_remove_confirm,
                        (dialog, which) -> removeSelectedGame())
                .show();
    }

    private void removeSelectedGame() {
        final GameEntry game = selectedGame;
        if (game == null) {
            return;
        }
        selectedGame = null;
        updateRemoveAction();
        if (!workRunning.compareAndSet(false, true)) {
            setStatus(activity.getString(R.string.operation_wait));
            return;
        }
        if (!IMPORT_ACTIVE.compareAndSet(false, true)) {
            workRunning.set(false);
            setStatus(activity.getString(R.string.operation_wait));
            return;
        }
        setBusy(true, activity.getString(R.string.game_removing, game.title));
        Thread worker = new Thread(() -> {
            String message;
            List<GameEntry> games;
            try {
                synchronized (IMPORT_LOCK) {
                    try {
                        File boundary = game.rootPriority == 0
                                ? appInstallRoot() : checkedChild(homeDir(), "games");
                        ensureWithin(game.directory, boundary);
                        deleteTreeWithin(game.directory, boundary);
                        compatibilityRepository.clear(game.titleId);
                        message = activity.getString(R.string.game_removed, game.title);
                    } catch (Exception error) {
                        Log.e(TAG, "Could not remove " + game.titleId, error);
                        message = activity.getString(R.string.game_remove_failed, game.title);
                    }
                    try {
                        games = scanInstalledGames();
                    } catch (Exception ignored) {
                        games = Collections.emptyList();
                    }
                }
            } finally {
                IMPORT_ACTIVE.set(false);
                workRunning.set(false);
            }
            finishUiWork(games, message);
        }, "lsx4-game-remove");
        worker.setDaemon(true);
        worker.start();
    }

    private void launch(GameEntry game) {
        if (workRunning.get() || IMPORT_ACTIVE.get()) {
            toast(activity.getString(R.string.import_wait_before_launch));
            return;
        }
        Intent intent = new Intent(activity, GameActivity.class);
        intent.addFlags(MainActivity.crossProcessTaskFlags());
        intent.putExtra("autoload_existing_runtime", true);
        intent.putExtra("autoinit_runtime", true);
        String launchPath = PLATFORM_PS5.equals(game.platform)
                ? game.directory.getAbsolutePath()
                : game.eboot.getAbsolutePath();
        intent.putExtra("scan_game_path", launchPath);
        intent.putExtra("launch_game_path", launchPath);
        intent.putExtra(EXTRA_GAME_PLATFORM, game.platform);
        intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE_ID, game.titleId);
        intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE, game.title);
        intent.putExtra("fullscreen_render", true);
        intent.putExtra(EXTRA_EMBEDDED_AARCH64_JIT_BACKEND, true);
        SharedPreferences launchPreferences = SettingsActivity.prefsForGame(
                activity, game.titleId);
        intent.putExtra(MainActivity.EXTRA_RENDER_RESOLUTION_MODE,
                launchPreferences.getInt(
                        SettingsActivity.K_RES_MODE, SettingsActivity.DEFAULT_RES_MODE));
        try {
            activity.routeIntentToGameActivity(intent);
            finishLauncherAfterGameHandoff();
        } catch (ActivityNotFoundException e) {
            toast(activity.getString(R.string.game_activity_unavailable));
        }
    }

    private void finishLauncherAfterGameHandoff() {
        MainActivity.retireProcessAfterTaskHandoff(activity, "launcher");
    }

    private void copySafDirectory(Uri treeUri, Uri sourceDirectory, File destination,
                                  File stagingBoundary, int depth, CopyProgress copied,
                                  Set<String> visitedDocumentIds)
            throws Exception {
        checkCancelled();
        if (depth > MAX_TREE_DEPTH) {
            throw new IOException("слишком глубокое дерево папок");
        }
        String documentId = DocumentsContract.getDocumentId(sourceDirectory);
        if (!visitedDocumentIds.add(documentId)) {
            throw new IOException("провайдер вернул циклическое дерево папок");
        }
        ensureWithin(destination, stagingBoundary);
        ensureDirectory(destination);
        Set<String> childNames = new HashSet<>();
        int remainingNodes = MAX_IMPORT_NODES - copied.nodes;
        if (remainingNodes <= 0) {
            throw new IOException("слишком много объектов в импортируемой игре");
        }
        long remainingMetadataChars = MAX_DOCUMENT_METADATA_CHARS - copied.metadataChars;
        if (remainingMetadataChars <= 0) {
            throw new IOException("слишком много метаданных в дереве игры");
        }
        List<SafNode> children = listChildren(treeUri, sourceDirectory,
                Math.min(MAX_DIRECTORY_CHILDREN, remainingNodes), remainingMetadataChars);
        if (children.size() > MAX_IMPORT_NODES - copied.nodes) {
            throw new IOException("слишком много объектов в импортируемой игре");
        }
        long reservedMetadataChars = 0;
        for (SafNode child : children) {
            reservedMetadataChars += child.metadataChars;
        }
        if (reservedMetadataChars > MAX_DOCUMENT_METADATA_CHARS - copied.metadataChars) {
            throw new IOException("слишком много метаданных в дереве игры");
        }
        copied.nodes += children.size();
        copied.metadataChars += reservedMetadataChars;
        for (SafNode child : children) {
            validateDocumentName(child.name);
            if (!childNames.add(child.name)) {
                throw new IOException("повторяющееся имя в папке: " + child.name);
            }
            File output = checkedChild(destination, child.name);
            ensureWithin(output, stagingBoundary);
            if (child.directory) {
                copySafDirectory(treeUri, child.uri, output, stagingBoundary, depth + 1, copied,
                        visitedDocumentIds);
            } else {
                if (child.size >= 0 && child.size > copied.maxBytes - copied.bytes) {
                    throw new IOException("недостаточно места для файла " + child.name);
                }
                copyDocument(child.uri, output, copied);
            }
        }
    }

    private void copyDocument(Uri source, File destination, CopyProgress copied) throws Exception {
        checkCancelled();
        if (copied.files >= MAX_IMPORT_FILES) {
            throw new IOException("слишком много файлов в импортируемой игре");
        }
        ContentResolver resolver = activity.getContentResolver();
        try (InputStream input = resolver.openInputStream(source);
             FileOutputStream output = new FileOutputStream(destination)) {
            if (input == null) {
                throw new IOException("провайдер не открыл " + source);
            }
            byte[] buffer = new byte[COPY_BUFFER_BYTES];
            int count;
            while ((count = input.read(buffer)) != -1) {
                checkCancelled();
                if (copied.bytes > copied.maxBytes - count) {
                    throw new IOException("импорт остановлен: заканчивается свободное место");
                }
                output.write(buffer, 0, count);
                copied.bytes += count;
                if (copied.bytes - copied.lastReportedBytes >= 32L * 1024L * 1024L) {
                    copied.lastReportedBytes = copied.bytes;
                    postStatus(activity.getString(R.string.import_copy_progress,
                            copied.files, formatBytes(copied.bytes)));
                }
            }
            output.getFD().sync();
        }
        copied.files++;
    }

    private List<SafNode> listChildren(Uri treeUri, Uri parentDocument) throws IOException {
        return listChildren(treeUri, parentDocument, MAX_DIRECTORY_CHILDREN,
                MAX_DOCUMENT_METADATA_CHARS);
    }

    private List<SafNode> listChildren(Uri treeUri, Uri parentDocument, int maximumChildren,
                                       long maximumMetadataChars) throws IOException {
        if (maximumChildren <= 0) {
            throw new IOException("слишком много объектов в импортируемой игре");
        }
        if (maximumMetadataChars <= 0) {
            throw new IOException("слишком много метаданных в дереве игры");
        }
        String parentId = DocumentsContract.getDocumentId(parentDocument);
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId);
        String[] projection = new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE
        };
        List<SafNode> nodes = new ArrayList<>();
        long metadataChars = 0;
        try (Cursor cursor = activity.getContentResolver().query(
                childrenUri, projection, null, null, null)) {
            if (cursor == null) {
                throw new IOException("провайдер не вернул содержимое папки");
            }
            while (cursor.moveToNext()) {
                if (nodes.size() >= maximumChildren) {
                    throw new IOException("слишком много объектов в одной папке");
                }
                String documentId = cursor.getString(0);
                String name = cursor.getString(1);
                String mime = cursor.getString(2);
                long size = cursor.isNull(3) ? -1 : cursor.getLong(3);
                if (documentId == null || documentId.length() > MAX_DOCUMENT_ID_LENGTH) {
                    throw new IOException("провайдер вернул некорректный document ID");
                }
                if (name == null || name.length() > MAX_DOCUMENT_NAME_LENGTH) {
                    throw new IOException("слишком длинное имя в выбранной папке");
                }
                Uri uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
                int nodeMetadataChars = documentId.length() + name.length()
                        + (mime == null ? 0 : mime.length()) + uri.toString().length() + 64;
                if (metadataChars > maximumMetadataChars - nodeMetadataChars) {
                    throw new IOException("слишком много метаданных в папке");
                }
                metadataChars += nodeMetadataChars;
                nodes.add(new SafNode(uri, name,
                        DocumentsContract.Document.MIME_TYPE_DIR.equals(mime), size,
                        nodeMetadataChars));
            }
        } catch (SecurityException e) {
            throw new IOException("нет доступа к выбранной папке", e);
        }
        return nodes;
    }

    private SafNode findDirectChild(Uri treeUri, Uri parent, String name, boolean directory)
            throws IOException {
        for (SafNode child : listChildren(treeUri, parent)) {
            if (name.equals(child.name) && child.directory == directory) {
                return child;
            }
        }
        return null;
    }

    private void installStagingTree(File staging, File destination, File stagingRoot)
            throws Exception {
        ensureWithin(staging, stagingRoot);
        ensureWithin(destination, appInstallRoot());
        if (destination.exists() || Files.isSymbolicLink(destination.toPath())) {
            throw new IOException("игра " + destination.getName()
                    + " уже установлена; слияние файлов запрещено");
        }
        try {
            Files.move(staging.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE);
        } catch (AtomicMoveNotSupportedException ignored) {
            Files.move(staging.toPath(), destination.toPath());
        }
    }

    private static void moveReplace(File source, File target) throws IOException {
        try {
            Files.move(source.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException e) {
            Files.move(source.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private static void deleteTreeWithin(File path, File boundary) throws IOException {
        ensureWithin(path, boundary);
        File[] children = path.listFiles();
        if (children != null) {
            for (File child : children) {
                if (child.isDirectory()) {
                    deleteTreeWithin(child, boundary);
                } else if (!child.delete()) {
                    throw new IOException("не удалось удалить временный файл " + child.getName());
                }
            }
        }
        if (path.exists() && !path.delete()) {
            throw new IOException("не удалось очистить staging " + path.getName());
        }
    }

    private void writeInstallManifest(File directory, String titleId, String title, String source,
                                      boolean ebootExtracted, String installedBy,
                                      String platform) throws Exception {
        JSONObject json = new JSONObject();
        json.put("titleId", titleId);
        json.put("title", title == null ? "" : title);
        json.put("source", source);
        json.put("ebootExtracted", ebootExtracted);
        json.put("installedBy", installedBy);
        json.put("platform", platform);
        File target = checkedChild(directory, "executor-installed.json");
        File temporary = checkedChild(directory, ".executor-installed.json.tmp");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            output.write(json.toString().getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        }
        moveReplace(temporary, target);
    }

    private PkgMetadata preflightPkg(File pkg) throws Exception {
        try (RandomAccessFile file = new RandomAccessFile(pkg, "r")) {
            long length = file.length();
            if (length < 32 || u32be(file, 0) != 0x7f434e54L) {
                throw new IOException("файл не является PS4 PKG");
            }
            long entryCount = u32be(file, 0x10);
            long tableOffset = u32be(file, 0x18);
            if (entryCount <= 0 || entryCount > 8192 || tableOffset < 0
                    || tableOffset > length || entryCount > (length - tableOffset) / 32L) {
                throw new IOException("повреждена таблица PKG");
            }
            byte[] paramSfo = null;
            int paramSfoEntries = 0;
            for (long index = 0; index < entryCount; index++) {
                long entry = tableOffset + index * 32L;
                long id = u32be(file, entry);
                if (id != PkgInstaller.ENTRY_PARAM_SFO) {
                    continue;
                }
                paramSfoEntries++;
                if (paramSfoEntries > 1) {
                    throw new IOException("PKG содержит несколько param.sfo");
                }
                long offset = u32be(file, entry + 16);
                long size = u32be(file, entry + 20);
                if (size <= 0 || size > MAX_SFO_BYTES || offset > length
                        || size > length - offset) {
                    throw new IOException("повреждён param.sfo в PKG");
                }
                paramSfo = new byte[(int) size];
                file.seek(offset);
                file.readFully(paramSfo);
            }
            if (paramSfo == null) {
                throw new IOException("PKG не содержит param.sfo");
            }
            String id = normalizeTitleId(PkgInstaller.sfoString(paramSfo, "TITLE_ID"));
            String title = cleanSfoString(PkgInstaller.sfoString(paramSfo, "TITLE"));
            return new PkgMetadata(id, title);
        }
    }

    private long copyUriToFile(Uri uri, File destination, long maximumBytes) throws Exception {
        long copied = 0;
        try (InputStream input = activity.getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(destination)) {
            if (input == null) {
                throw new IOException("провайдер не открыл выбранный PKG");
            }
            byte[] buffer = new byte[COPY_BUFFER_BYTES];
            int count;
            long lastReport = 0;
            while ((count = input.read(buffer)) != -1) {
                checkCancelled();
                if (copied > maximumBytes - count) {
                    throw new IOException("импорт остановлен: заканчивается свободное место");
                }
                output.write(buffer, 0, count);
                copied += count;
                if (copied - lastReport >= 64L * 1024L * 1024L) {
                    lastReport = copied;
                    postStatus(activity.getString(R.string.import_pkg_copy_progress,
                            formatBytes(copied)));
                }
            }
            output.getFD().sync();
        } catch (Exception e) {
            if (destination.exists()) {
                destination.delete();
            }
            throw e;
        }
        return copied;
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor cursor = activity.getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getString(0);
            }
        } catch (Exception ignored) {
        }
        return uri.getLastPathSegment();
    }

    private long querySize(Uri uri) {
        try (Cursor cursor = activity.getContentResolver().query(
                uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return Math.max(0, cursor.getLong(0));
            }
        } catch (Exception ignored) {
        }
        return -1;
    }

    private static byte[] readUriLimited(ContentResolver resolver, Uri uri, int maximum)
            throws IOException {
        try (InputStream input = resolver.openInputStream(uri);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            if (input == null) {
                throw new IOException("провайдер не открыл param.sfo");
            }
            byte[] buffer = new byte[8192];
            int total = 0;
            int count;
            while ((count = input.read(buffer)) != -1) {
                total += count;
                if (total > maximum) {
                    throw new IOException("param.sfo слишком большой");
                }
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        }
    }

    private static byte[] readFileLimited(File file, int maximum) throws IOException {
        long length = file.length();
        if (length <= 0 || length > maximum) {
            throw new IOException("metadata file has invalid size");
        }
        byte[] data = new byte[(int) length];
        try (FileInputStream input = new FileInputStream(file)) {
            int offset = 0;
            while (offset < data.length) {
                int count = input.read(data, offset, data.length - offset);
                if (count < 0) {
                    throw new IOException("truncated metadata file");
                }
                offset += count;
            }
        }
        return data;
    }

    private static Ps5Metadata readPs5Metadata(byte[] data) throws IOException {
        try {
            JSONObject json = new JSONObject(new String(data, StandardCharsets.UTF_8));
            String titleId = normalizeTitleId(json.optString("titleId", ""));
            if (!GamePlatformMetadata.isPs5TitleId(titleId)) return null;
            String title = "";
            JSONObject localized = json.optJSONObject("localizedParameters");
            if (localized != null) {
                String language = localized.optString("defaultLanguage", "");
                JSONObject selected = localized.optJSONObject(language);
                if (selected == null) {
                    java.util.Iterator<String> keys = localized.keys();
                    while (keys.hasNext() && selected == null) {
                        Object value = localized.opt(keys.next());
                        if (value instanceof JSONObject) {
                            selected = (JSONObject) value;
                        }
                    }
                }
                if (selected != null) {
                    title = selected.optString("titleName", "");
                }
            }
            return new Ps5Metadata(titleId, title.trim());
        } catch (Exception error) {
            throw new IOException("повреждён sce_sys/param.json", error);
        }
    }

    private String readSfoValue(File sfo, String key) throws IOException {
        long length = sfo.length();
        if (length <= 0 || length > MAX_SFO_BYTES) {
            return null;
        }
        byte[] data = new byte[(int) length];
        try (FileInputStream input = new FileInputStream(sfo)) {
            int offset = 0;
            while (offset < data.length) {
                int count = input.read(data, offset, data.length - offset);
                if (count < 0) {
                    throw new IOException("обрезанный param.sfo");
                }
                offset += count;
            }
        }
        String value = cleanSfoString(PkgInstaller.sfoString(data, key));
        return "TITLE_ID".equals(key) ? normalizeTitleId(value) : value;
    }

    private static long u32be(RandomAccessFile file, long offset) throws IOException {
        file.seek(offset);
        int b0 = file.read();
        int b1 = file.read();
        int b2 = file.read();
        int b3 = file.read();
        if ((b0 | b1 | b2 | b3) < 0) {
            throw new IOException("unexpected EOF at " + offset);
        }
        return ((long) b0 << 24) | ((long) b1 << 16) | ((long) b2 << 8) | b3;
    }

    private File homeDir() throws IOException {
        return checkedChild(activity.getFilesDir(), "lsx4-home");
    }

    private File appInstallRoot() throws IOException {
        File runtimeFs = checkedChild(homeDir(), "runtime-fs");
        File user = checkedChild(runtimeFs, "user");
        return checkedChild(user, "app");
    }

    private long availableImportBudget(File existingDirectory) throws IOException {
        ensureDirectory(existingDirectory);
        StatFs stat = new StatFs(existingDirectory.getAbsolutePath());
        long available = stat.getAvailableBytes();
        long budget = available - FREE_SPACE_RESERVE_BYTES;
        if (budget <= 0) {
            throw new IOException("недостаточно свободного места; требуется резерв не менее 512 MiB");
        }
        return budget;
    }

    private static File checkedChild(File parent, String name) throws IOException {
        validateDocumentName(name);
        File child = new File(parent, name);
        ensureWithin(child, parent);
        return child;
    }

    private static File uniqueChild(File parent, String requestedName) throws IOException {
        File candidate = checkedChild(parent, requestedName);
        if (candidate.createNewFile()) {
            return candidate;
        }
        int dot = requestedName.lastIndexOf('.');
        String stem = dot > 0 ? requestedName.substring(0, dot) : requestedName;
        String extension = dot > 0 ? requestedName.substring(dot) : "";
        for (int suffix = 1; suffix <= 10000; suffix++) {
            candidate = checkedChild(parent, stem + "-" + suffix + extension);
            if (candidate.createNewFile()) {
                return candidate;
            }
        }
        throw new IOException("слишком много файлов с одинаковым именем");
    }

    private static void validateDocumentName(String name) throws IOException {
        if (name == null || name.isEmpty() || ".".equals(name) || "..".equals(name)
                || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0
                || name.indexOf('\0') >= 0) {
            throw new IOException("небезопасное имя файла");
        }
    }

    private static String safeFileName(String name, String fallback) {
        if (name == null || name.trim().isEmpty()) {
            return fallback;
        }
        String safe = name.replaceAll("[^A-Za-z0-9._ -]", "_").trim();
        while (safe.startsWith(".")) {
            safe = safe.substring(1);
        }
        return safe.isEmpty() || ".".equals(safe) || "..".equals(safe) ? fallback : safe;
    }

    private static void ensureWithin(File child, File parent) throws IOException {
        String childPath = child.getCanonicalPath();
        String parentPath = parent.getCanonicalPath();
        if (!childPath.equals(parentPath)
                && !childPath.startsWith(parentPath + File.separator)) {
            throw new IOException("путь выходит за границу импорта");
        }
    }

    private static void ensureDirectory(File directory) throws IOException {
        if (directory.isDirectory()) {
            return;
        }
        if (directory.exists() || !directory.mkdirs()) {
            throw new IOException("не удалось создать " + directory.getName());
        }
    }

    private static boolean safeRegularFileWithin(File file, File boundary) {
        try {
            if (!file.isFile() || Files.isSymbolicLink(file.toPath())) {
                return false;
            }
            ensureWithin(file, boundary);
            return true;
        } catch (IOException ignored) {
            return false;
        }
    }

    private static boolean isPlausibleEboot(File file, File boundary) {
        if (!safeRegularFileWithin(file, boundary) || file.length() < 4) {
            return false;
        }
        byte[] magic = new byte[4];
        try (FileInputStream input = new FileInputStream(file)) {
            if (input.read(magic) != magic.length) {
                return false;
            }
        } catch (IOException ignored) {
            return false;
        }
        boolean elf = (magic[0] & 0xff) == 0x7f
                && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
        boolean ps4Self = (magic[0] & 0xff) == 0x4f
                && (magic[1] & 0xff) == 0x15
                && (magic[2] & 0xff) == 0x3d
                && (magic[3] & 0xff) == 0x1d;
        return elf || ps4Self;
    }

    private static File findInstalledEboot(File directory) {
        File exact = new File(directory, "eboot.bin");
        if (isUsableInstalledBinary(exact, directory)) {
            return exact;
        }
        File[] children = directory.listFiles();
        if (children == null) {
            return null;
        }
        for (File child : children) {
            if ("eboot.bin".equalsIgnoreCase(child.getName())
                    && isUsableInstalledBinary(child, directory)) {
                return child;
            }
        }
        return null;
    }

    private static boolean isUsableInstalledBinary(File file, File boundary) {
        return safeRegularFileWithin(file, boundary) && file.length() >= 4;
    }

    private static boolean isOrphanedMetadataInstall(File manifest) {
        try {
            JSONObject json = new JSONObject(new String(
                    readFileLimited(manifest, MAX_SFO_BYTES), StandardCharsets.UTF_8));
            if (json.optBoolean("ebootExtracted", false)) {
                return true;
            }
            String source = json.optString("source", "").trim();
            if (source.isEmpty() || source.contains("://")) {
                return false;
            }
            File sourceFile = new File(source);
            return sourceFile.isAbsolute() && !sourceFile.isFile();
        } catch (Exception ignored) {
            return false;
        }
    }

    private void installPkgStagingTree(File staging, File destination, File stagingRoot)
            throws Exception {
        ensureWithin(staging, stagingRoot);
        ensureWithin(destination, appInstallRoot());
        if (!destination.exists()) {
            installStagingTree(staging, destination, stagingRoot);
            return;
        }
        if (!destination.isDirectory() || Files.isSymbolicLink(destination.toPath())
                || findInstalledEboot(destination) != null) {
            throw new IOException("игра " + destination.getName()
                    + " уже установлена; её файлы не будут изменены");
        }

        File backup = checkedChild(stagingRoot,
                destination.getName() + "-metadata-backup-"
                        + Long.toUnsignedString(System.nanoTime()));
        moveDirectory(destination, backup);
        boolean installed = false;
        try {
            installStagingTree(staging, destination, stagingRoot);
            installed = true;
        } finally {
            if (!installed && backup.exists() && !destination.exists()) {
                moveDirectory(backup, destination);
            }
            if (installed && backup.exists()) {
                deleteTreeWithin(backup, stagingRoot);
            }
        }
    }

    private static void moveDirectory(File source, File target) throws IOException {
        try {
            Files.move(source.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
        } catch (AtomicMoveNotSupportedException ignored) {
            Files.move(source.toPath(), target.toPath());
        }
    }

    private void checkCancelled() throws IOException {
        if (destroyed || Thread.currentThread().isInterrupted()) {
            throw new IOException("импорт отменён: Activity закрыта");
        }
    }

    private static boolean isValidTitleId(String titleId) {
        return titleId != null && TITLE_ID.matcher(titleId).matches();
    }

    private static String normalizeTitleId(String value) {
        String clean = cleanSfoString(value);
        return clean == null ? null : clean.toUpperCase(Locale.ROOT);
    }

    private static String cleanSfoString(String value) {
        if (value == null) {
            return null;
        }
        int nul = value.indexOf('\0');
        String clean = (nul >= 0 ? value.substring(0, nul) : value).trim();
        return clean.isEmpty() ? null : clean;
    }

    private static String displayTitle(String title, String titleId) {
        return title == null || title.isEmpty() ? titleId : title;
    }

    private static String formatBytes(long bytes) {
        if (bytes >= 1024L * 1024L * 1024L) {
            return String.format(Locale.US, "%.2f GiB", bytes / (1024.0 * 1024.0 * 1024.0));
        }
        if (bytes >= 1024L * 1024L) {
            return String.format(Locale.US, "%.1f MiB", bytes / (1024.0 * 1024.0));
        }
        if (bytes >= 1024L) {
            return String.format(Locale.US, "%.1f KiB", bytes / 1024.0);
        }
        return bytes + " B";
    }

    private static String friendlyMessage(Throwable throwable) {
        String message = throwable.getMessage();
        return message == null || message.trim().isEmpty()
                ? throwable.getClass().getSimpleName() : message;
    }

    private static Bitmap decodeIcon(File icon, int requestedPixels) {
        if (icon == null || !icon.isFile()) {
            return null;
        }
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(icon.getAbsolutePath(), bounds);
            int sample = 1;
            while (bounds.outWidth / sample > requestedPixels * 2
                    || bounds.outHeight / sample > requestedPixels * 2) {
                sample *= 2;
            }
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            return BitmapFactory.decodeFile(icon.getAbsolutePath(), options);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private void finishUiWork(List<GameEntry> games, String message) {
        postUi(() -> {
            renderGameList(games);
            setBusy(false, message);
        });
    }

    private void setBusy(boolean busy, String message) {
        if (progress != null) {
            progress.setVisibility(busy ? View.VISIBLE : View.GONE);
        }
        if (status != null) {
            status.setText(message);
        }
        if (homeView != null) {
            homeView.setStatus(message, busy);
        }
    }

    private void setStatus(String message) {
        postUi(() -> {
            if (status != null) {
                status.setText(message);
            }
            if (homeView != null) {
                homeView.setStatus(message, progress != null
                        && progress.getVisibility() == View.VISIBLE);
            }
        });
    }

    private void postStatus(String message) {
        setStatus(message);
    }

    private void postUi(Runnable runnable) {
        activity.runOnUiThread(() -> {
            if (destroyed || activity.isFinishing()
                    || (Build.VERSION.SDK_INT >= 17 && activity.isDestroyed())) {
                return;
            }
            runnable.run();
        });
    }

    private void toast(String message) {
        postUi(() -> Toast.makeText(activity, message, Toast.LENGTH_LONG).show());
    }

    private TextView label(String text, int sp, int color) {
        TextView label = new TextView(activity);
        label.setText(text);
        label.setTextSize(sp);
        label.setTextColor(color);
        return label;
    }

    private GradientDrawable roundedBackground(int fill, int stroke, int radiusDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(dp(radiusDp));
        drawable.setStroke(dp(1), stroke);
        return drawable;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams matchWrapWithMargins(int left, int top, int right,
                                                            int bottom) {
        LinearLayout.LayoutParams lp = matchWrap();
        lp.setMargins(dp(left), dp(top), dp(right), dp(bottom));
        return lp;
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private static int clamp(int value, int minimum, int maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }

    private interface ImportJob {
        String run() throws Exception;
    }

    private static final class GameEntry {
        final File directory;
        final File eboot;
        final File icon;
        final String titleId;
        final String title;
        final String platform;
        final boolean launchable;
        final int rootPriority;
        final long addedAtMillis;

        GameEntry(File directory, File eboot, File icon, String titleId, String title,
                  String platform, boolean launchable, int rootPriority,
                  long addedAtMillis) {
            this.directory = directory;
            this.eboot = eboot;
            this.icon = icon;
            this.titleId = titleId;
            this.title = title;
            this.platform = platform;
            this.launchable = launchable;
            this.rootPriority = rootPriority;
            this.addedAtMillis = addedAtMillis;
        }
    }

    private static final class Ps5Metadata {
        final String titleId;
        final String title;

        Ps5Metadata(String titleId, String title) {
            this.titleId = titleId;
            this.title = title;
        }
    }

    private static final class SafNode {
        final Uri uri;
        final String name;
        final boolean directory;
        final long size;
        final int metadataChars;

        SafNode(Uri uri, String name, boolean directory, long size, int metadataChars) {
            this.uri = uri;
            this.name = name;
            this.directory = directory;
            this.size = size;
            this.metadataChars = metadataChars;
        }
    }

    private static final class CopyProgress {
        int nodes;
        int files;
        long bytes;
        long lastReportedBytes;
        long metadataChars;
        final long maxBytes;

        CopyProgress(long maxBytes) {
            this.maxBytes = maxBytes;
        }

        void reserveNode(boolean directory) throws IOException {
            nodes++;
            if (nodes > MAX_IMPORT_NODES) {
                throw new IOException("слишком много записей в архиве");
            }
            if (!directory) {
                files++;
                if (files > MAX_IMPORT_FILES) {
                    throw new IOException("слишком много файлов в архиве");
                }
            }
        }

        void reserveFile(long declaredBytes) throws IOException {
            if (declaredBytes > maxBytes || (declaredBytes >= 0
                    && bytes > maxBytes - declaredBytes)) {
                throw new IOException("распаковка остановлена: недостаточно свободного места");
            }
        }

        void addExtracted(long count) throws IOException {
            if (count < 0 || bytes > maxBytes - count) {
                throw new IOException("распаковка остановлена: недостаточно свободного места");
            }
            bytes += count;
        }
    }

    private static final class BudgetOutputStream extends OutputStream {
        private final OutputStream output;
        private final CopyProgress progress;
        private final long declaredBytes;
        private long written;

        BudgetOutputStream(OutputStream output, CopyProgress progress, long declaredBytes) {
            this.output = output;
            this.progress = progress;
            this.declaredBytes = declaredBytes;
        }

        @Override
        public void write(int value) throws IOException {
            byte[] one = new byte[]{(byte) value};
            write(one, 0, 1);
        }

        @Override
        public void write(byte[] buffer, int offset, int count) throws IOException {
            if (declaredBytes >= 0 && written > declaredBytes - count) {
                throw new IOException("запись архива превышает заявленный размер");
            }
            progress.addExtracted(count);
            output.write(buffer, offset, count);
            written += count;
        }

        @Override
        public void flush() throws IOException {
            output.flush();
        }

        @Override
        public void close() throws IOException {
            output.close();
        }
    }

    private static final class PkgMetadata {
        final String titleId;
        final String title;

        PkgMetadata(String titleId, String title) {
            this.titleId = titleId;
            this.title = title;
        }
    }
}
