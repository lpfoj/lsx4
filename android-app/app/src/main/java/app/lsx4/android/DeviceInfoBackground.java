package app.lsx4.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;
import java.util.Random;


final class DeviceInfoBackground extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float[][] icons = new float[12][6];
    private final AndroidMenuGlyph androidGlyph = new AndroidMenuGlyph();
    private final long epoch = SystemClock.uptimeMillis();
    private Shader base, vignette;
    private boolean active;
    private PlayStationHomeView.Theme theme = PlayStationHomeView.THEMES[0];

    DeviceInfoBackground(Context context) {
        super(context);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        Random random = new Random(0x505332);
        for (float[] orb : icons) {
            orb[0] = random.nextFloat();
            orb[1] = random.nextFloat();
            orb[2] = random.nextFloat() * 40 + 20;
            orb[3] = random.nextFloat() * 0.3f + 0.1f;
            orb[4] = random.nextFloat() * 6.28f;
            orb[5] = random.nextFloat() * 0.15f + 0.05f;
        }
    }

    void setActive(boolean value) { active = value; if (value) invalidate(); }

    void setTheme(int index) {
        theme = PlayStationHomeView.THEMES[Math.max(0, Math.min(3, index))];
        onSizeChanged(getWidth(), getHeight(), 0, 0); invalidate();
    }

    @Override protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        if (w <= 0 || h <= 0) return;
        base = new RadialGradient(w * 0.3f, h * 0.5f, Math.max(w, h) * 0.9f,
            new int[]{theme.gradientStart, theme.background, theme.gradientEnd}, null, Shader.TileMode.CLAMP);
        vignette = new RadialGradient(w * 0.5f, h * 0.5f, Math.max(w, h) * 0.8f,
            0x00000000, 0x99000000, Shader.TileMode.CLAMP);
    }

    @Override protected void onDraw(Canvas canvas) {
        if (base == null) return;
        paint.setShader(base);
        canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
        paint.setShader(null);
        float time = (SystemClock.uptimeMillis() - epoch) / 1000f;
        for (float[] orb : icons) {
            float x = orb[0] * getWidth() + (float)Math.sin(time * orb[3] + orb[4]) * orb[5] * getWidth();
            float y = orb[1] * getHeight() + (float)Math.cos(time * orb[3] * 0.7f + orb[4]) * orb[5] * getHeight();
            androidGlyph.draw(canvas, x, y, orb[2] * 1.5f,
                (float)Math.sin(time * 0.2f + orb[4]) * 12f, (theme.primary & 0xffffff) | 0x40000000);
        }
        paint.setShader(vignette);
        canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
        paint.setShader(null);
        if (active && isShown() && getWindowVisibility() == VISIBLE) postInvalidateOnAnimation();
    }
}
