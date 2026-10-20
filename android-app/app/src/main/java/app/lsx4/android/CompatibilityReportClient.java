package app.lsx4.android;

import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import javax.net.ssl.HttpsURLConnection;


final class CompatibilityReportClient {
    interface ConnectionFactory { HttpsURLConnection open(URL url) throws IOException; }
    static final class Receipt {
        final String id, receivedAt;
        Receipt(String id, String receivedAt) { this.id = id; this.receivedAt = receivedAt; }
    }

    static Receipt send(String endpoint, String json) throws Exception {
        return send(endpoint, json, url -> (HttpsURLConnection) url.openConnection());
    }

    static Receipt send(String endpoint, String json, ConnectionFactory factory) throws Exception {
        URL url = new URL(endpoint);
        if (!"https".equalsIgnoreCase(url.getProtocol()) || url.getUserInfo() != null
                || url.getRef() != null) throw new IOException("HTTPS required");
        byte[] body = json.getBytes(StandardCharsets.UTF_8);
        if (body.length > CompatibilityReport.MAX_BYTES) throw new IOException("Report too large");
        HttpsURLConnection connection = factory.open(url);
        try {
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(8000);
            connection.setReadTimeout(10000);
            connection.setRequestMethod("POST");
            connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("User-Agent", "LSX4-CompatibilityReports/1");
            connection.setFixedLengthStreamingMode(body.length);
            connection.setDoOutput(true);
            try (java.io.OutputStream output = connection.getOutputStream()) { output.write(body); }
            int code = connection.getResponseCode();
            if (code != 201 && code != 202) throw new IOException("HTTP " + code);
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (InputStream input = connection.getInputStream()) {
                byte[] buffer = new byte[512];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (bytes.size() + count > 4096) throw new IOException("Response too large");
                    bytes.write(buffer, 0, count);
                }
            }
            JSONObject receipt = new JSONObject(bytes.toString("UTF-8"));
            String id = receipt.getString("report_id");
            if (!UUID.fromString(id).toString().equals(id)) throw new IOException("Invalid receipt");
            String at = receipt.getString("received_at");
            if (!at.matches("[0-9T:.+Z-]{10,40}")) throw new IOException("Invalid receipt time");
            return new Receipt(id, at);
        } finally { connection.disconnect(); }
    }

    private CompatibilityReportClient() { }
}
