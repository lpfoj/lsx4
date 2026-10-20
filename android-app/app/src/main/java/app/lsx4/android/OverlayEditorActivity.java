package app.lsx4.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;


public final class OverlayEditorActivity extends Activity {
    private SharedPreferences preferences;
    private MainActivity.DualShockOverlayView gameplayPreview;
    private EditorCanvas editor;
    private TextView selectionLabel;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLocale.wrap(base));
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        getWindow().setStatusBarColor(0xff070a0f);
        getWindow().setNavigationBarColor(0xff070a0f);
        String titleId = getIntent().getStringExtra(
                GameProfileSettings.EXTRA_GAME_TITLE_ID);
        preferences = SettingsActivity.prefsForGame(this, titleId);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xff070a0f);
        gameplayPreview = new MainActivity.DualShockOverlayView(
                this, preferences, null);
        int opacity = Math.max(20, Math.min(100,
                preferences.getInt(SettingsActivity.K_OVERLAY_OPACITY, 74)));
        gameplayPreview.setAlpha((opacity / 100f) * (112f / 255f));
        root.addView(gameplayPreview, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        editor = new EditorCanvas();
        root.addView(editor, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setOrientation(LinearLayout.HORIZONTAL);
        toolbar.setGravity(Gravity.CENTER_VERTICAL);
        toolbar.setPadding(dp(12), dp(8), dp(12), dp(8));
        GradientDrawable toolbarBackground = new GradientDrawable();
        toolbarBackground.setColor(0xe818202b);
        toolbarBackground.setStroke(dp(1), 0x557aa7dc);
        toolbarBackground.setCornerRadius(dp(18));
        toolbar.setBackground(toolbarBackground);
        toolbar.setElevation(dp(24));

        selectionLabel = actionLabel(getString(R.string.overlay_editor_select_hint), false);
        toolbar.addView(selectionLabel, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        addAction(toolbar, R.string.overlay_editor_hide, view -> editor.hideSelected());
        addAction(toolbar, R.string.overlay_editor_add, view -> showHiddenElements());
        addAction(toolbar, R.string.overlay_editor_reset, view -> confirmReset());
        addAction(toolbar, R.string.overlay_editor_done, view -> finish());

        FrameLayout.LayoutParams toolbarParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        toolbarParams.setMargins(dp(16), dp(12), dp(16), 0);
        root.addView(toolbar, toolbarParams);

        TextView hint = actionLabel(getString(R.string.overlay_editor_gesture_hint), false);
        hint.setGravity(Gravity.CENTER);
        hint.setTextColor(0xff9fb0c5);
        FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        hintParams.setMargins(dp(16), 0, dp(16), dp(12));
        root.addView(hint, hintParams);
        setContentView(root);
        hideSystemBarsWhenAttached(root);
    }

    private void hideSystemBarsWhenAttached(View root) {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.R) {
            return;
        }



        root.post(() -> {
            WindowInsetsController controller = root.getWindowInsetsController();
            if (controller == null) {
                return;
            }
            controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
            controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        });
    }

    @Override
    protected void onPause() {
        if (editor != null) {
            editor.persistSelected();
        }
        super.onPause();
    }

    private void addAction(LinearLayout toolbar, int label, View.OnClickListener listener) {
        TextView action = actionLabel(getString(label), true);
        action.setOnClickListener(listener);
        toolbar.addView(action, actionParams());
    }

    private LinearLayout.LayoutParams actionParams() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(40));
        params.leftMargin = dp(7);
        return params;
    }

    private TextView actionLabel(String text, boolean clickable) {
        TextView label = new TextView(this);
        label.setText(text);
        label.setTextColor(clickable ? 0xfff1f6fc : 0xffaab8c9);
        label.setTextSize(clickable ? 13 : 12);
        label.setGravity(Gravity.CENTER);
        label.setPadding(dp(13), 0, dp(13), 0);
        if (clickable) {
            GradientDrawable background = new GradientDrawable();
            background.setColor(0xff28374a);
            background.setStroke(dp(1), 0x667fb4ee);
            background.setCornerRadius(dp(12));
            label.setBackground(background);
            label.setClickable(true);
            label.setFocusable(true);
        }
        return label;
    }

    private void showHiddenElements() {
        List<OverlayLayoutStore.Element> hidden = editor.hiddenElements();
        if (hidden.isEmpty()) {
            new AlertDialog.Builder(this)
                    .setMessage(R.string.overlay_editor_none_hidden)
                    .setPositiveButton(android.R.string.ok, null)
                    .show();
            return;
        }
        String[] labels = new String[hidden.size()];
        for (int index = 0; index < hidden.size(); ++index) {
            labels[index] = hidden.get(index).label;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.overlay_editor_add)
                .setItems(labels, (dialog, which) -> editor.restore(hidden.get(which)))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void confirmReset() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.overlay_editor_reset)
                .setMessage(R.string.overlay_editor_reset_confirm)
                .setPositiveButton(R.string.overlay_editor_reset, (dialog, which) -> {
                    OverlayLayoutStore.reset(preferences);
                    editor.reload();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private final class EditorCanvas extends View {
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Map<OverlayLayoutStore.Element, RectF> rectangles = new LinkedHashMap<>();
        private OverlayLayoutStore.State[] states;
        private OverlayLayoutStore.State selected;
        private float lastX;
        private float lastY;
        private boolean dragging;
        private final ScaleGestureDetector scaleDetector;

        EditorCanvas() {
            super(OverlayEditorActivity.this);
            setClickable(true);
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeWidth(dp(2));
            text.setTextAlign(Paint.Align.CENTER);
            text.setTypeface(Typeface.DEFAULT_BOLD);
            text.setTextSize(dp(11));
            states = OverlayLayoutStore.readAll(preferences);
            scaleDetector = new ScaleGestureDetector(OverlayEditorActivity.this,
                    new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                        @Override
                        public boolean onScale(ScaleGestureDetector detector) {
                            if (selected == null) {
                                return false;
                            }
                            selected.scale = Math.max(0.45f, Math.min(2.2f,
                                    selected.scale * detector.getScaleFactor()));
                            gameplayPreview.previewLayout(states);
                            invalidate();
                            return true;
                        }

                        @Override
                        public void onScaleEnd(ScaleGestureDetector detector) {
                            persistSelected();
                        }
                    });
        }

        void reload() {
            states = OverlayLayoutStore.readAll(preferences);
            gameplayPreview.previewLayout(states);
            selected = null;
            updateSelectionLabel();
            invalidate();
        }

        void persistSelected() {
            if (selected != null) {
                OverlayLayoutStore.write(preferences, selected);
            }
        }

        void hideSelected() {
            if (selected == null) {
                return;
            }
            selected.visible = false;
            OverlayLayoutStore.write(preferences, selected);
            gameplayPreview.previewLayout(states);
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            selected = null;
            updateSelectionLabel();
            invalidate();
        }

        void restore(OverlayLayoutStore.Element element) {
            OverlayLayoutStore.State state = OverlayLayoutStore.find(states, element);
            state.visible = true;
            OverlayLayoutStore.write(preferences, state);
            gameplayPreview.previewLayout(states);
            selected = state;
            updateSelectionLabel();
            invalidate();
        }

        List<OverlayLayoutStore.Element> hiddenElements() {
            List<OverlayLayoutStore.Element> hidden = new ArrayList<>();
            for (OverlayLayoutStore.State state : states) {
                if (!state.visible) {
                    hidden.add(state.element);
                }
            }
            return hidden;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            computeRectangles();
            for (OverlayLayoutStore.State state : states) {
                if (!state.visible) {
                    continue;
                }
                RectF rect = rectangles.get(state.element);
                if (rect != null) {
                    drawControl(canvas, state, rect);
                }
            }
        }

        private void computeRectangles() {
            rectangles.clear();
            for (OverlayLayoutStore.State state : states) {
                rectangles.put(state.element,
                        gameplayPreview.controlBounds(state.element));
            }
        }

        private void drawControl(Canvas canvas, OverlayLayoutStore.State state, RectF rect) {
            if (selected != state) {
                return;
            }
            stroke.setColor(0xffffffff);
            stroke.setStrokeWidth(dp(2));
            canvas.drawRoundRect(rect, dp(12), dp(12), stroke);
            text.setTextSize(dp(9));
            text.setColor(0xffbfe1ff);
            canvas.drawText(Math.round(state.scale * 100f) + "%", rect.centerX(),
                    rect.bottom + dp(13), text);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            scaleDetector.onTouchEvent(event);
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    selected = hit(event.getX(), event.getY());
                    lastX = event.getX();
                    lastY = event.getY();
                    dragging = selected != null;
                    if (dragging) {
                        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    }
                    updateSelectionLabel();
                    invalidate();
                    return true;
                case MotionEvent.ACTION_MOVE:
                    if (dragging && selected != null && !scaleDetector.isInProgress()) {
                        selected.offsetX += (event.getX() - lastX) / Math.max(1f, getWidth());
                        selected.offsetY += (event.getY() - lastY) / Math.max(1f, getHeight());
                        selected.offsetX = Math.max(-1f, Math.min(1f, selected.offsetX));
                        selected.offsetY = Math.max(-1f, Math.min(1f, selected.offsetY));
                        lastX = event.getX();
                        lastY = event.getY();
                        gameplayPreview.previewLayout(states);
                        invalidate();
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    dragging = false;
                    persistSelected();
                    return true;
                default:
                    return true;
            }
        }

        private OverlayLayoutStore.State hit(float x, float y) {
            for (int index = states.length - 1; index >= 0; --index) {
                OverlayLayoutStore.State state = states[index];
                RectF rect = rectangles.get(state.element);
                if (state.visible && rect != null && rect.contains(x, y)) {
                    return state;
                }
            }
            return null;
        }

        private void updateSelectionLabel() {
            selectionLabel.setText(selected == null
                    ? getString(R.string.overlay_editor_select_hint)
                    : getString(R.string.overlay_editor_selected, selected.element.label));
        }
    }
}
