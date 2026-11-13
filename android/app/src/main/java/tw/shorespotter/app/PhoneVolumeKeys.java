package tw.shorespotter.app;

import android.view.InputDevice;
import android.view.KeyEvent;

/** Ignore the phone clamp's side-key presses; external input devices remain distinguishable. */
final class PhoneVolumeKeys {
    static boolean shouldConsume(KeyEvent event) {
        int key=event.getKeyCode();
        if(key!=KeyEvent.KEYCODE_VOLUME_UP&&key!=KeyEvent.KEYCODE_VOLUME_DOWN&&key!=KeyEvent.KEYCODE_VOLUME_MUTE)return false;
        InputDevice source=event.getDevice();
        return source==null||!source.isExternal();
    }
    private PhoneVolumeKeys() {}
}
