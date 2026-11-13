package tw.shorespotter.app;

import android.app.*;
import android.graphics.Color;
import android.os.*;
import android.view.*;
import android.widget.*;
import org.json.*;
import java.io.IOException;
import java.util.*;
import java.util.concurrent.ExecutorService;
import java.util.function.Supplier;
import java.util.function.Consumer;

/** Field controls, deliberately separate from the Station's engineering/debug website. */
public final class StationControls {
    public interface Exchange { JSONObject request(String url, String body) throws Exception; }
    private final Activity activity;
    private final ExecutorService executor;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Exchange exchange;
    private final Supplier<String> phoneInfo;
    private final Runnable editAddress;
    private final Consumer<String> onConnected;
    private final LinearLayout panel, controlPage;
    private final TextView alert, readiness, angle, information;
    private final Button clientButton, manualButton, gpsButton, calibrateButton, speedButton;
    private final SeekBar aim;
    private volatile boolean active;
    private volatile int generation, failedGeneration = -1;
    private String endpoint = "", transientError = "", alertDetail = "";
    private long lastPollAttempt, errorUntil;
    private volatile long lastSuccess;
    private boolean reading, motionBusy, clientBusy, dragging;
    private JSONObject track = new JSONObject(), control = new JSONObject(), status = new JSONObject();
    private long pendingStationBoot = -1, pendingOperation = -1, clientDeadline;

    public StationControls(Activity activity, LinearLayout panel, TextView alert, ExecutorService executor,
                           Exchange exchange, Supplier<String> phoneInfo, Runnable editAddress,
                           Consumer<String> onConnected) {
        this.activity=activity;this.panel=panel;this.alert=alert;this.executor=executor;
        this.exchange=exchange;this.phoneInfo=phoneInfo;this.editAddress=editAddress;
        this.onConnected=onConnected;
        panel.setPadding(dp(16),dp(8),dp(16),dp(16));
        controlPage=column();
        ScrollView scroll=new ScrollView(activity);scroll.setFillViewport(true);scroll.addView(controlPage);panel.addView(scroll,new LinearLayout.LayoutParams(-1,-1));
        readiness=text("等待連線",18);readiness.setSingleLine(true);readiness.setGravity(Gravity.CENTER_VERTICAL);controlPage.addView(readiness,new LinearLayout.LayoutParams(-1,dp(44)));
        information=text("",16);information.setLines(2);information.setGravity(Gravity.CENTER_VERTICAL);controlPage.addView(information,new LinearLayout.LayoutParams(-1,dp(64)));
        clientButton=button("開啟位置回報",v->clientAction());controlPage.addView(clientButton,fullButton());
        LinearLayout modes=new LinearLayout(activity);
        manualButton=button("手動",v->motion("/api/servo/mode?mode=manual"));
        gpsButton=button("GPS 追蹤",v->motion("/api/servo/mode?mode=gps"));
        LinearLayout.LayoutParams manualCell=new LinearLayout.LayoutParams(0,dp(56),1);manualCell.rightMargin=dp(8);
        modes.addView(manualButton,manualCell);
        modes.addView(gpsButton,new LinearLayout.LayoutParams(0,dp(56),1));
        LinearLayout.LayoutParams modeRow=new LinearLayout.LayoutParams(-1,dp(56));modeRow.topMargin=dp(12);controlPage.addView(modes,modeRow);
        LinearLayout direction=new LinearLayout(activity);direction.setGravity(Gravity.CENTER_VERTICAL);
        angle=text("方向",16);direction.addView(angle,new LinearLayout.LayoutParams(dp(112),dp(56)));
        aim=new SeekBar(activity);aim.setMax(359);aim.setContentDescription("手動鏡頭方向，放開送出");
        direction.addView(aim,new LinearLayout.LayoutParams(0,dp(56),1));
        LinearLayout.LayoutParams directionRow=new LinearLayout.LayoutParams(-1,dp(56));directionRow.topMargin=dp(8);controlPage.addView(direction,directionRow);
        aim.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            public void onStartTrackingTouch(SeekBar b){dragging=true;}
            public void onProgressChanged(SeekBar b,int value,boolean user){if(user)angle.setText("方向  "+value+"°");}
            public void onStopTrackingTouch(SeekBar b){dragging=false;motion("/api/servo?angle="+(359-b.getProgress()));}
        });
        calibrateButton=button("校正鏡頭方向",v->calibrate());controlPage.addView(calibrateButton,fullButton());
        speedButton=button("轉動速度",v->speed());controlPage.addView(speedButton,fullButton());
        controlPage.addView(button("更改 Station IP",v->editAddress.run()),fullButton());
        alert.setOnClickListener(v->{if(!alertDetail.isEmpty())new AppDialogBuilder(activity).setMessage(alertDetail).setPositiveButton("知道了",null).show();});
        render();
    }
    private int dp(int value){return Math.round(value*activity.getResources().getDisplayMetrics().density);}
    private LinearLayout column(){LinearLayout v=new LinearLayout(activity);v.setOrientation(LinearLayout.VERTICAL);return v;}
    private TextView text(String value,int size){TextView t=new TextView(activity);t.setText(value);t.setTextSize(size);t.setTextColor(Color.WHITE);t.setPadding(dp(8),dp(8),dp(8),dp(8));return t;}
    private LinearLayout.LayoutParams fullButton(){LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,dp(56));p.topMargin=dp(12);return p;}
    private Button button(String value,View.OnClickListener action){Button b=new Button(activity);b.setText(value);b.setTextSize(16);b.setAllCaps(false);b.setOnClickListener(action);FieldStyle.apply(b,FieldStyle.Tone.NEUTRAL);return b;}
    public boolean isConnected(){return fresh();}
    public void start(String endpoint){
        if(!active||!this.endpoint.equals(endpoint)){generation++;lastSuccess=0;lastPollAttempt=0;reading=false;motionBusy=false;clientBusy=false;}
        this.endpoint=endpoint;active=true;tick();
    }
    public void stop(){active=false;generation++;reading=false;motionBusy=false;clientBusy=false;lastSuccess=0;render();}
    private void invalidateConnection(){generation++;lastSuccess=0;reading=false;motionBusy=false;clientBusy=false;pendingOperation=-1;}
    public void tick(){
        if(active&&lastSuccess>0&&!fresh())invalidateConnection();
        render();
        if(!active||reading||SystemClock.elapsedRealtime()-lastPollAttempt<1200)return;
        reading=true;lastPollAttempt=SystemClock.elapsedRealtime();
        final int gen=generation;final String base=endpoint;
        executor.execute(()->{
            try{
                if(!active||gen!=generation)return;
                JSONObject t=exchange.request(base+"/api/track",null);
                JSONObject c=exchange.request(base+"/api/client/control",null);
                JSONObject s=exchange.request(base+"/api/status",null);
                ui.post(()->{
                    if(!active||gen!=generation)return;
                    track=t;control=c;status=s;lastSuccess=SystemClock.elapsedRealtime();reading=false;
                    onConnected.accept(base);
                    if(clientBusy&&pendingOperation>=0){
                        if(c.optLong("station_boot_id",-1)!=pendingStationBoot||c.optLong("operation_id",-1)!=pendingOperation){clientBusy=false;showError("追蹤器操作未確認，請重新操作");}
                        else if(!"pending".equals(c.optString("command"))){
                            clientBusy=false;
                            if(!"confirmed".equals(c.optString("command")))showError("追蹤器未確認，請檢查連線");
                        }
                    }
                    render();
                });
            }catch(Exception e){
                // Revoke queued work before the executor advances, even if the UI is busy.
                failedGeneration=gen;
                ui.post(()->{if(gen==generation){invalidateConnection();render();}});
            }
        });
    }
    private JSONObject servo(){JSONObject s=track.optJSONObject("servo");return s==null?new JSONObject():s;}
    private boolean fresh(){return active&&generation!=failedGeneration&&lastSuccess>0&&SystemClock.elapsedRealtime()-lastSuccess<5000;}
    private boolean readyForMotion(){return fresh()&&!motionBusy&&servo().optBoolean("output_ready")&&!servo().optBoolean("motion_fault");}
    private void render(){
        boolean fresh=fresh();JSONObject servo=servo();boolean gps="gps".equals(servo.optString("mode"));
        String state=control.optString("state","unknown");boolean reporting=state.equals("tracking")||state.equals("test");
        boolean waiting=clientBusy||"pending".equals(control.optString("command"));
        if(clientBusy&&SystemClock.elapsedRealtime()>clientDeadline){clientBusy=false;waiting=false;showError("追蹤器操作未確認，請檢查連線");}
        clientButton.setText(waiting?"等待追蹤器確認…":reporting?"關閉位置回報":"開啟位置回報");
        clientButton.setEnabled(fresh&&!waiting&&control.optBoolean("can_control"));
        manualButton.setEnabled(readyForMotion());gpsButton.setEnabled(readyForMotion()&&servo.optBoolean("gps_available"));
        manualButton.setSelected(!gps);gpsButton.setSelected(gps);
        manualButton.setText(gps?"手動":"手動 ✓");gpsButton.setText(gps?"GPS 追蹤 ✓":"GPS 追蹤");
        aim.setEnabled(readyForMotion()&&!gps);
        if(!dragging){int target=(int)Math.round(359-servo.optDouble(gps?"angle":"target",179.5));aim.setProgress(target);angle.setText("方向  "+target+"°");}
        readiness.setText(!fresh?"正在連線…":!control.optBoolean("supported")?"等待追蹤器連線":waiting?"正在啟動／停止位置回報":!reporting?"追蹤器待機":!servo.optBoolean("gps_available")?"等待定位，可先手動拍攝":gps?"GPS 追蹤中":"定位就緒，可開始追蹤");
        calibrateButton.setEnabled(readyForMotion()&&!gps&&!servo.optBoolean("moving"));
        speedButton.setEnabled(readyForMotion());
        FieldStyle.apply(clientButton,waiting?FieldStyle.Tone.WARNING:!clientButton.isEnabled()?FieldStyle.Tone.DISABLED:reporting?FieldStyle.Tone.GOOD:FieldStyle.Tone.NEUTRAL);
        FieldStyle.apply(manualButton,!manualButton.isEnabled()?FieldStyle.Tone.DISABLED:!gps?FieldStyle.Tone.GOOD:FieldStyle.Tone.NEUTRAL);
        FieldStyle.apply(gpsButton,!gpsButton.isEnabled()?FieldStyle.Tone.DISABLED:gps?FieldStyle.Tone.GOOD:FieldStyle.Tone.NEUTRAL);
        FieldStyle.apply(calibrateButton,calibrateButton.isEnabled()?FieldStyle.Tone.NEUTRAL:FieldStyle.Tone.DISABLED);
        FieldStyle.apply(speedButton,speedButton.isEnabled()?FieldStyle.Tone.NEUTRAL:FieldStyle.Tone.DISABLED);
        FieldStyle.apply(readiness,!fresh||waiting?FieldStyle.Tone.WARNING:!reporting?FieldStyle.Tone.NEUTRAL:servo.optBoolean("gps_available")?FieldStyle.Tone.GOOD:FieldStyle.Tone.WARNING);
        {
            JSONObject station=track.optJSONObject("station");int percent=station==null?-1:station.optInt("batt_pct",-1);
            int mv=control.optInt("battery_mv",0);
            // Same 3200–4150 mV estimate as firmware geo_math.h, never a measured state of charge.
            String battery=fresh&&mv>0?Math.max(0,Math.min(100,Math.round((mv-3200)*100f/950)))+"%（估計）":"--";
            information.setText("Surfer  "+battery+"  ·  Station "+(fresh&&percent>=0?percent+"%":"--")
                    +"\n手機位置  "+phoneInfo.get());
            speedButton.setText("轉動速度  "+Math.round(servo.optDouble("speed_limit_deg_s",30))+"°/s");
        }
        renderAlerts();
    }
    private void renderAlerts(){
        if(SystemClock.elapsedRealtime()<errorUntil){alert.setText("⚠ "+transientError);alert.setVisibility(View.VISIBLE);FieldStyle.apply(alert,FieldStyle.Tone.DANGER);return;}
        if(active&&!fresh()){alertDetail="無法取得 Station 新狀態，正在自動重連；舊操作不會自動重送。";alert.setText("正在重新連線…");alert.setVisibility(View.VISIBLE);FieldStyle.apply(alert,FieldStyle.Tone.WARNING);return;}
        JSONArray entries=status.optJSONArray("alerts");List<JSONObject> relevant=new ArrayList<>();
        if(fresh()&&entries!=null)for(int i=0;i<entries.length();i++){
            JSONObject a=entries.optJSONObject(i);if(a==null)continue;
            String id=a.optString("id");
            // GNSS rate/scatter debug is irrelevant while the configured source is this fixed phone.
            if(id.equals("station_gnss_rate")||id.equals("station_scatter")||id.equals("station_sd"))continue;
            relevant.add(a);
        }
        if(relevant.isEmpty()){alert.setVisibility(View.GONE);alertDetail="";return;}
        JSONObject first=relevant.get(0);StringBuilder detail=new StringBuilder();
        for(JSONObject a:relevant){if(detail.length()>0)detail.append("\n\n");detail.append(a.optString("title")).append("\n").append(a.optString("detail"));}
        alertDetail=detail.toString();alert.setText("⚠ "+first.optString("title")+(relevant.size()>1?"  +"+(relevant.size()-1):""));
        FieldStyle.apply(alert,"error".equals(first.optString("level"))?FieldStyle.Tone.DANGER:FieldStyle.Tone.WARNING);alert.setVisibility(View.VISIBLE);
    }
    private void showError(String message){transientError=message;alertDetail=message;errorUntil=SystemClock.elapsedRealtime()+6000;renderAlerts();}
    private void motion(String path){
        if(!readyForMotion())return;
        JSONObject snapshot=servo();int gen=generation;String base=endpoint;
        ControlLease lease=new ControlLease(SystemClock.elapsedRealtime(),gen,snapshot.optLong("control_boot_id",-1),snapshot.optLong("control_epoch",-1));
        motionBusy=true;render();
        executor.execute(()->{
            String failure=null;
            try{
                if(!fresh()||!lease.valid(SystemClock.elapsedRealtime(),generation,snapshot.optLong("control_boot_id",-1),snapshot.optLong("control_epoch",-1)))throw new IOException("操作已過期，請重試");
                JSONObject t=exchange.request(base+"/api/track",null);JSONObject s=t.getJSONObject("servo");
                if(!fresh()||!lease.valid(SystemClock.elapsedRealtime(),generation,s.getLong("control_boot_id"),s.getLong("control_epoch")))throw new IOException("狀態已改變，請重新操作");
                String query=path+(path.contains("?")?"&":"?")+"epoch="+s.getLong("control_epoch")+"&seq="+ControlLease.nextSequence(s.getLong("command_seq"))+"&stamp="+s.getLong("clock_ms");
                exchange.request(base+query,"");
            }catch(Exception e){failure=e.getMessage();}
            String error=failure;ui.post(()->{if(gen!=generation)return;motionBusy=false;lastPollAttempt=0;if(error!=null)showError("操作未確認："+error);tick();});
        });
    }
    private void clientAction(){
        if(!fresh()||clientBusy||!control.optBoolean("can_control"))return;
        String state=control.optString("state");String action=state.equals("tracking")||state.equals("test")?"stop":"start";
        final int gen=generation;final String base=endpoint;final long deadline=SystemClock.elapsedRealtime()+2000;
        final long boot=control.optLong("station_boot_id",-1), clientBoot=control.optLong("boot_id",-1), id=control.optLong("client_id",-1);
        clientBusy=true;pendingOperation=-1;clientDeadline=SystemClock.elapsedRealtime()+65000;render();
        executor.execute(()->{
            try{
                if(!fresh()||gen!=generation||SystemClock.elapsedRealtime()>=deadline)throw new IOException("操作已過期");
                JSONObject c=exchange.request(base+"/api/client/control",null);
                if(!fresh()||gen!=generation||SystemClock.elapsedRealtime()>=deadline||!c.optBoolean("can_control")
                        ||c.optLong("station_boot_id",-1)!=boot||c.optLong("boot_id",-1)!=clientBoot||c.optLong("client_id",-1)!=id)throw new IOException("追蹤器狀態已變更");
                JSONObject accepted=exchange.request(base+"/api/client/control?action="+action,"");
                ui.post(()->{if(gen!=generation)return;control=accepted;pendingStationBoot=accepted.optLong("station_boot_id",-1);pendingOperation=accepted.optLong("operation_id",-1);lastPollAttempt=0;render();});
            }catch(Exception e){ui.post(()->{if(gen!=generation)return;clientBusy=false;lastPollAttempt=0;showError("操作未確認，請檢查追蹤器");});}
        });
    }
    private void calibrate(){
        EditText input=new EditText(activity);input.setHint("0–359°");input.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        new AppDialogBuilder(activity).setTitle("校正鏡頭方向")
                .setMessage("先按控制板實體置中，等鏡頭停穩，再輸入鏡頭指南針讀數。")
                .setView(input).setNegativeButton("取消",null).setPositiveButton("校正",(d,w)->{
                    try{int bearing=Integer.parseInt(input.getText().toString());if(bearing<0||bearing>359)throw new NumberFormatException();motion("/api/track/calibrate?physical_center=1&bearing="+bearing);}
                    catch(NumberFormatException e){showError("請輸入 0–359 的指南針角度");}
                }).show();
    }
    private void speed(){
        EditText input=new EditText(activity);input.setText(""+Math.round(servo().optDouble("speed_limit_deg_s",30)));input.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        new AppDialogBuilder(activity).setTitle("轉動速度（1–90°/s）").setView(input).setNegativeButton("取消",null).setPositiveButton("儲存",(d,w)->{
            try{int speed=Integer.parseInt(input.getText().toString());if(speed<1||speed>90)throw new NumberFormatException();motion("/api/servo/settings?speed="+speed);}
            catch(NumberFormatException e){showError("請輸入 1–90 的速度");}
        }).show();
    }
}
