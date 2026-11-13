package tw.shorespotter.app;

import android.app.AlertDialog;
import android.content.Context;

/** Dialogs own a separate window, so apply the same side-key rule there as on the preview. */
final class AppDialogBuilder extends AlertDialog.Builder {
    AppDialogBuilder(Context context) { super(context); }
    @Override public AlertDialog create() {
        AlertDialog dialog=super.create();
        dialog.setOnKeyListener((d,key,event)->PhoneVolumeKeys.shouldConsume(event));
        return dialog;
    }
    @Override public AlertDialog show() {
        AlertDialog dialog=create();dialog.show();return dialog;
    }
}
