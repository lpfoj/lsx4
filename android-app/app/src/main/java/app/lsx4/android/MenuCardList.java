package app.lsx4.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.GridLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;


final class MenuCardList extends ScrollView {
    interface Listener {
        void onSelected(PlayStationHomeView.Item item);
        void onVisible(PlayStationHomeView.Item item);
    }
    private static final class Card {
        final PlayStationHomeView.Item item;
        final LinearLayout root;
        final ImageView image;
        Card(PlayStationHomeView.Item item, LinearLayout root, ImageView image) {
            this.item = item; this.root = root; this.image = image;
        }
    }
    private final List<Card> cards = new ArrayList<>();
    private Listener listener;
    private String selected;
    private int theme, style;
    private boolean active = true;
    private boolean compactRail;
    private MenuSoundEngine sounds;
    MenuCardList(Context context) { super(context); setFillViewport(true); setClipToPadding(false); }
    void setListener(Listener value) { listener = value; }
    void setSounds(MenuSoundEngine value) { sounds = value; }
    void setMenuStyle(int value) { style = value; }
    void setCompactRail(boolean value) { compactRail = value; }
    void setTheme(int value) { theme = value; updateHighlight(); }
    void setMenuActive(boolean value) { active = value; if (value) post(this::visibleCards); }
    void setItems(List<PlayStationHomeView.Item> items) {
        removeAllViews(); cards.clear();
        boolean tiles = !compactRail && (style == 5 || style == 6);
        boolean stacked = compactRail || tiles;
        LinearLayout rows = new LinearLayout(getContext()); rows.setOrientation(LinearLayout.VERTICAL);
        GridLayout grid = new GridLayout(getContext()); grid.setColumnCount(2);
        int index = 0;
        for (PlayStationHomeView.Item item : items) {
            LinearLayout card = new LinearLayout(getContext());
            card.setOrientation(stacked ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
            card.setGravity(compactRail ? Gravity.CENTER_HORIZONTAL : Gravity.CENTER_VERTICAL);
            int padding = dp(compactRail ? 6 : 10); card.setPadding(padding, padding, padding, padding);
            card.setFocusable(true); card.setClickable(true); card.setContentDescription(item.title);
            ImageView image = null;
            if (item.utility && item.key.startsWith("menu:theme:")) {
                int palette = Integer.parseInt(item.key.substring("menu:theme:".length()));
                card.addView(new ThemeIcon(getContext(), palette), new LinearLayout.LayoutParams(dp(64), dp(52)));
            } else {
                image = new ImageView(getContext()); image.setScaleType(ImageView.ScaleType.FIT_CENTER);
                image.setBackgroundColor(0xff0a0d10);
                if (item.utility) image.setImageResource(item.key.endsWith("emulation")
                        ? android.R.drawable.ic_menu_preferences : android.R.drawable.ic_lock_silent_mode_off);
                else image.setImageResource(android.R.drawable.ic_menu_gallery);
                card.addView(image, new LinearLayout.LayoutParams(stacked ? -1 : dp(84), dp(52)));
            }
            LinearLayout labels = new LinearLayout(getContext()); labels.setOrientation(LinearLayout.VERTICAL);
            labels.setPadding(dp(stacked ? 0 : 10), dp(stacked ? 6 : 0), 0, 0);
            TextView title = label(item.title, compactRail ? 13 : 16, 0xfff4f6f8);
            title.setTypeface(null, 1); title.setShadowLayer(dp(2), 0, dp(1), 0xff000000);
            title.setMinLines(tiles ? 2 : 1); labels.addView(title);
            if (compactRail) title.setGravity(Gravity.CENTER);
            if (item.status == PlayStationHomeView.STATUS_SUPPORTED)
                labels.addView(label(compactRail ? item.platform : item.platform + " · Playable", 11, 0xff67d391));
            card.addView(labels, new LinearLayout.LayoutParams(stacked ? -1 : 0, -2, stacked ? 0 : 1));
            final Card record = new Card(item, card, image); cards.add(record);
            card.setOnClickListener(v -> { if (sounds != null) sounds.tick(); selectKey(item.key); });
            card.setOnFocusChangeListener((v, focused) -> {
                if (focused && !item.key.equals(selected)) selectKey(item.key);
            });
            if (tiles) {
                GridLayout.LayoutParams lp = new GridLayout.LayoutParams(GridLayout.spec(index / 2),
                        GridLayout.spec(index % 2, 1f));
                lp.width = 0; lp.height = -2; lp.setMargins(dp(4), dp(4), dp(4), dp(4));
                grid.addView(card, lp);
            } else {
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
                lp.setMargins(dp(4), dp(4), dp(4), dp(4)); rows.addView(card, lp);
            }
            index++;
        }
        addView(tiles ? grid : rows, new ScrollView.LayoutParams(-1, -2));
        if (selected == null && !cards.isEmpty()) selected = cards.get(0).item.key;
        updateHighlight(); post(this::visibleCards);
    }
    void selectKey(String key) {
        selected = key; updateHighlight();
        for (Card card : cards) if (card.item.key.equals(key) && listener != null) {
            listener.onSelected(card.item); return;
        }
    }
    void repeatSelection() { if (selected != null) selectKey(selected); }
    void revealSelection() {
        post(() -> {
            for (Card card : cards) if (card.item.key.equals(selected)) {
                card.root.requestRectangleOnScreen(new Rect(0, 0,
                        card.root.getWidth(), card.root.getHeight()), true);
                return;
            }
        });
    }
    void updateArtwork(String key, Bitmap bitmap) {
        for (Card card : cards) if (card.item.key.equals(key) && card.image != null) {
            card.image.setImageBitmap(bitmap); return;
        }
    }
    private void updateHighlight() {
        for (Card card : cards) {
            boolean chosen = card.item.key.equals(selected);
            GradientDrawable background = new GradientDrawable(); background.setCornerRadius(dp(12));
            background.setColor(chosen ? 0xf0222b32 : 0xee10161c);
            background.setStroke(dp(chosen ? 2 : 1), chosen ? PlayStationHomeView.THEMES[theme].primary : 0xff34414e);
            card.root.setBackground(background); card.root.setSelected(chosen);
        }
    }
    private void visibleCards() {
        if (!active || listener == null || !isShown()) return;
        Rect visible = new Rect();
        for (Card card : cards) if (!card.item.utility && card.root.getLocalVisibleRect(visible))
            listener.onVisible(card.item);
    }
    @Override protected void onScrollChanged(int x, int y, int oldX, int oldY) {
        super.onScrollChanged(x, y, oldX, oldY); visibleCards();
    }
    @Override protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        super.onSizeChanged(w, h, oldW, oldH); post(this::visibleCards);
    }
    void release() { active = false; listener = null; cards.clear(); }
    private TextView label(String value, int size, int color) {
        TextView view = new TextView(getContext()); view.setText(value); view.setTextSize(size); view.setTextColor(color);
        return view;
    }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }


    private static final class ThemeIcon extends View {
        private final PlayStationHomeView.Theme palette;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path wave = new Path();
        private final AndroidMenuGlyph glyph = new AndroidMenuGlyph();
        private Shader gradient;
        ThemeIcon(Context context, int theme) { super(context); palette = PlayStationHomeView.THEMES[theme]; }
        @Override protected void onSizeChanged(int w, int h, int oldW, int oldH) {
            gradient = new LinearGradient(0, 0, w, h, new int[]{palette.gradientStart, palette.gradientEnd},
                    null, Shader.TileMode.CLAMP);
            wave.reset(); wave.moveTo(0, h * .8f); wave.cubicTo(w * .25f, 0, w * .7f, h, w, h * .2f);
        }
        @Override protected void onDraw(Canvas canvas) {
            paint.setShader(gradient); canvas.drawRoundRect(0, 0, getWidth(), getHeight(), 12, 12, paint);
            paint.setShader(null); paint.setColor(palette.primary); paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3); canvas.drawPath(wave, paint); paint.setStyle(Paint.Style.FILL);
            glyph.draw(canvas, getWidth() * .5f, getHeight() * .5f, getHeight() * .55f, 0, palette.highlight);
        }
    }
}
