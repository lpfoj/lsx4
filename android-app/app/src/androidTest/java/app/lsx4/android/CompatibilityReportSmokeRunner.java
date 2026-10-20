package app.lsx4.android;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.io.ByteArrayOutputStream;
import java.net.URL;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.security.cert.Certificate;
import javax.net.ssl.HttpsURLConnection;


public final class CompatibilityReportSmokeRunner extends Instrumentation {
    private Context context;
    private GameCompatibilityRepository repository;
    private final String id = "CUSA99999";
    private JSONObject evidence = new JSONObject();
    private boolean uiOnly;

    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        uiOnly = arguments != null && arguments.getBoolean("uiOnly", false);
        if (arguments != null && "true".equals(arguments.getString("uiOnly"))) uiOnly = true;
        start();
    }
    private void check(boolean value, String detail) { if (!value) throw new AssertionError(detail); }
    private void save(String name, String data) throws Exception {
        try (FileOutputStream output = context.openFileOutput(name, Context.MODE_PRIVATE)) {
            output.write(data.getBytes(StandardCharsets.UTF_8));
        }
    }
    private CompatibilityReportActivity preview(String title) {
        Intent intent = new Intent(context, CompatibilityReportActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK).putExtra("title_id", id)
                .putExtra("title", title).putExtra("platform", "PS4");
        return (CompatibilityReportActivity) startActivitySync(intent);
    }
    private Button button(View view, String text) {
        if (view instanceof Button && text.contentEquals(((Button) view).getText())) return (Button) view;
        if (view instanceof ViewGroup) for (int i=0; i<((ViewGroup)view).getChildCount(); i++) {
            Button found = button(((ViewGroup)view).getChildAt(i), text);
            if (found != null) return found;
        }
        return null;
    }
    private String screen(View view) {
        String text = view instanceof TextView ? ((TextView)view).getText().toString() + "\n" : "";
        if (view instanceof ViewGroup) for (int i=0; i<((ViewGroup)view).getChildCount(); i++)
            text += screen(((ViewGroup)view).getChildAt(i));
        return text;
    }
    private Button awaitButton(Activity activity, int label) throws Exception {
        String text = activity.getString(label);
        long deadline = android.os.SystemClock.uptimeMillis() + 25000;
        while (android.os.SystemClock.uptimeMillis() < deadline) {
            final Button[] found = new Button[1];
            runOnMainSync(() -> found[0] = button(activity.getWindow().getDecorView(), text));
            if (found[0] != null && found[0].isEnabled()) return found[0];
            Thread.sleep(100);
        }
        throw new AssertionError("Missing enabled button: " + text);
    }
    private JSONObject snapshot(CompatibilityReportActivity activity) throws Exception {
        java.lang.reflect.Field field = CompatibilityReportActivity.class.getDeclaredField("report");
        field.setAccessible(true);
        return new JSONObject(((JSONObject) field.get(activity)).toString());
    }
    private void finishActivity(Activity activity) { runOnMainSync(activity::finish); waitForIdleSync(); }

    private void libraryUi() throws Exception {
        MainActivity host = (MainActivity) startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        java.lang.reflect.Field field = MainActivity.class.getDeclaredField("launcherUi");
        field.setAccessible(true);
        LauncherUi ui = (LauncherUi) field.get(host);
        Class<?> entryClass = Class.forName("app.lsx4.android.LauncherUi$GameEntry");
        java.lang.reflect.Constructor<?> ctor = entryClass.getDeclaredConstructors()[0];
        ctor.setAccessible(true);
        File fixture = new File(context.getFilesDir(),"report-ui-fixture");
        Object game = ctor.newInstance(fixture,new File(fixture,"eboot.bin"),null,id,
                "LSX4 local rating test","ps4",false,0,0L);
        java.lang.reflect.Method rowMethod = LauncherUi.class.getDeclaredMethod("gameRow",entryClass);
        rowMethod.setAccessible(true);
        final View[] row = new View[1];
        runOnMainSync(() -> {
            try { row[0] = (View) rowMethod.invoke(ui,game); host.setContentView(row[0]); }
            catch(Exception e) { throw new RuntimeException(e); }
        });
        repository.clear(id);
        long down=android.os.SystemClock.uptimeMillis();
        runOnMainSync(() -> row[0].dispatchTouchEvent(android.view.MotionEvent.obtain(down,down,
                android.view.MotionEvent.ACTION_DOWN,30,30,0)));
        Thread.sleep(5100);
        long up=android.os.SystemClock.uptimeMillis();
        runOnMainSync(() -> row[0].dispatchTouchEvent(android.view.MotionEvent.obtain(down,up,
                android.view.MotionEvent.ACTION_UP,30,30,0)));
        check(repository.statusOf(id)==GameCompatibilityStatus.SUPPORTED,"actual hold rating");
        check(!context.getSharedPreferences("lsx4_compatibility_report_draft",0).contains("pending_json"),"hold sent");
        java.lang.reflect.Method menu = LauncherUi.class.getDeclaredMethod("openGameSettings",entryClass);
        menu.setAccessible(true);
        java.lang.reflect.Field style = LauncherUi.class.getDeclaredField("homeStyle");
        style.setAccessible(true);
        for(int layout=0;layout<7;layout++) {
            style.setInt(ui,layout);
            final android.app.AlertDialog[] opened = new android.app.AlertDialog[1];
            runOnMainSync(() -> {
                try { opened[0]=(android.app.AlertDialog) menu.invoke(ui,game); }
                catch(Exception error) { throw new RuntimeException(error); }
            });
            check(opened[0].isShowing(),"menu not shown");
            check(opened[0].getListView().getAdapter().getCount()==2,"rated menu action count");
            check(host.getString(R.string.report_action).contentEquals(String.valueOf(
                    opened[0].getListView().getAdapter().getItem(1))),"report action label in layout " + layout);
            runOnMainSync(opened[0]::dismiss);
        }
        repository.clear(id);
        final android.app.AlertDialog[] opened = new android.app.AlertDialog[1];
        runOnMainSync(() -> {
            try { opened[0]=(android.app.AlertDialog) menu.invoke(ui,game); }
            catch(Exception error) { throw new RuntimeException(error); }
        });
        check(opened[0].getListView().getAdapter().getCount()==1,"unclassified menu action visible");
        runOnMainSync(opened[0]::dismiss);
        finishActivity(host);
        evidence.put("actual_hold_local_only",true).put("seven_menu_actions",true)
                .put("unclassified_action_hidden",true).put("passed",true);
        save("compatibility-library-ui-result.json",evidence.toString(2));
    }


    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            context = getTargetContext();
            check(context.getPackageName().equals("app.lsx4.android.reports"), "isolated package required");
            repository = new GameCompatibilityRepository(context);
            context.getSharedPreferences("lsx4_compatibility_report_draft", 0).edit().clear().commit();
            if(uiOnly) {
                libraryUi();
                result.putString("result",evidence.toString());
                finish(Activity.RESULT_OK,result);
                return;
            }
            repository.setStatus(id, GameCompatibilityStatus.SUPPORTED);
            CompatibilityReportActivity cancelled = preview("LSX4 consent test");
            awaitButton(cancelled, R.string.report_send);
            JSONObject cancelledPayload = snapshot(cancelled);
            save("report-cancelled.json", cancelledPayload.toString());
            finishActivity(cancelled);
            check(!context.getSharedPreferences("lsx4_compatibility_report_draft",0).contains("pending_json"), "cancel sent");
            check(repository.statusOf(id)==GameCompatibilityStatus.SUPPORTED, "cancel changed rating");
            evidence.put("cancel_no_request", cancelledPayload.getString("client_report_id"));

            for (GameCompatibilityStatus status : new GameCompatibilityStatus[]{GameCompatibilityStatus.SUPPORTED,
                    GameCompatibilityStatus.UNSUPPORTED}) {
                repository.setStatus(id, status);
                CompatibilityReportActivity activity = preview("LSX4 end-to-end test");
                Button send = awaitButton(activity, R.string.report_send);
                JSONObject payload = snapshot(activity);
                save("report-"+payload.getString("result")+".json", payload.toString());
                final String[] text = new String[1];
                runOnMainSync(() -> text[0]=screen(activity.getWindow().getDecorView()));
                check(text[0].contains(payload.getJSONObject("device").getString("model")), "device preview");
                runOnMainSync(send::performClick);
                awaitButton(activity, R.string.report_copy);
                runOnMainSync(() -> text[0]=screen(activity.getWindow().getDecorView()));
                save("receipt-"+payload.getString("result")+".txt", text[0]);
                check(repository.statusOf(id)==status, "send changed rating");
                evidence.put(payload.getString("result"), payload.getString("client_report_id"));
                finishActivity(activity);
            }


            repository.setStatus(id, GameCompatibilityStatus.SUPPORTED);
            CompatibilityReportActivity rejected = preview(new String(new char[257]).replace('\0', 'x'));
            Button send = awaitButton(rejected, R.string.report_send);
            JSONObject first = snapshot(rejected);
            runOnMainSync(send::performClick);
            Button retry = awaitButton(rejected, R.string.report_retry);
            check(first.toString().equals(snapshot(rejected).toString()), "failure regenerated UUID");
            runOnMainSync(retry::performClick);
            awaitButton(rejected, R.string.report_retry);
            check(first.toString().equals(context.getSharedPreferences("lsx4_compatibility_report_draft",0)
                    .getString("pending_json", "")), "retry payload changed");
            check(repository.statusOf(id)==GameCompatibilityStatus.SUPPORTED, "error changed rating");
            finishActivity(rejected);
            evidence.put("manual_retry_same_payload", true);

            JSONObject missing = CompatibilityReport.create(context, "LSX4 null GPU test", id, "PS4",
                    GameCompatibilityStatus.UNSUPPORTED, new String[0]);
            for(String key:new String[]{"gpu_renderer","gpu_vendor","vulkan_device","vulkan_api_version","vulkan_driver_version"})
                check(missing.getJSONObject("device").isNull(key), "missing graphics field " + key);
            context.getSharedPreferences("lsx4_compatibility_report_draft",0).edit().clear().commit();
            repository.setStatus(id, GameCompatibilityStatus.UNSUPPORTED);
            CompatibilityReportActivity noGraphics = preview("LSX4 null GPU test");
            awaitButton(noGraphics, R.string.report_send);
            java.lang.reflect.Field reportField = CompatibilityReportActivity.class.getDeclaredField("report");
            reportField.setAccessible(true);
            reportField.set(noGraphics, missing);
            java.lang.reflect.Method showPreview = CompatibilityReportActivity.class.getDeclaredMethod("preview");
            showPreview.setAccessible(true);
            runOnMainSync(() -> {
                try { showPreview.invoke(noGraphics); } catch(Exception error) { throw new RuntimeException(error); }
            });
            final String[] missingText = new String[1];
            runOnMainSync(() -> missingText[0] = screen(noGraphics.getWindow().getDecorView()));
            check(missingText[0].contains(noGraphics.getString(R.string.report_unknown)), "null graphics preview");
            waitForIdleSync();
            Thread.sleep(250);
            try (FileOutputStream image=context.openFileOutput("compatibility-preview.png",Context.MODE_PRIVATE)) {
                getUiAutomation().takeScreenshot().compress(android.graphics.Bitmap.CompressFormat.PNG,100,image);
            }
            Button nullSend = awaitButton(noGraphics, R.string.report_send);
            runOnMainSync(nullSend::performClick);
            awaitButton(noGraphics, R.string.report_copy);
            runOnMainSync(() -> missingText[0]=screen(noGraphics.getWindow().getDecorView()));
            save("receipt-null-graphics.txt",missingText[0]);
            finishActivity(noGraphics);
            save("report-null-graphics.json", missing.toString());
            evidence.put("null_graphics_client_report_id", missing.getString("client_report_id"));

            final int[] opened = {0};
            try {
                CompatibilityReportClient.send("http://example.invalid", missing.toString(), url->{opened[0]++;return null;});
                throw new AssertionError("HTTP accepted");
            } catch (java.io.IOException expected) { check(opened[0]==0,"HTTP network request"); }
            for (boolean timeout : new boolean[]{false,true}) {
                try {
                    CompatibilityReportClient.send("https://example.invalid", missing.toString(), url -> new FailureConnection(url,timeout));
                    throw new AssertionError("expected failure");
                } catch(java.io.IOException expected) { }
                check(repository.statusOf(id)==GameCompatibilityStatus.UNSUPPORTED,"network error changed rating");
            }
            try {
                CompatibilityReportClient.send("https://127.0.0.1:1/",missing.toString());
                throw new AssertionError("unavailable server accepted");
            } catch(java.io.IOException expected) { }
            evidence.put("https_and_offline_timeout",true);
            repository.clear(id);
            check(repository.statusOf(id)==GameCompatibilityStatus.UNCLASSIFIED,"clear failed");
            try {
                CompatibilityReport.create(context,"unrated",id,"PS4",GameCompatibilityStatus.UNCLASSIFIED,new String[0]);
                throw new AssertionError("unclassified allowed");
            } catch(IllegalArgumentException expected) { }
            evidence.put("unclassified_rejected",true);
            String[] languages={"en","ru","es","fr","de","zh-CN"};
            String[] ratings={"Playable","Играбельна","Jugable","Jouable","Spielbar","可玩"};
            String[] actions={"Send","Отправить","Enviar","Envoyer","Senden","发送"};
            for(int locale=0;locale<languages.length;locale++) {
                context.getSharedPreferences("lsx4_settings",0).edit().putString("app_language",languages[locale]).commit();
                repository.setStatus(id,GameCompatibilityStatus.SUPPORTED);
                CompatibilityReportActivity localized=preview("LSX4 preview locale test");
                Button localSend=awaitButton(localized,R.string.report_send);
                check(actions[locale].equals(localSend.getText().toString()),"localized action "+languages[locale]);
                final String[] localizedText=new String[1];
                runOnMainSync(()->localizedText[0]=screen(localized.getWindow().getDecorView()));
                check(localizedText[0].contains(ratings[locale]),"localized rating "+languages[locale]);
                save("report-locale-"+languages[locale]+"-cancelled.json",snapshot(localized).toString());
                save("preview-locale-"+languages[locale]+".txt",localizedText[0]);
                finishActivity(localized);
            }
            context.getSharedPreferences("lsx4_settings",0).edit().putString("app_language","en").commit();
            repository.clear(id);
            evidence.put("six_languages_preview",true);
            context.getSharedPreferences("lsx4_compatibility_report_draft",0).edit().clear().commit();
            evidence.put("passed",true);
            save("compatibility-smoke-result.json",evidence.toString(2));
            result.putString("result",evidence.toString());
            finish(Activity.RESULT_OK,result);
        } catch (Throwable failure) {
            result.putString("failure",failure.toString());
            try { evidence.put("passed",false).put("error",failure.toString()); save("compatibility-smoke-result.json",evidence.toString(2)); }
            catch(Exception ignored) { }
            finish(Activity.RESULT_CANCELED,result);
        }
    }

    private static final class FailureConnection extends HttpsURLConnection {
        private final boolean timeout;
        FailureConnection(URL url,boolean timeout){super(url);this.timeout=timeout;}
        @Override public java.io.OutputStream getOutputStream() throws java.io.IOException {
            if(!"LSX4-CompatibilityReports/1".equals(getRequestProperty("User-Agent")))
                throw new AssertionError("default device user-agent leaked");
            if(timeout) throw new SocketTimeoutException("simulated timeout");
            throw new java.net.ConnectException("simulated offline");
        }
        @Override public void connect(){} @Override public void disconnect(){}
        @Override public boolean usingProxy(){return false;}
        @Override public String getCipherSuite(){return "test";}
        @Override public Certificate[] getLocalCertificates(){return null;}
        @Override public Certificate[] getServerCertificates(){return null;}
    }
}
