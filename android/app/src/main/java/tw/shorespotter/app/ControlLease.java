package tw.shorespotter.app;

/** A user action cannot survive a reconnect, reboot, mode generation, or two-second deadline. */
public final class ControlLease {
    private final long deadline, boot, epoch;
    private final int generation;
    public ControlLease(long now, int generation, long boot, long epoch) {
        this.deadline = now + 2000; this.generation = generation; this.boot = boot; this.epoch = epoch;
    }
    public boolean valid(long now, int generation, long boot, long epoch) {
        return now < deadline && generation == this.generation && boot == this.boot && epoch == this.epoch;
    }
    public static long nextSequence(long value) { return (value + 1) & 0xffffffffL; }
}
