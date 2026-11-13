#!/usr/bin/env python3
"""Host regressions for actual Android lifecycle/control methods, without a phone.

Extract production Java methods into small Android/UI/network fakes. This checks
callback ordering and queued actions, not CameraX/device behavior. JDK 17+ is
required; no Gradle, Android SDK, downloads, or network requests are involved.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'android/app/src/main/java/tw/shorespotter/app'


def java_tool(name):
    candidates = []
    if os.environ.get('JAVA_HOME'):
        candidates.append(Path(os.environ['JAVA_HOME']) / 'bin' / name)
    found = shutil.which(name)
    if found:
        candidates.append(Path(found))
    candidates.append(Path('/opt/android-studio/jbr/bin') / name)
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
    raise RuntimeError(f'Missing {name}: install JDK 17+ or set JAVA_HOME')


def block(source, marker):
    start = source.index(marker)
    body = source.index('{', start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, source[body:]):
        if match.group() == '{':
            depth += 1
        elif match.group() == '}':
            depth -= 1
            if depth == 0:
                return source[start:body + match.end()]
    raise ValueError(f'Unclosed Java block: {marker}')


def controls_source():
    source = (JAVA / 'StationControls.java').read_text()
    markers = ['public void tick()', 'private JSONObject servo()',
               'private boolean fresh()', 'private boolean readyForMotion()',
               'private void motion(String path)', 'private void clientAction()']
    # Optional only to allow the pre-fix source to compile and fail behaviorally.
    if 'private void invalidateConnection()' in source:
        markers.append('private void invalidateConnection()')
    methods = '\n'.join(block(source, marker) for marker in markers)
    lease = (JAVA / 'ControlLease.java').read_text().replace(
        'package tw.shorespotter.app;', '').replace('public final class', 'final class')
    remember = block((JAVA / 'MainActivity.java').read_text(),
                     'private void rememberStation(String endpoint)').replace('private void', 'void', 1)
    return r'''
import java.util.*;
import java.util.concurrent.*;
import java.io.IOException;
class SystemClock {
    static long now = 10000;
    static long elapsedRealtime() { return now; }
}
class JSONObject extends HashMap<String,Object> {
    JSONObject with(String k,Object v) { put(k,v);return this; }
    JSONObject optJSONObject(String k) { return (JSONObject)get(k); }
    JSONObject getJSONObject(String k) { return optJSONObject(k); }
    long optLong(String k,long d) { Object v=get(k);return v instanceof Number?((Number)v).longValue():d; }
    long getLong(String k) { return optLong(k,-1); }
    boolean optBoolean(String k) { return Boolean.TRUE.equals(get(k)); }
    String optString(String k) { return Objects.toString(get(k), ""); }
}
class QueueExecutor extends AbstractExecutorService {
    ArrayDeque<Runnable> jobs=new ArrayDeque<>();
    public void execute(Runnable task) { jobs.add(task); }
    void runOne() { jobs.remove().run(); }
    public void shutdown() {}
    public List<Runnable> shutdownNow() { return List.of(); }
    public boolean isShutdown() { return false; }
    public boolean isTerminated() { return false; }
    public boolean awaitTermination(long time,TimeUnit unit) { return false; }
}
class Ui {
    ArrayDeque<Runnable> jobs=new ArrayDeque<>();
    void post(Runnable task) { jobs.add(task); }
    void drain() { while(!jobs.isEmpty())jobs.remove().run(); }
}
class Prefs {
    Map<String,String> values=new HashMap<>();int writes;
    String getString(String key,String fallback) { return values.getOrDefault(key,fallback); }
    Prefs edit() { return this; }
    Prefs putString(String key,String value) { values.put(key,value);return this; }
    void apply() { writes++; }
}
class History {
    boolean destroyed,resumed=true,verified=true;
    String station="http://station.invalid";
    Prefs prefs=new Prefs();
''' + remember + r'''
}
class ControlsTest {
    interface Exchange { JSONObject request(String url,String body) throws Exception; }
    QueueExecutor executor=new QueueExecutor();
    Ui ui=new Ui();
    Exchange exchange;
    History history=new History();
    java.util.function.Consumer<String> onConnected=history::rememberStation;
    boolean active=true,reading,motionBusy,clientBusy;
    int generation=1,failedGeneration=-1;
    long lastSuccess=9999,lastPollAttempt,pendingOperation=-1,pendingStationBoot,clientDeadline;
    String endpoint="http://station.invalid";
    JSONObject track,control,status;
    void render() {}
    void showError(String message) {}
''' + methods + r'''
    static void check(boolean value,String message) { if(!value)throw new AssertionError(message); }
    static class Fixture {
        ControlsTest subject=new ControlsTest();
        List<String> posts=new ArrayList<>();
        JSONObject servo=new JSONObject().with("control_boot_id",17L).with("control_epoch",22L)
            .with("command_seq",4L).with("clock_ms",10000L).with("output_ready",true);
        boolean failNext;
        Runnable beforeFailure=()->{};
        Fixture() {
            SystemClock.now=10000;
            subject.track=new JSONObject().with("servo",servo);
            subject.control=new JSONObject().with("station_boot_id",17L).with("boot_id",7L)
                .with("client_id",5L).with("can_control",true).with("state","ready")
                .with("operation_id",1L).with("command","pending");
            subject.status=new JSONObject();
            subject.exchange=(url,body)->{
                if(failNext) { failNext=false;beforeFailure.run();throw new IOException("connection reset"); }
                if(body!=null)posts.add(url);
                return url.contains("/api/client/control")?subject.control:subject.track;
            };
        }
        void action(boolean client) {
            if(client)subject.clientAction();else subject.motion("/api/servo?angle=90");
        }
    }
    static void failedPollCancels(boolean client,boolean uiFirst) {
        Fixture f=new Fixture();ControlsTest x=f.subject;
        x.tick();f.action(client);f.failNext=true;
        x.executor.runOne();
        if(uiFirst)x.ui.drain();
        x.executor.runOne();x.ui.drain();
        check(f.posts.isEmpty(),"queued "+(client?"client":"motion")+" POST survived poll failure; uiFirst="+uiFirst);
        check(!x.motionBusy&&!x.clientBusy&&!x.reading,"disconnect left controls busy");
        // A new successful poll permits a fresh gesture; no old POST is retried.
        SystemClock.now+=1300;x.tick();x.executor.runOne();x.ui.drain();
        check(x.fresh(),"polling did not recover");
        check(f.posts.isEmpty(),"poll recovery replayed cancelled operation");
        f.action(client);x.executor.runOne();x.ui.drain();
        check(f.posts.size()==1,"new gesture after reconnect did not send exactly once");
    }
    static void staleStatusCancels(boolean client) {
        Fixture f=new Fixture();ControlsTest x=f.subject;
        x.lastSuccess=5501;f.action(client);
        SystemClock.now+=600;x.tick();x.executor.runOne();x.ui.drain();
        check(f.posts.isEmpty(),"queued action survived status freshness expiry");
    }
    static void oldPollCannotInvalidateNewGeneration() {
        Fixture f=new Fixture();ControlsTest x=f.subject;x.tick();f.failNext=true;
        // The old HTTP call completes after a new foreground/endpoint generation.
        f.beforeFailure=()->{x.generation++;x.lastSuccess=SystemClock.now;x.reading=true;x.motionBusy=true;};
        x.executor.runOne();x.ui.drain();
        check(x.generation==2&&x.fresh()&&x.reading&&x.motionBusy,"old poll failure altered new connection");
    }
    static void freshnessCheckedAfterGet(boolean client) {
        Fixture f=new Fixture();ControlsTest x=f.subject;x.lastSuccess=5501;
        Exchange normal=x.exchange;
        x.exchange=(url,body)->{if(body==null)SystemClock.now+=600;return normal.request(url,body);};
        f.action(client);x.executor.runOne();x.ui.drain();
        check(f.posts.isEmpty(),"freshness expired during GET but POST was sent");
    }
    static void healthyAndLeaseGuards() {
        for(int changed=0;changed<4;changed++) {
            Fixture f=new Fixture();ControlsTest x=f.subject;f.action(false);
            if(changed==1)SystemClock.now+=2000;
            // Replace, rather than mutate, the captured snapshot as a real GET does.
            if(changed==2||changed==3) {
                JSONObject next=new JSONObject();next.putAll(f.servo);
                next.put(changed==2?"control_boot_id":"control_epoch",99L);
                x.track=new JSONObject().with("servo",next);
            }
            x.executor.runOne();x.ui.drain();
            check(f.posts.size()==(changed==0?1:0),"boot/epoch/deadline guard failed: "+changed);
            if(changed==0)check(f.posts.get(0).endsWith("epoch=22&seq=5&stamp=10000"),"motion metadata changed");
        }
    }
    static void recentSuccessfulStations() {
        Fixture f=new Fixture();ControlsTest x=f.subject;History h=x.history;
        String a="http://192.168.1.10",b="http://192.168.1.20",c="http://192.168.1.30";
        for(String endpoint:new String[]{a,a,b,a,c}) {
            x.endpoint=endpoint;h.station=endpoint;SystemClock.now+=1300;
            x.tick();x.executor.runOne();
            // Successful HTTP alone is insufficient until the current UI generation accepts it.
            int before=h.prefs.writes;x.ui.drain();
            check(h.prefs.writes<=before+1,"poll wrote history more than once");
        }
        check(h.prefs.getString("recent_station_1","").equals(c),"newest IP missing");
        check(h.prefs.getString("recent_station_2","").equals(a),"dedupe/order/limit incorrect");
        check(h.prefs.writes==4,"repeated successful poll rewrote history");
        // A recreated Activity reads the same persisted pair.
        History reopened=new History();reopened.prefs=h.prefs;
        check(reopened.prefs.getString("recent_station_2","").equals(a),"history did not persist");
        for(int failedRequest=1;failedRequest<=3;failedRequest++) {
            Fixture bad=new Fixture();ControlsTest y=bad.subject;
            y.history.prefs=h.prefs;y.history.station=b;y.endpoint=b;
            int failAt=failedRequest;int[] calls={0};Exchange normal=y.exchange;
            y.exchange=(url,body)->{if(++calls[0]==failAt)throw new IOException("offline");return normal.request(url,body);};
            y.tick();y.executor.runOne();y.ui.drain();
            check(h.prefs.writes==4,"failed/partial poll recorded an IP");
        }
        for(int stale=0;stale<3;stale++) {
            Fixture old=new Fixture();ControlsTest y=old.subject;
            y.history.prefs=h.prefs;y.history.station=b;y.endpoint=b;
            y.tick();y.executor.runOne();
            if(stale==0)y.generation++;else if(stale==1)y.active=false;else y.history.resumed=false;
            y.ui.drain();check(h.prefs.writes==4,"stale/background success recorded an IP");
        }
        System.out.println("PASS Android recent IPs: successful full poll only, newest two, dedupe, persistence, failed/partial/stale/background responses");
    }
    public static void main(String[] args) {
        for(boolean client:new boolean[]{false,true}) {
            for(boolean uiFirst:new boolean[]{false,true})failedPollCancels(client,uiFirst);
            staleStatusCancels(client);
            freshnessCheckedAfterGet(client);
        }
        oldPollCannotInvalidateNewGeneration();
        healthyAndLeaseGuards();
        recentSuccessfulStations();
        System.out.println("PASS Android queued controls: poll failure/UI ordering, expiry, reconnect, boot/epoch/deadline");
    }
}
''' + lease


def lifecycle_source():
    source = (JAVA / 'MainActivity.java').read_text()
    methods = '\n'.join(block(source, marker).replace('@Override ', '') for marker in (
        '@Override protected void onResume()', '@Override protected void onPause()',
        'private void stopRecording()'))
    finalize = block(source, 'else if (event instanceof VideoRecordEvent.Finalize)')
    finalize = finalize[finalize.index('{') + 1:-1]
    return r'''
class Base { protected void onResume() {} protected void onPause() {} }
class View {
    static int GONE=8;boolean enabled=true;
    void setEnabled(boolean value) { enabled=value; }
    void setVisibility(int value) {} void setImageDrawable(Object drawable) {}
    void setContentDescription(String text) {}
}
class CaptureButtonDrawable { CaptureButtonDrawable(boolean recording) {} }
class Handler {
    void postDelayed(Runnable task,int delay) {} void removeCallbacks(Runnable task) {}
    void post(Runnable task) {}
}
class Recording { void stop() {} }
class VideoRecordEvent { static class Finalize { boolean hasError(){return false;}int getError(){return 0;} } }
class StationControls { void stop() {} }
class LifecycleTest extends Base {
    boolean resumed=true,destroyed,verified=true,connecting,finalizing,paused,cameraReady=true;
    int generation;
    Object latestLocation=new Object(),video=new Object();
    Recording recording=new Recording();
    View recordButton=new View(),pauseButton=new View(),cameraStatus=new View();
    Handler main=new Handler();Runnable tick=()->{};
    String recordingStatus="",station="http://station.invalid";
    StationControls stationControls=new StationControls();
    void startLocations() {} void stopLocations() {} void togglePanel() {} void connectStation() {}
    void setFormatEnabled(boolean enabled) {} void updateCameraStatus() {} void toast(String message) {}
''' + methods + '\nvoid finalizeEvent(Object event) {' + finalize + r'''}
    static void check(boolean value,String message) { if(!value)throw new AssertionError(message); }
    public static void main(String[] args) {
        LifecycleTest saved=new LifecycleTest();
        saved.onPause();saved.finalizeEvent(new VideoRecordEvent.Finalize());
        check(!saved.recordButton.enabled,"background Finalize enabled recording");
        saved.onResume();check(saved.recordButton.enabled,"background Finalize permanently disabled recording after resume");
        LifecycleTest saving=new LifecycleTest();saving.onPause();saving.onResume();
        check(!saving.recordButton.enabled,"resume enabled recording before Finalize");
        saving.finalizeEvent(new VideoRecordEvent.Finalize());
        check(saving.recordButton.enabled,"foreground Finalize did not restore recording");
        LifecycleTest unavailable=new LifecycleTest();unavailable.recording=null;
        unavailable.cameraReady=false;unavailable.video=null;unavailable.recordButton.enabled=false;
        unavailable.onPause();unavailable.onResume();
        check(!unavailable.recordButton.enabled,"resume enabled recording without camera");
        unavailable.video=new Object();unavailable.onResume();
        check(!unavailable.recordButton.enabled,"resume enabled recording despite camera error");
        System.out.println("PASS Android recording lifecycle: background/foreground Finalize, pending save, camera unavailable/error");
    }
}
'''


def main():
    try:
        javac, java = java_tool('javac'), java_tool('java')
        failed = False
        with tempfile.TemporaryDirectory(prefix='shore-android-lifecycle-') as temp:
            for name, source in [('ControlsTest', controls_source()), ('LifecycleTest', lifecycle_source())]:
                path = Path(temp) / name
                path.mkdir()
                java_file = path / f'{name}.java'
                java_file.write_text(source)
                subprocess.run([javac, '--release', '17', '-d', str(path), str(java_file)], check=True)
                result = subprocess.run([java, '-cp', str(path), name])
                failed |= result.returncode != 0
        return 1 if failed else 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'Android host regression unavailable/failed: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
