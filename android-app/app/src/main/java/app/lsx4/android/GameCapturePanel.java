package app.lsx4.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;


final class GameCapturePanel extends FrameLayout {
    interface Actions {
        void screenshot();
        void toggleRecording();
        void gallery();
    }

    private final LinearLayout tray;
    private final ImageButton record;
    private final TextView status;
    private final View handle;
    private boolean expanded;
    private boolean recordingActive;
    private String recordingLabel = "";
    private float downX;
    private float downY;
    private boolean dragged;
    private float outlineHeight;
    private float outlineWidth;
    private View geometryParent;
    private final OnLayoutChangeListener parentLayout =
            (view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> updatePosition();

    GameCapturePanel(Context context, Actions actions) {
        super(context);
        outlineHeight = dp(52);
        outlineWidth = dp(7);
        setClipChildren(false);
        setClipToPadding(false);
        tray = new LinearLayout(context);
        tray.setOrientation(LinearLayout.VERTICAL);
        tray.setGravity(Gravity.CENTER);
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xeb18202b);
        background.setCornerRadii(new float[]{0, 0, dp(16), dp(16), dp(16), dp(16), 0, 0});
        tray.setBackground(background);
        LinearLayout buttons = new LinearLayout(context);
        buttons.setGravity(Gravity.CENTER);
        buttons.addView(button(R.drawable.capture_screenshot, R.string.capture_screenshot,
                () -> { collapse(); actions.screenshot(); }));
        record = button(R.drawable.capture_video, R.string.capture_record,
                () -> { collapse(); actions.toggleRecording(); });
        buttons.addView(record);
        buttons.addView(button(R.drawable.capture_gallery, R.string.capture_gallery,
                () -> { collapse(); actions.gallery(); }));
        tray.addView(buttons);
        status = new TextView(context);
        status.setTextSize(10);
        status.setTextColor(0xffff8a8a);
        status.setGravity(Gravity.CENTER);
        status.setVisibility(GONE);
        tray.addView(status, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(20)));
        LayoutParams trayParams = new LayoutParams(dp(164), dp(78), Gravity.START | Gravity.CENTER_VERTICAL);
        addView(tray, trayParams);
        tray.setVisibility(GONE);
        handle = new View(context) {
            final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
            final Path path = new Path();
            @Override protected void onDraw(Canvas canvas) {
                float stroke = Math.max(1f, outlineWidth * 0.27f);
                paint.setColor(0xa05c6166);
                paint.setStrokeWidth(stroke);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeCap(Paint.Cap.BUTT);
                paint.setStrokeJoin(Paint.Join.MITER);
                float middle = getHeight() / 2f;
                path.reset();
                float outer = expanded ? getWidth() + stroke : -stroke;
                float tip = expanded ? getWidth() - outlineWidth : outlineWidth - stroke / 2;
                float top = middle - outlineHeight / 2;
                float bottom = middle + outlineHeight / 2;
                path.moveTo(outer, top);
                path.lineTo(tip, top + outlineWidth);
                path.lineTo(tip, bottom - outlineWidth);
                path.lineTo(outer, bottom);
                canvas.drawPath(path, paint);
                if (recordingActive) {
                    paint.setStyle(Paint.Style.FILL);
                    paint.setColor(0xffff6666);
                    canvas.drawCircle(expanded ? dp(12) : dp(18), top - dp(4), dp(3), paint);
                    if (!expanded) {
                        paint.setTextSize(dp(10));

                        String time = recordingLabel.replaceFirst("^.*?(\\d+:\\d+).*$", "$1");
                        if (!time.matches("\\d+:\\d+")) time = "…";
                        canvas.drawText(time, dp(5), bottom + dp(12), paint);
                    }
                }
            }
            @Override public boolean performClick() {
                super.performClick();
                setExpanded(!expanded);
                return true;
            }
        };
        handle.setClickable(true);
        handle.setFocusable(true);
        handle.setContentDescription(context.getString(R.string.capture_open_panel));
        handle.setOnTouchListener((view, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                downX = event.getRawX();
                downY = event.getRawY();
                dragged = false;
                return true;
            }
            if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
                float delta = event.getRawX() - downX;
                if (!dragged && Math.abs(delta) > ViewConfiguration.get(context).getScaledTouchSlop()
                        && Math.abs(delta) > Math.abs(event.getRawY() - downY)) {
                    dragged = true;
                    setExpanded(delta > 0);
                }
                return true;
            }
            if (event.getActionMasked() == MotionEvent.ACTION_UP) {
                if (!dragged) view.performClick();
                return true;
            }
            return event.getActionMasked() == MotionEvent.ACTION_CANCEL;
        });
        addView(handle, new LayoutParams(dp(24), ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.LEFT | Gravity.CENTER_VERTICAL));
        setLayoutParams(new FrameLayout.LayoutParams(dp(24), dp(80), Gravity.LEFT | Gravity.TOP));
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (getParent() instanceof View) {
            geometryParent = (View) getParent();
            geometryParent.addOnLayoutChangeListener(parentLayout);
        }
        post(this::updatePosition);
    }

    @Override protected void onDetachedFromWindow() {
        if (geometryParent != null) geometryParent.removeOnLayoutChangeListener(parentLayout);
        geometryParent = null;
        tray.animate().cancel();
        super.onDetachedFromWindow();
    }

    private void updatePosition() {
        if (geometryParent == null || geometryParent.getHeight() <= 0) return;
        int height = geometryParent.getHeight();


        outlineHeight = height * (82f / 931f);
        outlineWidth = Math.max(2f, height * (13f / 931f));
        int wantedHeight = Math.max(dp(expanded ? 80 : 48), Math.round(outlineHeight) + dp(28));
        int top = Math.max(0, Math.min(height - wantedHeight,
                Math.round(height * 0.25f - wantedHeight / 2f)));
        FrameLayout.LayoutParams own = (FrameLayout.LayoutParams) getLayoutParams();
        if (own.height != wantedHeight || own.topMargin != top) {
            own.height = wantedHeight;
            own.topMargin = top;
            own.gravity = Gravity.LEFT | Gravity.TOP;
            setLayoutParams(own);
        }
        handle.invalidate();
    }

    void setRecording(boolean recording) {
        recordingActive = recording;
        record.setImageResource(recording ? R.drawable.capture_stop : R.drawable.capture_video);
        record.setContentDescription(getContext().getString(recording
                ? R.string.capture_stop_recording : R.string.capture_record));
        status.setVisibility(recording ? VISIBLE : GONE);
        if (!recording) status.setText("");
        updateHandleBounds();
        handle.invalidate();
    }

    void setRecordingStatus(String label) {
        recordingLabel = label;
        status.setText(label);
        handle.setContentDescription(getContext().getString(R.string.capture_open_panel)
                + (label.isEmpty() ? "" : ", " + label));
        handle.invalidate();
    }

    boolean isExpanded() { return expanded; }
    void collapse() { setExpanded(false); }

    private void setExpanded(boolean next) {
        if (expanded == next) return;
        expanded = next;
        tray.animate().cancel();
        updateHandleBounds();
        if (expanded) {
            tray.setVisibility(VISIBLE);
            tray.setAlpha(0f);
            tray.setTranslationX(-dp(24));
            tray.animate().alpha(1f).translationX(0).setDuration(160).start();
        } else {
            tray.setVisibility(GONE);
        }
        handle.invalidate();
    }

    private void updateHandleBounds() {
        FrameLayout.LayoutParams own = (FrameLayout.LayoutParams) getLayoutParams();
        if (own == null) return;
        own.width = dp(expanded ? 188 : (recordingActive ? 52 : 24));
        setLayoutParams(own);
        FrameLayout.LayoutParams grip = (FrameLayout.LayoutParams) handle.getLayoutParams();
        grip.leftMargin = dp(expanded ? 162 : 0);
        grip.width = dp(!expanded && recordingActive ? 52 : 24);
        handle.setLayoutParams(grip);
        updatePosition();
    }

    private ImageButton button(int icon, int label, Runnable action) {
        ImageButton button = new ImageButton(getContext());
        button.setImageResource(icon);
        button.setContentDescription(getContext().getString(label));
        android.util.TypedValue value = new android.util.TypedValue();
        getContext().getTheme().resolveAttribute(android.R.attr.selectableItemBackgroundBorderless,
                value, true);
        button.setBackgroundResource(value.resourceId);
        button.setLayoutParams(new LinearLayout.LayoutParams(dp(48), dp(48)));
        button.setOnClickListener(view -> action.run());
        return button;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
