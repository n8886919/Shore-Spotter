package tw.shorespotter.app;

import android.graphics.*;
import android.graphics.drawable.Drawable;

/** Translucent shutter circle or stop square, without an added outline. */
public final class CaptureButtonDrawable extends Drawable {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final boolean recording;
    public CaptureButtonDrawable(boolean recording) { this.recording = recording; }
    @Override public void draw(Canvas canvas) {
        Rect b = getBounds(); float cx = b.exactCenterX(), cy = b.exactCenterY();
        float unit = Math.min(b.width(), b.height()) / 72f;
        paint.setStyle(Paint.Style.FILL); paint.setColor(0x992f383d);
        canvas.drawCircle(cx,cy,35*unit,paint);
        paint.setStyle(Paint.Style.FILL); paint.setColor(0xb3f34b57);
        if (recording) canvas.drawRoundRect(cx-14*unit, cy-14*unit, cx+14*unit, cy+14*unit, 5*unit, 5*unit, paint);
        else canvas.drawCircle(cx, cy, 25 * unit, paint);
    }
    @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); }
    @Override public void setColorFilter(ColorFilter filter) { paint.setColorFilter(filter); }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
}
