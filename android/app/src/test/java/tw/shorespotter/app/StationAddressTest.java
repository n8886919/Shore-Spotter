package tw.shorespotter.app;

import static org.junit.Assert.*;
import org.junit.Test;

public class StationAddressTest {
    @Test public void acceptsOnlyLocalStationOrigins() {
        assertEquals("http://192.168.31.10", StationAddress.normalize(" 192.168.31.10/ "));
        assertEquals("http://station-584c.local:8080", StationAddress.normalize("http://station-584c.local:8080"));
        assertEquals("http://172.31.1.2", StationAddress.normalize("172.31.1.2"));
        for (String bad : new String[]{"https://192.168.1.2", "http://example.com", "http://8.8.8.8",
                "http://192.168.1.256", "http://172.32.1.1", "http://192.168.1.2/api/status",
                "http://user:pass@192.168.1.2", "http://192.168.1.2?foo", "http://192.168.1.2:0"}) {
            assertThrows(bad, IllegalArgumentException.class, () -> StationAddress.normalize(bad));
        }
    }
}
