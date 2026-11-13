package tw.shorespotter.app;

import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.InsetDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.View;
import android.widget.ImageView;
import android.widget.TextView;
import java.util.WeakHashMap;

/** Muted solid status surfaces, with translucent backplates reserved for camera tools. */
final class FieldStyle {
    enum Tone {
        NEUTRAL(0xff2f383d, 0xfff7f8f6),
        GOOD(0xff7acd9c, 0xff20372c),
        WARNING(0xffe5dcca, 0xff443824),
        DANGER(0xffe99a94, 0xff542f2a),
        DISABLED(0xffc7cdce, 0xff424d50);
        final int fill, ink;
        Tone(int fill, int ink) { this.fill=fill; this.ink=ink; }
    }
    private static final WeakHashMap<View,Integer> applied=new WeakHashMap<>();

    static void apply(View view,Tone tone) { apply(view,tone,0); }
    static void applyOverlay(View view) { apply(view,Tone.NEUTRAL,1); }
    static void applyToolbar(View view,Tone tone) { apply(view,tone,2); }
    static void applyOutlinedOverlay(View view) { apply(view,Tone.NEUTRAL,3); }
    private static void apply(View view,Tone tone,int mode) {
        int token=tone.ordinal()*4+mode;
        Integer previous=applied.get(view);if(previous!=null&&previous==token)return;
        float density=view.getResources().getDisplayMetrics().density;
        GradientDrawable surface=new GradientDrawable();
        surface.setColor(mode==1||mode==3?(tone.fill&0x00ffffff)|0xb3000000:tone.fill);surface.setCornerRadius(6*density);
        if(mode==3)surface.setStroke(Math.max(1,Math.round(density)),0xffffffff);
        android.graphics.drawable.Drawable background=mode==2?new InsetDrawable(surface,0,Math.round(8*density),0,Math.round(8*density)):surface;
        int left=view.getPaddingLeft(),top=view.getPaddingTop(),right=view.getPaddingRight(),bottom=view.getPaddingBottom();
        view.setBackgroundTintList(null);
        view.setBackground(view.isClickable()?new RippleDrawable(ColorStateList.valueOf(0x40ffffff),background,null):background);
        view.setPadding(left,top,right,bottom);
        view.setAlpha(1f);
        if(view instanceof TextView){
            TextView label=(TextView)view;label.setTextColor(tone.ink);label.setTypeface(null,Typeface.BOLD);
        }
        if(view instanceof ImageView)((ImageView)view).setImageTintList(ColorStateList.valueOf(tone.ink));
        applied.put(view,token);
    }
    private FieldStyle() {}
}
