package tw.shorespotter.app;
import org.junit.Test;
import static org.junit.Assert.*;
public class ControlLeaseTest {
    @Test public void dropsExpiredReconnectedRebootedAndChangedModeActions() {
        ControlLease lease = new ControlLease(1000, 3, 17, 22);
        assertTrue(lease.valid(2999, 3, 17, 22));
        assertFalse(lease.valid(3000, 3, 17, 22));
        assertFalse(lease.valid(1001, 4, 17, 22));
        assertFalse(lease.valid(1001, 3, 18, 22));
        assertFalse(lease.valid(1001, 3, 17, 23));
        assertEquals(0, ControlLease.nextSequence(0xffffffffL));
        assertEquals(2147483648L, ControlLease.nextSequence(2147483647L));
    }
}
