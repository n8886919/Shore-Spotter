package tw.shorespotter.app;

import java.net.URI;
import java.util.Locale;

/** Local HTTP endpoint only. A phone's debug address is never a Station default. */
public final class StationAddress {
    private StationAddress() {}
    public static String normalize(String value) {
        String text = value.trim();
        if (!text.contains("://")) text = "http://" + text;
        URI uri = URI.create(text);
        String host = uri.getHost();
        if (host == null || uri.getRawUserInfo() != null || uri.getRawQuery() != null
                || uri.getRawFragment() != null || !"http".equalsIgnoreCase(uri.getScheme())
                || (uri.getPath() != null && !uri.getPath().isEmpty() && !"/".equals(uri.getPath()))) {
            throw new IllegalArgumentException("請輸入 Station IP，例如 192.168.1.100");
        }
        host = host.toLowerCase(Locale.ROOT);
        boolean local = host.endsWith(".local") && host.length() > 6;
        if (host.matches("[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+")) {
            String[] pieces = host.split("\\.");
            int[] octets = new int[4];
            for (int i = 0; i < 4; i++) {
                octets[i] = Integer.parseInt(pieces[i]);
                if (octets[i] > 255) throw new IllegalArgumentException("IP 位址不合法");
            }
            local = octets[0] == 10 || (octets[0] == 192 && octets[1] == 168)
                    || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31);
        }
        int port = uri.getPort();
        if (!local || port == 0 || port > 65535) throw new IllegalArgumentException("只接受區域網路 IP 或 .local Station 網址");
        return "http://" + host + (port == -1 || port == 80 ? "" : ":" + port);
    }
}
