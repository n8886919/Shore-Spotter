package tw.shorespotter.app;

/** Only new, recent satellite GPS samples may refresh a Station. */
public final class LocationSamplePolicy {
    private LocationSamplePolicy() {}
    public static boolean shouldSend(String provider, long sampleNanos, long lastOfferedNanos, long nowNanos) {
        return "gps".equals(provider) && sampleNanos > lastOfferedNanos && sampleNanos > 0 && sampleNanos <= nowNanos
                && nowNanos - sampleNanos <= 10_000_000_000L;
    }
}
