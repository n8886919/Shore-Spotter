package tw.shorespotter.app;
import org.junit.Test;
import static org.junit.Assert.*;
public class LocationSamplePolicyTest {
    @Test public void acceptsOnlyNewRecentSamples() {
        long now = 100_000_000_000L;
        assertTrue(LocationSamplePolicy.shouldSend("gps", now - 1, 0, now));
        assertTrue(LocationSamplePolicy.shouldSend("gps", now - 10_000_000_000L, 0, now));
        assertFalse(LocationSamplePolicy.shouldSend("gps", now - 10_000_000_001L, 0, now));
        assertFalse(LocationSamplePolicy.shouldSend("gps", now - 1, now - 1, now));
        assertFalse(LocationSamplePolicy.shouldSend("gps", now - 2, now - 1, now));
        assertFalse(LocationSamplePolicy.shouldSend("gps", now + 1, 0, now));
        assertFalse(LocationSamplePolicy.shouldSend("gps", 0, 0, now));
    }
    @Test public void newerNetworkLocationCannotReplaceGps() {
        long now = 100_000_000_000L;
        long lastGps = now - 3_000_000_000L;
        assertFalse(LocationSamplePolicy.shouldSend("network", now - 1, lastGps, now));
        assertTrue(LocationSamplePolicy.shouldSend("gps", now - 2_000_000_000L, lastGps, now));
    }
    @Test public void rejectsOtherOrMissingProviders() {
        long now = 100_000_000_000L;
        for (String provider : new String[]{"fused", "passive", "", null}) {
            assertFalse(LocationSamplePolicy.shouldSend(provider, now - 1, 0, now));
        }
    }
}
