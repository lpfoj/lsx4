package app.lsx4.android;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONObject;
import java.nio.charset.StandardCharsets;


public final class CompatibilityReportActivity extends Activity {
    private static final String PREFS = "lsx4_compatibility_report_draft";
    private static final String DRAFT = "pending_json";
    private JSONObject report;
    private Button send;
    private TextView outcome;
    private LinearLayout fields;
    private volatile boolean closed;
    private SharedPreferences drafts;

    @Override protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLocale.wrap(base));
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        UiChrome.preparePortraitWindow(this);
        drafts = getSharedPreferences(PREFS, MODE_PRIVATE);
        String id = getIntent().getStringExtra("title_id");
        String title = getIntent().getStringExtra("title");
        String platform = getIntent().getStringExtra("platform");
        GameCompatibilityStatus status = new GameCompatibilityRepository(this).statusOf(id);
        if (id == null || title == null || status == GameCompatibilityStatus.UNCLASSIFIED
                || !("PS4".equals(platform) || "PS5".equals(platform))) { finish(); return; }
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (16 * getResources().getDisplayMetrics().density);
        root.setPadding(padding, padding, padding, padding);
        UiChrome.applyInsets(root, padding, padding, padding, padding);
        ScrollView scroll = new ScrollView(this);
        fields = new LinearLayout(this);
        fields.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(fields);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        outcome = new TextView(this);
        outcome.setText(R.string.report_loading);
        root.addView(outcome);
        Button policy = new Button(this);
        policy.setText(R.string.report_privacy);
        policy.setOnClickListener(v -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://lsx45.app/privacy/"))); }
            catch (android.content.ActivityNotFoundException unavailable) { }
        });
        root.addView(policy);
        LinearLayout actions = new LinearLayout(this);
        Button cancel = new Button(this);
        cancel.setText(android.R.string.cancel);
        cancel.setOnClickListener(v -> finish());
        actions.addView(cancel, new LinearLayout.LayoutParams(0, -2, 1));
        send = new Button(this);
        send.setText(R.string.report_send);
        send.setEnabled(false);
        send.setOnClickListener(v -> submit());
        actions.addView(send, new LinearLayout.LayoutParams(0, -2, 1));
        root.addView(actions);
        setContentView(root);
        new Thread(() -> {
            try {
                JSONObject snapshot = null;
                String saved = drafts.getString(DRAFT, null);
                if (saved != null) try {
                    JSONObject pending = new JSONObject(saved);
                    if (id.equals(pending.getJSONObject("game").getString("title_id"))
                            && (status == GameCompatibilityStatus.SUPPORTED ? "supported" : "unsupported")
                            .equals(pending.getString("result"))) snapshot = pending;
                } catch (Exception invalid) { }
                if (snapshot == null) snapshot = CompatibilityReport.create(this, title, id, platform,
                        status, DeviceGraphicsInfo.collect());
                JSONObject ready = snapshot;
                runOnUiThread(() -> {
                    if (closed || isFinishing()) return;
                    report = ready;
                    preview();
                });
            } catch (Exception unavailable) {
                runOnUiThread(() -> { if (!closed) outcome.setText(R.string.report_prepare_error); });
            }
        }, "compatibility-preview").start();
    }

    private void row(int label, Object value) {
        TextView item = new TextView(this);
        item.setText(getString(label) + ": " + (value == null || value == JSONObject.NULL
                ? getString(R.string.report_unknown) : String.valueOf(value)));
        item.setTextSize(15);
        item.setTextIsSelectable(true);
        item.setPadding(0, 6, 0, 6);
        fields.addView(item);
    }

    private void preview() {
        fields.removeAllViews();
        TextView purpose = new TextView(this);
        purpose.setText(R.string.report_consent);
        purpose.setTextSize(16);
        fields.addView(purpose);
        JSONObject game = report.optJSONObject("game"), device = report.optJSONObject("device");
        row(R.string.report_game, game.opt("title"));
        row(R.string.report_title_id, game.opt("title_id"));
        row(R.string.report_platform, game.opt("platform"));
        row(R.string.report_result, getString("supported".equals(report.optString("result"))
                ? R.string.game_status_supported : R.string.game_status_unsupported));
        row(R.string.report_app_version, report.opt("app_version"));
        String[] keys = {"manufacturer", "model", "android_version", "android_api", "soc", "cpu_abi",
                "gpu_renderer", "gpu_vendor", "vulkan_device", "vulkan_api_version", "vulkan_driver_version", "ram_total_mib"};
        int[] labels = {R.string.report_manufacturer, R.string.report_model, R.string.report_android,
                R.string.report_android_api, R.string.report_soc, R.string.report_abi,
                R.string.report_gpu, R.string.report_gpu_vendor, R.string.report_vulkan_device,
                R.string.report_vulkan_api, R.string.report_vulkan_driver, R.string.report_ram};
        for (int i = 0; i < keys.length; i++) row(labels[i], device.opt(keys[i]));
        row(R.string.report_schema, report.opt("schema_version"));
        row(R.string.report_request_id, report.opt("client_report_id"));
        if (report.toString().getBytes(StandardCharsets.UTF_8).length > CompatibilityReport.MAX_BYTES) {
            outcome.setText(R.string.report_prepare_error);
        } else {
            outcome.setText(R.string.report_ready);
            send.setEnabled(true);
        }
    }

    private void submit() {
        if (report == null || !send.isEnabled()) return;
        String body = report.toString();
        send.setEnabled(false);
        outcome.setText(R.string.report_sending);

        boolean saved;
        synchronized (CompatibilityReportActivity.class) {
            saved = drafts.edit().putString(DRAFT, body).commit();
        }
        if (!saved) {
            outcome.setText(R.string.report_error);
            send.setText(R.string.report_retry);
            send.setEnabled(true);
            return;
        }
        new Thread(() -> {
            try {
                CompatibilityReportClient.Receipt receipt = CompatibilityReportClient.send(
                        BuildConfig.LSX4_COMPATIBILITY_REPORT_URL, body);
                synchronized (CompatibilityReportActivity.class) {
                    if (body.equals(drafts.getString(DRAFT, null))) drafts.edit().remove(DRAFT).commit();
                }
                runOnUiThread(() -> {
                    if (closed || isFinishing()) return;
                    outcome.setText(getString(R.string.report_success, receipt.id, receipt.receivedAt));
                    outcome.setTextIsSelectable(true);
                    send.setText(R.string.report_copy);
                    send.setEnabled(true);
                    send.setOnClickListener(v -> {
                        ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                        if (clipboard != null) clipboard.setPrimaryClip(ClipData.newPlainText(
                                getString(R.string.report_copy), receipt.id));
                    });
                });
            } catch (Exception failure) {
                runOnUiThread(() -> {
                    if (closed || isFinishing()) return;
                    outcome.setText(R.string.report_error);
                    send.setText(R.string.report_retry);
                    send.setEnabled(true);
                });
            }
        }, "compatibility-submit").start();
    }

    @Override protected void onDestroy() { closed = true; super.onDestroy(); }
}
