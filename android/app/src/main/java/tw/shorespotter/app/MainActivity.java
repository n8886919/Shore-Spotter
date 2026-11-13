package tw.shorespotter.app;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ContentValues;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.MediaStore;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ImageSpan;
import android.util.Range;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.*;

import androidx.activity.ComponentActivity;
import androidx.camera.core.*;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.camera.video.*;
import androidx.camera.view.PreviewView;
import androidx.core.content.ContextCompat;

import com.google.common.util.concurrent.ListenableFuture;
import org.json.JSONObject;

import java.io.*;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.*;
import java.util.concurrent.*;

/** Foreground-only camera + local Station controls. No audio, cloud, or background service. */
public class MainActivity extends ComponentActivity {
    private static final int PERMISSIONS = 7;
    private static final long MAX_LOCATION_AGE_MS = 10_000;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService network = Executors.newSingleThreadExecutor();
    private SharedPreferences prefs;
    private PreviewView preview;
    private LinearLayout remotePanel;
    private StationControls stationControls;
    private TextView cameraStatus, positionStatus, phonePower;
    private ImageButton recordButton, pauseButton;
    private Button stationButton, positionButton, applyButton, formatButton;
    private Spinner qualitySpinner, fpsSpinner;
    private SeekBar zoom;
    private TextView zoomLabel;
    private final List<Quality> qualities = new ArrayList<>();
    private final List<String> qualityNames = new ArrayList<>();
    private final List<Integer> frameRates = new ArrayList<>();
    private ProcessCameraProvider provider;
    private Camera camera;
    private VideoCapture<Recorder> video;
    private Recording recording;
    private LocationManager locations;
    private Location latestLocation;
    private String station = "", appliedFormat = "", locationError = "";
    private volatile boolean resumed, destroyed;
    private boolean connecting, sending, verified, finalizing, paused;
    private volatile int generation;
    private long lastAckElapsed, lastAttemptElapsed, lastOfferedSampleNanos, lastConnectAttemptElapsed;
    private String lastAckStatus = "尚未傳送", recordingStatus = "準備相機…";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getSharedPreferences("shore", MODE_PRIVATE);
        locations = (LocationManager) getSystemService(LOCATION_SERVICE);
        buildUi();
        cameraStatus.setText("準備相機…");
        String saved = prefs.getString("station", "");
        if (!saved.isEmpty()) station = saved;
        requestNeededPermissions();
        initializeCamera();
    }

    private int dp(int n) { return Math.round(n * getResources().getDisplayMetrics().density); }
    private TextView label(String text, int size) {
        TextView v = new TextView(this); v.setText(text); v.setTextSize(size);
        v.setTextColor(Color.WHITE); v.setGravity(Gravity.CENTER_VERTICAL);
        v.setPadding(dp(12), dp(3), dp(12), dp(3)); return v;
    }
    private Button button(String text, View.OnClickListener click) {
        Button b = new Button(this); b.setText(text); b.setTextSize(15); b.setAllCaps(false);
        b.setMinHeight(dp(48));
        b.setMinWidth(0); b.setMinimumWidth(0); b.setPadding(dp(8), 0, dp(8), 0);
        b.setOnClickListener(click);FieldStyle.applyToolbar(b,FieldStyle.Tone.NEUTRAL);return b;
    }
    private LinearLayout.LayoutParams toolbarCell(int width) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(dp(width),dp(48));p.rightMargin=dp(8);return p;
    }
    private FrameLayout cameraRoot;
    private LinearLayout zoomRow;
    private void buildUi() {
        cameraRoot = new FrameLayout(this);
        cameraRoot.setBackgroundColor(Color.BLACK);
        preview = new PreviewView(this);
        preview.setImplementationMode(PreviewView.ImplementationMode.COMPATIBLE);
        preview.setScaleType(PreviewView.ScaleType.FILL_CENTER);
        cameraRoot.addView(preview, new FrameLayout.LayoutParams(-1, -1));
        remotePanel = new LinearLayout(this); remotePanel.setOrientation(LinearLayout.VERTICAL);
        remotePanel.setBackgroundColor(0xff10181d); remotePanel.setVisibility(View.GONE);
        FrameLayout.LayoutParams panel = new FrameLayout.LayoutParams(dp(360), -1, Gravity.RIGHT);
        panel.topMargin = dp(56); panel.bottomMargin = dp(16); panel.rightMargin=dp(16);
        cameraRoot.addView(remotePanel, panel);
        cameraRoot.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob) -> {
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) remotePanel.getLayoutParams();
            int width = Math.min(dp(380), Math.max(dp(300), (r-l)/2));
            if (lp.width != width) { lp.width = width; remotePanel.setLayoutParams(lp); }
            FrameLayout.LayoutParams zl=(FrameLayout.LayoutParams)zoomRow.getLayoutParams();
            int zoomWidth=(int)Math.round(Math.min(dp(620),Math.max(dp(240),(r-l)-dp(360)))*0.8);
            zoomWidth=Math.min(zoomWidth,Math.max(dp(200),(r-l)-dp(464)));
            if(zl.width!=zoomWidth){zl.width=zoomWidth;zoomRow.setLayoutParams(zl);}
        });
        LinearLayout toolbar = new LinearLayout(this); toolbar.setGravity(Gravity.CENTER_VERTICAL);
        toolbar.setPadding(dp(24), 0, dp(16), 0);
        formatButton = button("1080p · 30", v -> formatDialog());FieldStyle.applyOutlinedOverlay(formatButton);
        FrameLayout.LayoutParams formatLayout=new FrameLayout.LayoutParams(dp(128),dp(48),Gravity.BOTTOM|Gravity.RIGHT);
        formatLayout.rightMargin=dp(172);formatLayout.bottomMargin=dp(26);cameraRoot.addView(formatButton,formatLayout);
        phonePower=label("",13);phonePower.setGravity(Gravity.CENTER);phonePower.setLines(2);phonePower.setPadding(dp(10),0,dp(10),0);FieldStyle.applyOverlay(phonePower);
        FrameLayout.LayoutParams powerLayout=new FrameLayout.LayoutParams(dp(144),dp(48),Gravity.BOTTOM|Gravity.RIGHT);
        powerLayout.rightMargin=dp(20);powerLayout.bottomMargin=dp(26);cameraRoot.addView(phonePower,powerLayout);
        positionButton = button("定位中…", v -> positionDialog()); toolbar.addView(positionButton,toolbarCell(112));
        toolbar.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1));
        stationButton = button("Station 斷線", v -> showPanel());toolbar.addView(stationButton,new LinearLayout.LayoutParams(dp(128),dp(48)));
        cameraRoot.addView(toolbar, new FrameLayout.LayoutParams(-1, dp(48), Gravity.TOP));
        zoomRow = new LinearLayout(this); zoomRow.setGravity(Gravity.CENTER_VERTICAL);
        zoomRow.setPadding(dp(8),0,dp(12),0);FieldStyle.applyOutlinedOverlay(zoomRow);
        zoomLabel = label("1.0×", 18);zoomLabel.setTypeface(null,android.graphics.Typeface.BOLD);zoomRow.addView(zoomLabel,new LinearLayout.LayoutParams(dp(80),-1));
        zoom = new SeekBar(this); zoom.setMax(1000); zoom.setContentDescription("相機縮放倍率");
        configureZoomSlider();zoomRow.addView(zoom,new LinearLayout.LayoutParams(0,dp(48),1));
        FrameLayout.LayoutParams zoomLayout = new FrameLayout.LayoutParams(dp(448), dp(48), Gravity.BOTTOM | Gravity.LEFT);
        zoomLayout.leftMargin=dp(148);zoomLayout.bottomMargin = dp(26); cameraRoot.addView(zoomRow, zoomLayout);
        zoom.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar bar, int value, boolean user) {
                if (user && camera != null) {
                    ZoomState zs = camera.getCameraInfo().getZoomState().getValue();
                    if (zs != null) setZoomRatio((float) (zs.getMinZoomRatio() * Math.pow(Math.min(20, zs.getMaxZoomRatio()) / zs.getMinZoomRatio(), value / 1000.0)));
                }
            }
            public void onStartTrackingTouch(SeekBar bar) {}
            public void onStopTrackingTouch(SeekBar bar) {}
        });
        preview.setContentDescription("相機預覽，點按對焦");
        preview.setOnTouchListener((v, e) -> {
            if(e.getAction()==android.view.MotionEvent.ACTION_UP&&remotePanel.getVisibility()==View.VISIBLE){
                android.graphics.Rect bounds=new android.graphics.Rect();remotePanel.getHitRect(bounds);
                if(!bounds.contains((int)e.getX(),(int)e.getY())){togglePanel();updateStationButton();v.performClick();}
                return true;
            }
            if (e.getAction() == android.view.MotionEvent.ACTION_UP && camera != null) {
                MeteringPoint p = preview.getMeteringPointFactory().createPoint(e.getX(), e.getY());
                camera.getCameraControl().startFocusAndMetering(new FocusMeteringAction.Builder(p).setAutoCancelDuration(4, TimeUnit.SECONDS).build());
                v.performClick();
            }
            return true;
        });
        qualitySpinner = new Spinner(this); fpsSpinner = new Spinner(this);
        applyButton = button("套用", v -> bindCamera());
        cameraStatus = label("", 16);FieldStyle.apply(cameraStatus,FieldStyle.Tone.NEUTRAL);
        FrameLayout.LayoutParams statusLayout = new FrameLayout.LayoutParams(-2, dp(40), Gravity.BOTTOM | Gravity.LEFT);
        statusLayout.leftMargin = dp(20); statusLayout.bottomMargin = dp(86);
        cameraRoot.addView(cameraStatus, statusLayout);
        recordButton = new ImageButton(this); recordButton.setImageDrawable(new CaptureButtonDrawable(false));
        recordButton.setContentDescription("開始錄影"); recordButton.setEnabled(false);
        recordButton.setPadding(0, 0, 0, 0); recordButton.setBackgroundColor(Color.TRANSPARENT);
        recordButton.setOnClickListener(v -> { if (recording != null) stopRecording(); else startRecording(); });
        FrameLayout.LayoutParams recordLayout = new FrameLayout.LayoutParams(dp(48), dp(48), Gravity.BOTTOM | Gravity.LEFT);
        recordLayout.leftMargin = dp(20); recordLayout.bottomMargin = dp(26);
        cameraRoot.addView(recordButton, recordLayout);
        pauseButton = new ImageButton(this); pauseButton.setImageResource(tw.shorespotter.app.R.drawable.ic_pause);
        pauseButton.setContentDescription("暫停錄影"); pauseButton.setPadding(dp(12), dp(12), dp(12), dp(12));
        pauseButton.setVisibility(View.GONE);
        pauseButton.setOnClickListener(v -> {
            if (recording == null || finalizing) return;
            pauseButton.setEnabled(false);
            if (paused) recording.resume(); else recording.pause();
        });
        FieldStyle.applyOverlay(pauseButton);
        FrameLayout.LayoutParams pauseLayout = new FrameLayout.LayoutParams(dp(48), dp(48), Gravity.BOTTOM | Gravity.LEFT);
        pauseLayout.leftMargin = dp(84); pauseLayout.bottomMargin = dp(26); cameraRoot.addView(pauseButton, pauseLayout);
        positionStatus = label("", 12);
        TextView alertOverlay=label("",16);alertOverlay.setVisibility(View.GONE);
        FrameLayout.LayoutParams alertsLayout=new FrameLayout.LayoutParams(-2,dp(48),Gravity.TOP|Gravity.LEFT);
        alertsLayout.leftMargin=dp(24);alertsLayout.topMargin=dp(64);
        cameraRoot.addView(alertOverlay,alertsLayout);
        stationControls=new StationControls(this,remotePanel,alertOverlay,network,this::request,this::phonePositionSummary,this::addressDialog);
        WindowManager.LayoutParams windowLayout=getWindow().getAttributes();
        windowLayout.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(windowLayout);
        setContentView(cameraRoot);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }
    private void togglePanel() { remotePanel.setVisibility(View.GONE); zoomRow.setVisibility(View.VISIBLE);formatButton.setVisibility(View.VISIBLE);phonePower.setVisibility(View.VISIBLE); }
    private void configureZoomSlider() {
        android.graphics.drawable.GradientDrawable rail=new android.graphics.drawable.GradientDrawable();
        rail.setColor(0xff52657a);rail.setCornerRadius(dp(4));
        android.graphics.drawable.GradientDrawable fill=new android.graphics.drawable.GradientDrawable();
        fill.setColor(Color.WHITE);fill.setCornerRadius(dp(4));
        android.graphics.drawable.LayerDrawable track=new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[]{
                rail,new android.graphics.drawable.ClipDrawable(fill,Gravity.LEFT,android.graphics.drawable.ClipDrawable.HORIZONTAL)});
        track.setId(0,android.R.id.background);track.setId(1,android.R.id.progress);
        for(int i=0;i<2;i++){track.setLayerHeight(i,dp(8));track.setLayerGravity(i,Gravity.CENTER_VERTICAL|Gravity.FILL_HORIZONTAL);}
        zoom.setProgressTintList(null);zoom.setProgressBackgroundTintList(null);zoom.setThumbTintList(null);
        zoom.setProgressDrawable(track);zoom.setSplitTrack(false);zoom.setPadding(dp(16),0,dp(16),0);
        android.graphics.drawable.GradientDrawable thumb=new android.graphics.drawable.GradientDrawable();
        thumb.setShape(android.graphics.drawable.GradientDrawable.OVAL);thumb.setSize(dp(28),dp(28));
        thumb.setColor(0xffdce3df);thumb.setStroke(dp(2),Color.BLACK);zoom.setThumb(thumb);zoom.setThumbOffset(dp(14));
    }
    private void showPanel() {
        if(!stationControls.isConnected()){addressDialog();return;}
        if(remotePanel.getVisibility()==View.VISIBLE)togglePanel();
        else {remotePanel.setVisibility(View.VISIBLE);zoomRow.setVisibility(View.GONE);formatButton.setVisibility(View.GONE);phonePower.setVisibility(View.GONE);}
        updateStationButton();
    }
    private void updateStationButton() {
        boolean connected=stationControls.isConnected();
        if(!connected&&remotePanel.getVisibility()==View.VISIBLE)togglePanel();
        stationButton.setText(connected?"Station 控制":"Station 斷線");
        FieldStyle.applyToolbar(stationButton,connected?FieldStyle.Tone.GOOD:FieldStyle.Tone.DANGER);
        stationButton.setContentDescription(connected?(remotePanel.getVisibility()==View.VISIBLE?"Station 控制，關閉面板":"Station 控制，開啟控制與資訊"):"Station 斷線，設定 IP");
    }

    private void requestNeededPermissions() {
        List<String> required = new ArrayList<>();
        for (String p : new String[]{Manifest.permission.CAMERA, Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION}) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) required.add(p);
        }
        if (!required.isEmpty()) requestPermissions(required.toArray(new String[0]), PERMISSIONS);
    }
    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(request, permissions, grants);
        if (request == PERMISSIONS) { initializeCamera(); startLocations(); }
    }

    private void initializeCamera() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            recordingStatus = "需要相機權限"; updateCameraStatus(); return;
        }
        if (provider != null) { if (video == null) bindCamera(); return; }
        ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(this);
        future.addListener(() -> {
            if (destroyed) return;
            try {
                provider = future.get();
                CameraInfo info = CameraSelector.DEFAULT_BACK_CAMERA.filter(provider.getAvailableCameraInfos()).get(0);
                qualities.clear(); qualityNames.clear();
                for (Quality q : QualitySelector.getSupportedQualities(info)) {
                    qualities.add(q); qualityNames.add(qualityName(q));
                }
                qualitySpinner.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, qualityNames));
                int preferred = qualityNames.indexOf(prefs.getString("quality", "1080p"));
                qualitySpinner.setSelection(Math.max(0, preferred));
                frameRates.clear();
                for (int fps : new int[]{24, 30, 60}) {
                    for (Range<Integer> range : info.getSupportedFrameRateRanges()) {
                        if (range.contains(fps)) { frameRates.add(fps); break; }
                    }
                }
                if (frameRates.isEmpty()) frameRates.add(30);
                List<String> names = new ArrayList<>(); for (int f : frameRates) names.add(f + " fps");
                fpsSpinner.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, names));
                int index = frameRates.indexOf(prefs.getInt("fps", 30));
                fpsSpinner.setSelection(Math.max(0, index));
                bindCamera();
            } catch (Exception e) { recordingStatus = "相機初始化失敗：" + e.getClass().getSimpleName(); updateCameraStatus(); }
        }, ContextCompat.getMainExecutor(this));
    }
    private String qualityName(Quality q) {
        if (q == Quality.UHD) return "4K";
        if (q == Quality.FHD) return "1080p";
        if (q == Quality.HD) return "720p";
        return "480p";
    }
    private void bindCamera() {
        if (provider == null || qualities.isEmpty() || recording != null || finalizing) return;
        int qi = qualitySpinner.getSelectedItemPosition(), fi = fpsSpinner.getSelectedItemPosition();
        if (qi < 0 || fi < 0) return;
        Quality quality = qualities.get(qi); int fps = frameRates.get(fi);
        try {
            if (camera != null) {
                camera.getCameraInfo().getZoomState().removeObservers(this);
                camera.getCameraInfo().getCameraState().removeObservers(this);
            }
            provider.unbindAll();
            Preview pv = new Preview.Builder().build(); pv.setSurfaceProvider(preview.getSurfaceProvider());
            Recorder recorder = new Recorder.Builder().setQualitySelector(QualitySelector.from(quality)).build();
            video = new VideoCapture.Builder<>(recorder).setTargetFrameRate(new Range<>(fps, fps)).build();
            video.setTargetRotation(getWindowManager().getDefaultDisplay().getRotation());
            camera = provider.bindToLifecycle(this, CameraSelector.DEFAULT_BACK_CAMERA, pv, video);
            camera.getCameraInfo().getZoomState().observe(this, zs -> {
                if (zs == null) return;
                zoomLabel.setText(String.format(Locale.US, "%.1f×", zs.getZoomRatio()));
                double max = Math.min(20, zs.getMaxZoomRatio());
                if (max > zs.getMinZoomRatio()) zoom.setProgress((int) Math.round(1000 * Math.log(zs.getZoomRatio() / zs.getMinZoomRatio()) / Math.log(max / zs.getMinZoomRatio())));
            });
            camera.getCameraInfo().getCameraState().observe(this, s -> {
                if (s.getError() != null) { recordingStatus = "相機錯誤 " + s.getError().getCode() + "；請選其他畫質並套用"; recordButton.setEnabled(false); updateCameraStatus(); }
            });
            appliedFormat = qualityName(quality) + " / " + fps + " fps 目標";
            recordingStatus = "待錄影 · 無聲"; recordButton.setEnabled(true);
            prefs.edit().putString("quality", qualityName(quality)).putInt("fps", fps).apply();
            updateCameraStatus();
        } catch (Exception e) {
            video = null; recordingStatus = "此畫質組合無法開啟，請更換後套用";
            recordButton.setEnabled(false); updateCameraStatus();
        }
    }
    private void setZoomRatio(float ratio) {
        if (camera == null) return;
        ZoomState zs = camera.getCameraInfo().getZoomState().getValue();
        if (zs == null || ratio < zs.getMinZoomRatio() || ratio > zs.getMaxZoomRatio()) { toast("此鏡頭不支援這個倍率"); return; }
        camera.getCameraControl().setZoomRatio(ratio);
    }
    private void startRecording() {
        if (video == null || finalizing || !resumed) return;
        ContentValues values = new ContentValues();
        String name = "ShoreSpotter_" + new SimpleDateFormat("yyyyMMdd_HHmmss_SSS", Locale.US).format(new Date());
        values.put(MediaStore.Video.Media.DISPLAY_NAME, name);
        values.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
        values.put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/ShoreSpotter");
        MediaStoreOutputOptions output = new MediaStoreOutputOptions.Builder(getContentResolver(), MediaStore.Video.Media.EXTERNAL_CONTENT_URI).setContentValues(values).build();
        try {
            recording = video.getOutput().prepareRecording(this, output).start(ContextCompat.getMainExecutor(this), event -> {
                if (event instanceof VideoRecordEvent.Start) {
                    paused = false; recordingStatus = "錄影中";
                    recordButton.setImageDrawable(new CaptureButtonDrawable(true)); recordButton.setContentDescription("停止錄影");
                    pauseButton.setImageResource(R.drawable.ic_pause); pauseButton.setContentDescription("暫停錄影"); pauseButton.setVisibility(View.VISIBLE); pauseButton.setEnabled(true);
                }
                else if (event instanceof VideoRecordEvent.Status) {
                    long seconds = event.getRecordingStats().getRecordedDurationNanos() / 1_000_000_000;
                    recordingStatus = String.format(Locale.US, "%s %02d:%02d", paused ? "暫停" : "錄影", seconds / 60, seconds % 60);
                } else if (event instanceof VideoRecordEvent.Pause) {
                    paused = true; recordingStatus = "暫停"; pauseButton.setImageResource(R.drawable.ic_play);
                    pauseButton.setContentDescription("繼續錄影"); pauseButton.setEnabled(true);
                } else if (event instanceof VideoRecordEvent.Resume) {
                    paused = false; recordingStatus = "錄影中"; pauseButton.setImageResource(R.drawable.ic_pause);
                    pauseButton.setContentDescription("暫停錄影"); pauseButton.setEnabled(true);
                } else if (event instanceof VideoRecordEvent.Finalize) {
                    VideoRecordEvent.Finalize done = (VideoRecordEvent.Finalize) event;
                    recording = null; finalizing = false; paused = false;
                    recordButton.setImageDrawable(new CaptureButtonDrawable(false)); recordButton.setContentDescription("開始錄影"); recordButton.setEnabled(resumed && video != null);
                    pauseButton.setVisibility(View.GONE); pauseButton.setEnabled(true);
                    setFormatEnabled(true);
                    if (done.hasError()) {
                        recordingStatus = "錄影錯誤 " + done.getError() + "，請檢查已保存的片段";
                        toast(recordingStatus);
                    } else {
                        recordingStatus = "已儲存";
                        main.postDelayed(()->{if(!destroyed&&recording==null&&"已儲存".equals(recordingStatus))cameraStatus.setVisibility(View.GONE);},2500);
                    }
                }
                updateCameraStatus();
            });
            setFormatEnabled(false);
        } catch (Exception e) { recording = null; recordingStatus = "無法開始錄影：" + e.getClass().getSimpleName(); setFormatEnabled(true); }
        updateCameraStatus();
    }
    private void stopRecording() {
        if (recording == null || finalizing) return;
        finalizing = true; recordButton.setEnabled(false); pauseButton.setEnabled(false); recordingStatus = "儲存中…";
        recording.stop(); updateCameraStatus();
    }
    private void setFormatEnabled(boolean enabled) {
        qualitySpinner.setEnabled(enabled); fpsSpinner.setEnabled(enabled); applyButton.setEnabled(enabled);
        formatButton.setEnabled(enabled);
    }
    private void updateCameraStatus() {
        if (cameraStatus == null) return;
        cameraStatus.setText(recordingStatus.replace(" · 無聲", ""));
        FieldStyle.apply(cameraStatus,recordingStatus.contains("錯誤")||recordingStatus.startsWith("無法")?FieldStyle.Tone.DANGER
                :finalizing||paused?FieldStyle.Tone.WARNING:recording!=null?FieldStyle.Tone.DANGER:FieldStyle.Tone.NEUTRAL);
        FieldStyle.applyOverlay(pauseButton);pauseButton.setImageAlpha(pauseButton.isEnabled()?255:128);
        FieldStyle.applyOutlinedOverlay(formatButton);formatButton.setTextColor(formatButton.isEnabled()?0xfff7f8f6:0xffc7cdce);
        cameraStatus.setVisibility(recordingStatus.startsWith("待錄影") ? View.GONE : View.VISIBLE);
        if (!qualities.isEmpty() && qualitySpinner.getSelectedItemPosition() >= 0 && fpsSpinner.getSelectedItemPosition() >= 0)
            formatButton.setText(qualityNames.get(qualitySpinner.getSelectedItemPosition()) + " · " + frameRates.get(fpsSpinner.getSelectedItemPosition()));
    }

    private void formatDialog() {
        if (recording != null || finalizing) return;
        LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(dp(24), dp(8), dp(24), dp(8));
        if (qualitySpinner.getParent() != null) ((ViewGroup) qualitySpinner.getParent()).removeView(qualitySpinner);
        if (fpsSpinner.getParent() != null) ((ViewGroup) fpsSpinner.getParent()).removeView(fpsSpinner);
        box.addView(qualitySpinner, new LinearLayout.LayoutParams(-1, dp(56)));
        box.addView(fpsSpinner, new LinearLayout.LayoutParams(-1, dp(56)));
        int qi = qualitySpinner.getSelectedItemPosition(), fi = fpsSpinner.getSelectedItemPosition();
        AlertDialog dialog = new AppDialogBuilder(this).setTitle("錄影畫質").setView(box)
                .setNegativeButton("取消", (d,w) -> { qualitySpinner.setSelection(qi); fpsSpinner.setSelection(fi); })
                .setPositiveButton("套用", (d,w) -> bindCamera()).create();
        dialog.setOnCancelListener(d -> { qualitySpinner.setSelection(qi); fpsSpinner.setSelection(fi); });
        dialog.show();
    }
    private void positionDialog() {
        updatePositionStatus();
        new AppDialogBuilder(this).setTitle("Station 位置").setMessage(positionStatus.getText())
                .setPositiveButton("完成", null).show();
    }

    private void addressDialog() {
        EditText input = new EditText(this); input.setSingleLine(true);
        input.setHint("192.168.1.100"); input.setText(station.replaceFirst("^http://", ""));
        // IPv4 needs several dots; a normal decimal-number filter permits only one.
        input.setKeyListener(android.text.method.DigitsKeyListener.getInstance("0123456789.:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-"));
        input.setRawInputType(android.text.InputType.TYPE_CLASS_NUMBER | android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL);
        input.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE | android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        input.setSelection(input.length());
        AlertDialog dialog = new AppDialogBuilder(this).setTitle("Station IP")
                .setView(input).setNegativeButton("取消", null).setPositiveButton("連線", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            try {
                String next = StationAddress.normalize(input.getText().toString());
                generation++; station = next; verified = false; connecting = false; latestLocation = null;
                lastAckElapsed = 0; lastOfferedSampleNanos = 0; lastAckStatus = "尚未傳送"; stationControls.stop();
                prefs.edit().putString("station", station).apply();
                dialog.dismiss(); connectStation();
            } catch (IllegalArgumentException e) { input.setError(e.getMessage()); }
        }));
        input.setOnEditorActionListener((v, action, event) -> {
            if(action!=android.view.inputmethod.EditorInfo.IME_ACTION_DONE)return false;
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();return true;
        });
        dialog.show();
    }
    private void connectStation() {
        if (station.isEmpty() || connecting || destroyed) return;
        final String target = station; final int gen = generation; connecting = true;
        lastConnectAttemptElapsed = SystemClock.elapsedRealtime();
        network.execute(() -> {
            String error = null;
            try {
                if (!resumed || gen != generation) throw new IOException("已取消連線");
                JSONObject p = request(target + "/api/station/position", null);
                if (!p.has("source") || !p.has("retention") || !p.has("valid")) throw new IOException("不是支援位置 API 的 Station");
                JSONObject status = request(target + "/api/track", null);
                // No movement commands are issued during identification.
                main.post(() -> {
                    if (destroyed || gen != generation || !resumed) return;
                    verified = true;
                    stationControls.start(target);
                    startLocations();
                });
            } catch (Exception e) { error = e.getMessage(); }
            final String failure = error;
            main.post(() -> {
                if (destroyed || gen != generation) return;
                connecting = false;
                if (failure != null) { verified = false; lastAckStatus = "未連上 Station：" + failure; updatePositionStatus(); }
            });
        });
    }

    private final LocationListener locationListener = new LocationListener() {
        @Override public void onLocationChanged(Location p) {
            if (!LocationManager.GPS_PROVIDER.equals(p.getProvider())) return;
            if (p.hasAccuracy() && p.getAccuracy() > 0 && p.getAccuracy() <= 100000
                    && Math.abs(p.getLatitude()) <= 90 && Math.abs(p.getLongitude()) <= 180
                    && locationAge(p) <= MAX_LOCATION_AGE_MS
                    && (latestLocation == null || p.getElapsedRealtimeNanos() > latestLocation.getElapsedRealtimeNanos())) {
                latestLocation = new Location(p); locationError = "";
            }
            maybeSendPosition(); updatePositionStatus();
        }
        @Override public void onProviderEnabled(String name) {
            if (!LocationManager.GPS_PROVIDER.equals(name)) return;
            locationError = "等待 GPS 定位"; updatePositionStatus();
        }
        @Override public void onProviderDisabled(String name) {
            if (!LocationManager.GPS_PROVIDER.equals(name)) return;
            latestLocation = null; locationError = "請開啟手機 GPS 定位"; updatePositionStatus();
        }
    };
    private long locationAge(Location p) { return Math.max(0, (SystemClock.elapsedRealtimeNanos() - p.getElapsedRealtimeNanos()) / 1_000_000); }
    private void startLocations() {
        if (!resumed) return;
        boolean fine = checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
        if (!fine) { locationError = "需要精確位置權限，概略位置不作為 Station 座標"; updatePositionStatus(); return; }
        stopLocations();
        try {
            if (locations.getAllProviders().contains(LocationManager.GPS_PROVIDER)) {
                // Keep the GPS subscription while disabled so re-enabling can recover in foreground.
                locations.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, locationListener, Looper.getMainLooper());
                locationError = locations.isProviderEnabled(LocationManager.GPS_PROVIDER)
                        ? "等待 GPS 定位" : "請開啟手機 GPS 定位";
            } else {
                locationError = "這台手機不支援 GPS 定位";
            }
        } catch (SecurityException e) { locationError = "定位權限不足"; }
        updatePositionStatus();
    }
    private void stopLocations() { if (locations != null) locations.removeUpdates(locationListener); }
    private void maybeSendPosition() {
        long now = SystemClock.elapsedRealtime();
        if (!resumed || !verified || sending || latestLocation == null
                || !LocationSamplePolicy.shouldSend(latestLocation.getProvider(), latestLocation.getElapsedRealtimeNanos(), lastOfferedSampleNanos, SystemClock.elapsedRealtimeNanos())
                || now - lastAttemptElapsed < 1000) return;
        final Location sample = new Location(latestLocation);
        final String target = station; final int gen = generation;
        sending = true; lastAttemptElapsed = now; lastOfferedSampleNanos = sample.getElapsedRealtimeNanos();
        network.execute(() -> {
            String outcome;
            boolean ok = false;
            try {
                if (destroyed || !resumed || gen != generation || locationAge(sample) > MAX_LOCATION_AGE_MS) throw new IOException("定位已停止或過期");
                String body = "action=phone&lat=" + encode(Double.toString(sample.getLatitude()))
                        + "&lon=" + encode(Double.toString(sample.getLongitude()))
                        + "&accuracy=" + encode(Float.toString(sample.getAccuracy()))
                        + "&timestamp=" + sample.getTime();
                JSONObject result = request(target + "/api/station/position", body);
                if (!"phone".equals(result.optString("source")) || !result.optBoolean("valid")
                        || Math.abs(result.optDouble("lat", 999) - sample.getLatitude()) > 0.000001
                        || Math.abs(result.optDouble("lon", 999) - sample.getLongitude()) > 0.000001
                        || Math.abs(result.optDouble("updated_utc_ms", 0) - sample.getTime()) > 1) {
                    throw new IOException("Station 未回覆相同手機快照");
                }
                ok = true; outcome = "Station 已確認";
            } catch (Exception e) { outcome = "未確認：" + e.getMessage(); }
            final boolean success = ok; final String state = outcome;
            main.post(() -> {
                sending = false;
                if (destroyed || gen != generation || !resumed) return;
                lastAckStatus = state;
                if (success) lastAckElapsed = SystemClock.elapsedRealtime();
                updatePositionStatus();
            });
        });
    }
    private void updatePositionStatus() {
        if (positionStatus == null) return;
        String detail;
        if (latestLocation == null) detail = locationError.isEmpty() ? "等待新定位" : locationError;
        else detail = String.format(Locale.US, "手機 ±%.0f m · 定位 %.0f 秒前", latestLocation.getAccuracy(), locationAge(latestLocation) / 1000.0);
        String ackAge = lastAckElapsed == 0 ? "" : " · 確認 " + ((SystemClock.elapsedRealtime() - lastAckElapsed) / 1000) + " 秒前";
        positionStatus.setText(detail + "  |  " + (station.isEmpty() ? "請設定 Station IP" : lastAckStatus) + ackAge);
        boolean fresh = latestLocation != null && locationAge(latestLocation) <= MAX_LOCATION_AGE_MS && lastAckElapsed > 0 && SystemClock.elapsedRealtime() - lastAckElapsed < 10_000;
        positionButton.setText(fresh ? "定位 ✓" : latestLocation == null ? "定位中…" : "定位未確認");
        FieldStyle.applyToolbar(positionButton,fresh?FieldStyle.Tone.GOOD:latestLocation==null?FieldStyle.Tone.WARNING:FieldStyle.Tone.DANGER);
    }
    private static String encode(String s) {
        try { return URLEncoder.encode(s, "UTF-8"); }
        catch (UnsupportedEncodingException impossible) { throw new AssertionError(impossible); }
    }
    private JSONObject request(String address, String body) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(address).openConnection();
        c.setInstanceFollowRedirects(false); c.setConnectTimeout(2000); c.setReadTimeout(2000);
        c.setRequestProperty("Accept", "application/json"); c.setUseCaches(false);
        try {
            if (body != null) {
                byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                c.setRequestMethod("POST"); c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
                c.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream out = c.getOutputStream()) { out.write(bytes); }
            }
            int code = c.getResponseCode();
            if (code != 200 && code != 202) {
                String message="HTTP "+code;
                try(InputStream err=c.getErrorStream()) {
                    if(err!=null){byte[] data=new byte[512];int n=err.read(data);if(n>0)message=new JSONObject(new String(data,0,n,StandardCharsets.UTF_8)).optString("error",message);}
                } catch(Exception ignored) {}
                throw new IOException(message);
            }
            try (InputStream in = c.getInputStream(); ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[2048]; int count;
                while ((count = in.read(buffer)) != -1) {
                    bytes.write(buffer, 0, count);
                    if (bytes.size() > 65536) throw new IOException("回覆過大");
                }
                return new JSONObject(bytes.toString(StandardCharsets.UTF_8.name()));
            }
        } finally { c.disconnect(); }
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (!resumed || destroyed) return;
            if (!verified && !connecting && !station.isEmpty() && SystemClock.elapsedRealtime() - lastConnectAttemptElapsed >= 5000) connectStation();
            maybeSendPosition(); updatePositionStatus();updatePhoneResources();stationControls.tick();updateStationButton();main.postDelayed(this, 1000);
        }
    };
    @Override protected void onResume() {
        super.onResume(); resumed = true;
        startLocations(); main.removeCallbacks(tick); main.post(tick);
        if (!station.isEmpty() && !verified) connectStation();
    }
    @Override protected void onPause() {
        resumed = false; generation++; verified = false; connecting = false;
        main.removeCallbacks(tick); stopLocations(); latestLocation = null; stopRecording(); stationControls.stop();togglePanel();
        super.onPause();
    }
    @Override protected void onDestroy() {
        destroyed = true; main.removeCallbacksAndMessages(null); stopLocations();
        if (recording != null) recording.close();
        if (provider != null) provider.unbindAll();
        stationControls.stop(); network.shutdownNow(); super.onDestroy();
    }
    @Override public void onBackPressed() {
        if (recording != null || finalizing) {
            new AppDialogBuilder(this).setMessage("離開會停止並保存錄影，確認離開？")
                    .setNegativeButton("繼續錄影", null).setPositiveButton("離開", (d, w) -> finish()).show();
        } else if (remotePanel.getVisibility() == View.VISIBLE) togglePanel(); else super.onBackPressed();
    }
    @Override public boolean onKeyDown(int keyCode,KeyEvent event) { return PhoneVolumeKeys.shouldConsume(event)||super.onKeyDown(keyCode,event); }
    @Override public boolean onKeyUp(int keyCode,KeyEvent event) { return PhoneVolumeKeys.shouldConsume(event)||super.onKeyUp(keyCode,event); }
    private long resourceReadAt, availableStorageBytes;
    private String phonePositionSummary() {
        if(latestLocation==null)return "等待定位";
        if(locationAge(latestLocation)>MAX_LOCATION_AGE_MS)return "等待新位置";
        return "±"+Math.round(latestLocation.getAccuracy())+" m · "+(lastAckElapsed>0&&SystemClock.elapsedRealtime()-lastAckElapsed<10000?"已同步":"尚未同步");
    }
    private void updatePhoneResources() {
        Intent battery=registerReceiver(null,new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int level=battery==null?-1:battery.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL,-1);
        int scale=battery==null?100:battery.getIntExtra(android.os.BatteryManager.EXTRA_SCALE,100);
        int percentage=level<0?-1:Math.round(level*100f/Math.max(1,scale));
        boolean plugged=battery!=null&&battery.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED,0)!=0;
        String batteryText=percentage<0?"--":percentage+"%";
        SpannableStringBuilder power=new SpannableStringBuilder(plugged?"\uFFFC ":"電量 ").append(batteryText);
        if(plugged){
            Drawable bolt=ContextCompat.getDrawable(this,R.drawable.ic_bolt);
            int size=Math.round(16*getResources().getDisplayMetrics().scaledDensity);
            bolt.setBounds(0,0,size,size);
            power.setSpan(new ImageSpan(bolt,ImageSpan.ALIGN_CENTER),0,1,Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        if(resourceReadAt==0||SystemClock.elapsedRealtime()-resourceReadAt>=10000){
            resourceReadAt=SystemClock.elapsedRealtime();
            java.io.File volume=getExternalFilesDir(null);
            android.os.StatFs disk=new android.os.StatFs((volume==null?getFilesDir():volume).getAbsolutePath());
            availableStorageBytes=disk.getAvailableBytes();
        }
        String storage=String.format(Locale.US,"剩餘 %.1f GB",availableStorageBytes/1_000_000_000.0);
        phonePower.setText(power.append('\n').append(storage));
        phonePower.setContentDescription((plugged?"充電中 ":"電量 ")+batteryText+"，"+storage);
    }
    private void toast(String text) { Toast.makeText(this, text, Toast.LENGTH_LONG).show(); }
}
