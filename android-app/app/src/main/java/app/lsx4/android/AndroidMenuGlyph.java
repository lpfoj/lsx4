package app.lsx4.android;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;


final class AndroidMenuGlyph {
    private final Path body = new Path();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    AndroidMenuGlyph() {
        body.moveTo(-8, -3);
        body.arcTo(new RectF(-8, -11, 8, 5), 180, 180, false);
        body.lineTo(-8, -3);
        body.close();
        body.addRoundRect(new RectF(-8, -1, 8, 10), 1.5f, 1.5f, Path.Direction.CW);
        body.addRoundRect(new RectF(-12, -1, -9, 9), 1.5f, 1.5f, Path.Direction.CW);
        body.addRoundRect(new RectF(9, -1, 12, 9), 1.5f, 1.5f, Path.Direction.CW);
        body.addRoundRect(new RectF(-6, 8, -2, 15), 1.5f, 1.5f, Path.Direction.CW);
        body.addRoundRect(new RectF(2, 8, 6, 15), 1.5f, 1.5f, Path.Direction.CW);
    }

    void draw(Canvas canvas, float x, float y, float size, float rotation, int color) {
        canvas.save();
        canvas.translate(x, y);
        canvas.rotate(rotation);
        canvas.scale(size / 28f, size / 28f);
        paint.setColor(color);
        paint.setStyle(Paint.Style.FILL);
        canvas.drawPath(body, paint);
        paint.setStrokeWidth(1.4f);
        paint.setStrokeCap(Paint.Cap.ROUND);
        canvas.drawLine(-5, -9, -8, -14, paint);
        canvas.drawLine(5, -9, 8, -14, paint);
        paint.setColor((color & 0xff000000) | 0x000a1420);
        canvas.drawCircle(-3.5f, -6, 0.9f, paint);
        canvas.drawCircle(3.5f, -6, 0.9f, paint);
        canvas.restore();
    }
}
