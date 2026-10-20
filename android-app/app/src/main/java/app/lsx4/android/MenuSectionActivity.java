package app.lsx4.android;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;


public final class MenuSectionActivity extends Activity {
    private static final String CATALOG_VIDEO_URL = "https://www.youtube.com/watch?v=jRe0-FwPfY0";
    static final String EXTRA_SECTION = "menu_section";
    static final String EXTRA_STYLE = "menu_style";
    private SharedPreferences preferences;
    private MenuCardList selector;
    private DeviceInfoBackground backdrop;
    private MenuSoundEngine sounds;
    private LinearLayout details;
    private ScrollView detailsScroll;
    private CompatibilityCatalog catalog;
    private String section, selectedKey;
    private int style, theme;
    private volatile int request;
    private volatile boolean closed, active;
    private final ExecutorService worker = Executors.newFixedThreadPool(2);
    private final Set<String> pendingThumbnails = new HashSet<>();
    private final Set<String> loadedThumbnails = new HashSet<>();

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        preferences = getSharedPreferences("lsx4_library_layout", MODE_PRIVATE);
        section = getIntent().getStringExtra(EXTRA_SECTION);
        style = state == null ? getIntent().getIntExtra(EXTRA_STYLE, 2) : state.getInt("style", 2);
        if (style < 1 || style > 6) style = 2;
        selectedKey = state == null ? null : state.getString("selected");
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR);
        theme = Math.max(0, Math.min(3, preferences.getInt("launcher_home_theme_v1", 0)));
        if (selectedKey == null && !"compatibility".equals(section)) selectedKey = "menu:theme:" + theme;
        sounds = new MenuSoundEngine(this, preferences);
        try { if ("compatibility".equals(section)) catalog = new CompatibilityCatalog(this); }
        catch (Exception error) { catalog = null; }
        build();
    }
    private boolean horizontal() {
        return getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
    }
    private void build() {
        FrameLayout scene = new FrameLayout(this);
        backdrop = new DeviceInfoBackground(this); backdrop.setTheme(theme);
        scene.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(horizontal() ? 12 : 8), dp(12), dp(horizontal() ? 12 : 8), dp(12));
        LinearLayout header = new LinearLayout(this); header.setGravity(Gravity.CENTER_VERTICAL);
        TextView back = action("‹ " + getString(R.string.back)); back.setOnClickListener(v -> finish());
        header.addView(back);
        TextView heading = text(getString(catalog != null || "compatibility".equals(section)
                ? R.string.menu_compatibility_card : R.string.menu_settings_card), horizontal() ? 20 : 16, 0xffeef2f6);
        header.addView(heading, new LinearLayout.LayoutParams(0, -2, 1));
        if (catalog != null) {
            TextView count = text(Integer.toString(catalog.entries.size()), 14,
                    PlayStationHomeView.THEMES[theme].primary);
            count.setTypeface(null, 1); header.addView(count);
        }
        root.addView(header);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.HORIZONTAL);
        selector = new MenuCardList(this);
        selector.setTheme(theme); selector.setMenuStyle(style);
        selector.setCompactRail(!horizontal());
        selector.setSounds(sounds);
        selector.setListener(new MenuCardList.Listener() {
            public void onSelected(PlayStationHomeView.Item item) { showDetails(item); }
            public void onVisible(PlayStationHomeView.Item item) { requestThumbnail(item); }
        });
        body.addView(selector, new LinearLayout.LayoutParams(0, -1, horizontal() ? 0.38f : 0.30f));
        detailsScroll = new ScrollView(this); detailsScroll.setFillViewport(true);
        details = new LinearLayout(this); details.setOrientation(LinearLayout.VERTICAL);
        details.setPadding(dp(horizontal() ? 12 : 8), dp(8), dp(horizontal() ? 12 : 8), dp(12));
        detailsScroll.addView(details);
        LinearLayout.LayoutParams detailLp = new LinearLayout.LayoutParams(0, -1, horizontal() ? 0.62f : 0.70f);
        detailLp.setMargins(dp(6), 0, 0, 0); body.addView(detailsScroll, detailLp);
        root.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));
        scene.addView(root, new FrameLayout.LayoutParams(-1, -1)); setContentView(scene);
        refreshItems(); selector.revealSelection(); selector.requestFocus();
    }
    private void refreshItems() {
        List<PlayStationHomeView.Item> items = new ArrayList<>();
        if (catalog != null) {
            for (CompatibilityCatalog.Entry game : catalog.entries) {
                File image = catalog.imageFile(this, game);
                items.add(new PlayStationHomeView.Item("catalog:" + game.image, game.title, "", game.platform,
                        image.isFile() ? image : null, image.isFile() ? image : null, false, false,
                        PlayStationHomeView.STATUS_SUPPORTED, game));
            }
        } else if (!"compatibility".equals(section)) {
            for (int i = 0; i < PlayStationHomeView.THEMES.length; i++)
                items.add(option("theme:" + i, getString(PlayStationHomeView.THEMES[i].name)));
            items.add(option("effects", getString(R.string.home_sound_effects)));
            items.add(option("music", getString(R.string.home_music)));
            items.add(option("emulation", getString(R.string.menu_emulation_settings)));
        }
        selector.setItems(items);
        if (selectedKey == null && !items.isEmpty()) selectedKey = items.get(0).key;
        if (selectedKey != null) selector.selectKey(selectedKey);
    }
    private PlayStationHomeView.Item option(String key, String title) {
        return new PlayStationHomeView.Item("menu:" + key, title, "", null, null, null,
                true, false, PlayStationHomeView.STATUS_UNCLASSIFIED, key);
    }
    private void choose(PlayStationHomeView.Item item) {
        if (item.tag instanceof CompatibilityCatalog.Entry) { showDetails(item); return; }
        String key = (String) item.tag;
        if (key.startsWith("theme:")) {
            theme = Integer.parseInt(key.substring(6));
            preferences.edit().putInt("launcher_home_theme_v1", theme).apply();
            selector.setTheme(theme); backdrop.setTheme(theme);
        } else if (key.equals("effects")) sounds.setEffectsEnabled(!sounds.effectsEnabled());
        else if (key.equals("music")) sounds.setMusicEnabled(!sounds.musicEnabled());
        else if (key.equals("emulation")) startActivity(new Intent(this, SettingsActivity.class));
        showDetails(item);
    }
    private void showDetails(PlayStationHomeView.Item item) {
        boolean changed = !item.key.equals(selectedKey);
        selectedKey = item.key;
        final int token = ++request;
        details.removeAllViews();
        if (changed) detailsScroll.scrollTo(0, 0);
        details.setBackground(panel());
        TextView title = text(item.title, horizontal() ? 21 : 18, 0xffeeeeee);
        title.setTypeface(null, 1); details.addView(title);
        if (!(item.tag instanceof CompatibilityCatalog.Entry)) {
            String key = (String) item.tag;
            details.addView(text(getString(key.startsWith("theme:") ? R.string.menu_theme_apply
                    : R.string.menu_section_open), 15, 0xffbdd4e9));
            if (key.startsWith("theme:")) {
                int index = Integer.parseInt(key.substring(6));
                PlayStationHomeView.Theme palette = PlayStationHomeView.THEMES[index];
                TextView swatch = text(getString(palette.name), 28, palette.highlight);
                GradientDrawable gradient = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                        new int[]{palette.gradientStart, palette.background, palette.gradientEnd});
                gradient.setCornerRadius(dp(12)); swatch.setBackground(gradient);
                swatch.setGravity(Gravity.CENTER); details.addView(swatch, new LinearLayout.LayoutParams(-1, dp(110)));
                if (index == theme) details.addView(text(getString(R.string.menu_selected), 15, palette.primary));
            } else if (key.equals("effects") || key.equals("music")) {
                boolean enabled = key.equals("music") ? sounds.musicEnabled() : sounds.effectsEnabled();
                details.addView(text(getString(enabled ? R.string.system_info_yes : R.string.system_info_no),
                        22, 0xff67d391));
            }
            TextView apply = action(getString(R.string.menu_apply)); apply.setOnClickListener(v -> choose(item));
            details.addView(apply); return;
        }
        CompatibilityCatalog.Entry game = (CompatibilityCatalog.Entry) item.tag;
        TextView status = text(game.platform + " · " + getString(R.string.game_status_supported),
                14, 0xff67d391); details.addView(status);
        ImageView image = new ImageView(this); image.setAdjustViewBounds(true);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER); image.setContentDescription(game.title);
        details.addView(image, new LinearLayout.LayoutParams(-1, dp(horizontal() ? 165 : 110)));
        TextView imageState = text(getString(R.string.menu_screenshot_loading), 11, 0xff94a4b5);
        details.addView(imageState);
        details.addView(text(getString(R.string.menu_test_devices), 13, 0xffc5d3e2));
        showPerformance(game.performance);
        details.addView(text(getString(R.string.menu_device_scope), 11, 0xff94a4b5));
        if (!game.notes.isEmpty()) details.addView(text(game.notes, 13, 0xffc5d3e2));
        TextView source = action("GitHub · " + catalog.captured + " · " + catalog.revision.substring(0, 7));
        source.setOnClickListener(v -> startActivity(new Intent(Intent.ACTION_VIEW,
                Uri.parse("https://github.com/llayerlinux/lsx4/tree/" + catalog.revision + "#playable-games"))));
        details.addView(source);
        TextView video = action(getString(R.string.menu_catalog_video));
        video.setOnClickListener(v -> startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(CATALOG_VIDEO_URL))));
        details.addView(video);
        if (!active) return;
        worker.execute(() -> {
            if (closed || !active || token != request) return;
            Bitmap bitmap = null;
            try { bitmap = catalog.screenshot(this, game); } catch (Exception unavailable) { }
            final Bitmap result = bitmap;
            runOnUiThread(() -> {
                if (closed || !active || token != request) { if (result != null) result.recycle(); return; }
                if (result == null) { imageState.setText(R.string.menu_screenshot_unavailable); return; }
                image.setImageBitmap(result); imageState.setText(R.string.menu_screenshot_source);
                requestThumbnail(item);
            });
        });
    }

    private void showPerformance(CompatibilityCatalog.Performance performance) {
        details.addView(text("Snapdragon 8 Gen 5 · Adreno 840", 14, 0xffeef2f6));
        details.addView(text("FPS: " + (performance.samples.isEmpty() ? getString(R.string.menu_fps_unpublished)
                : performance.range(0) + " · " + getString(R.string.menu_fps_video)), 14, 0xff67d391));
        if (!performance.samples.isEmpty()) details.addView(text(getString(R.string.menu_fps_sample_scope),
                11, 0xff94a4b5));
        details.addView(text("Snapdragon 8 Gen 3 · Adreno 750", 14, 0xffeef2f6));
        String estimate = performance.smallIndie ? "50–60" : performance.aaa ? performance.range(-10) : "";
        details.addView(text("FPS: " + (estimate.isEmpty() ? getString(R.string.menu_fps_unpublished)
                : "≈ " + estimate + " · " + getString(R.string.menu_fps_estimate)), 14, 0xffeef2f6));
        if (!estimate.isEmpty()) details.addView(text(getString(performance.aaa ? R.string.menu_fps_aaa_policy
                : R.string.menu_fps_indie_policy), 11, 0xff94a4b5));
    }

    private void requestThumbnail(PlayStationHomeView.Item item) {
        if (!active || closed || !(item.tag instanceof CompatibilityCatalog.Entry)
                || loadedThumbnails.contains(item.key) || !pendingThumbnails.add(item.key)) return;
        worker.execute(() -> {
            Bitmap thumbnail = null;
            if (!closed && active) try {
                Bitmap full = catalog.screenshot(this, (CompatibilityCatalog.Entry) item.tag);
                float scale = Math.min(1f, 256f / Math.max(full.getWidth(), full.getHeight()));
                thumbnail = Bitmap.createScaledBitmap(full, Math.max(1, Math.round(full.getWidth() * scale)),
                        Math.max(1, Math.round(full.getHeight() * scale)), true);
                if (thumbnail != full) full.recycle();
            } catch (Exception unavailable) { }
            final Bitmap result = thumbnail;
            runOnUiThread(() -> {
                pendingThumbnails.remove(item.key);
                if (closed || !active) { if (result != null) result.recycle(); return; }
                if (result != null) {
                    loadedThumbnails.add(item.key); selector.updateArtwork(item.key, result);
                }
            });
        });
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused && android.os.Build.VERSION.SDK_INT >= 30 && getWindow().getInsetsController() != null)
            getWindow().getInsetsController().hide(android.view.WindowInsets.Type.systemBars());
    }
    @Override protected void onResume() {
        super.onResume(); active = true; backdrop.setActive(true); selector.setMenuActive(true);
        if (selectedKey != null) selector.repeatSelection();
    }
    @Override protected void onPause() {
        active = false; request++; backdrop.setActive(false); selector.setMenuActive(false); super.onPause();
    }
    @Override protected void onDestroy() {
        closed = true; request++; worker.shutdownNow(); selector.release(); sounds.release(); super.onDestroy();
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        state.putInt("style", style); state.putString("selected", selectedKey); super.onSaveInstanceState(state);
    }
    @Override public boolean onKeyDown(int key, KeyEvent event) {
        if (key == KeyEvent.KEYCODE_BUTTON_B) { finish(); return true; }
        return super.onKeyDown(key, event);
    }
    private GradientDrawable panel() {
        GradientDrawable background = new GradientDrawable(); background.setColor(0xdd101417);
        background.setCornerRadius(dp(14)); background.setStroke(dp(1), PlayStationHomeView.THEMES[theme].primary);
        return background;
    }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this); view.setText(value); view.setTextSize(size); view.setTextColor(color);
        view.setPadding(dp(8), dp(6), dp(8), dp(6)); return view;
    }
    private TextView action(String value) {
        TextView view = text(value, 14, PlayStationHomeView.THEMES[theme].primary);
        view.setFocusable(true); view.setClickable(true); view.setBackgroundResource(android.R.drawable.list_selector_background);
        return view;
    }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
}
