package app.lsx4.android;

import android.app.ActivityManager;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;


public final class GameSessionProvider extends ContentProvider {
    static final String TOKEN = "lsx4_game_session_token";
    private static final String FOREGROUND_EXIT = "lsx4_foreground_library_exit";
    private static final String LIBRARY_ALREADY_VISIBLE = "lsx4_library_already_visible";
    static final String DESCRIPTOR = "app.lsx4.android.GameSession";
    static final int STOP = IBinder.FIRST_CALL_TRANSACTION;
    private final Handler main = new Handler(Looper.getMainLooper());
    private IBinder session;
    private int sessionPid;
    private long sessionToken;
    private GameSessionTransitions<Intent> transitions;

    static Uri uri(Context context) {
        return Uri.parse("content://" + context.getPackageName() + ".game-session");
    }

    static void launch(Context context, Intent target) {
        Bundle args = new Bundle();
        args.putParcelable("target", target);
        context.getContentResolver().call(uri(context), "launch", null, args);
    }

    static boolean resumeExisting(Context context) {
        Bundle result = context.getContentResolver().call(uri(context), "resume", null, null);
        return result != null && result.getBoolean("resumed");
    }

    static Intent foregroundExitIntent(Context context, long token, IBinder owner) {
        Bundle request = new Bundle();
        request.putLong(TOKEN, token);
        request.putBinder("session", owner);
        return libraryIntent(context).putExtra(FOREGROUND_EXIT, request);
    }

    static boolean consumeForegroundExit(Context context, Intent intent) {
        if (intent == null) return false;
        try {
            Bundle request = intent.getBundleExtra(FOREGROUND_EXIT);
            intent.removeExtra(FOREGROUND_EXIT);
            if (request == null) return false;
            long token = request.getLong(TOKEN);
            IBinder owner = request.getBinder("session");
            if (token == 0 || owner == null) return false;
            Bundle args = new Bundle();
            args.putLong(TOKEN, token);
            args.putBinder("session", owner);
            Bundle result = context.getContentResolver().call(uri(context), "exit_foreground", null, args);
            return result != null && result.getBoolean("accepted");
        } catch (RuntimeException invalidRequest) {
            intent.removeExtra(FOREGROUND_EXIT);
            Log.w("LSX4Session", "Could not accept foreground library exit", invalidRequest);
            return false;
        }
    }

    @Override public boolean onCreate() {
        transitions = new GameSessionTransitions<>(new GameSessionTransitions.Host<Intent>() {
            @Override public void launch(Intent target, long token) {
                if (token == 0 && target.getBooleanExtra(LIBRARY_ALREADY_VISIBLE, false)) {
                    Log.i("LSX4Session", "foreground-library exit complete");
                    return;
                }
                Intent intent = new Intent(target);
                intent.setClass(getContext(), token == 0 ? MainActivity.class : GameActivity.class);
                intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
                intent.putExtra(TOKEN, token);
                main.post(() -> {
                    try {
                        getContext().startActivity(intent);
                        Log.i("LSX4Session", "launch token=" + token);
                        if (token != 0) main.postDelayed(() -> expireUnregisteredLaunch(token), 30000);
                    } catch (RuntimeException error) {
                        Log.e("LSX4Session", "Task handoff failed", error);
                        transitions.died(token);
                        if (token != 0) getContext().startActivity(libraryIntent(getContext()));
                    }
                });
            }
            @Override public void stop(long token) { main.post(() -> stopSession(token)); }
        });
        return true;
    }

    private static Intent libraryIntent(Context context) {
        return new Intent(context, MainActivity.class).setAction(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
    }

    @Override public synchronized Bundle call(String method, String arg, Bundle extras) {
        if (Binder.getCallingUid() != android.os.Process.myUid()) throw new SecurityException();
        Bundle result = new Bundle();
        if ("register".equals(method)) {
            IBinder binder = extras == null ? null : extras.getBinder("session");
            if (binder == null || !binder.isBinderAlive()) return result;
            long token = transitions.register(extras.getLong(TOKEN));
            if (token == 0) return result;
            session = binder;
            sessionPid = Binder.getCallingPid();
            sessionToken = token;
            try {
                binder.linkToDeath(() -> main.post(() -> sessionDied(binder, token)), 0);
            } catch (RemoteException alreadyDead) {
                main.post(() -> sessionDied(binder, token));
            }
            result.putLong(TOKEN, token);
        } else if ("launch".equals(method)) {
            Intent target = extras == null ? null : extras.getParcelable("target");
            if (target == null) throw new IllegalArgumentException("Missing game intent");
            transitions.request(new Intent(target), true);
        } else if ("resume".equals(method)) {


            if (transitions.canResume() && session != null && session.isBinderAlive()) {
                ActivityManager manager = (ActivityManager) getContext().getSystemService(Context.ACTIVITY_SERVICE);
                try {
                    for (ActivityManager.AppTask task : manager.getAppTasks()) {
                        ActivityManager.RecentTaskInfo info = task.getTaskInfo();
                        if (info.baseActivity != null
                                && getContext().getPackageName().equals(info.baseActivity.getPackageName())
                                && GameActivity.class.getName().equals(info.baseActivity.getClassName())) {
                            task.moveToFront();
                            result.putBoolean("resumed", true);
                            break;
                        }
                    }
                } catch (RuntimeException retiredTask) {
                    Log.w("LSX4Session", "Existing task ended before it could be resumed", retiredTask);
                }
            }
        } else if ("exit".equals(method) || "exit_foreground".equals(method)) {
            long token = extras == null ? 0 : extras.getLong(TOKEN);
            IBinder owner = extras == null ? null : extras.getBinder("session");
            if (owner != null && owner == session && sessionToken == token
                    && owner.isBinderAlive() && transitions.owns(token)) {


                if (transitions.canResume()) {
                    Intent library = libraryIntent(getContext());
                    if ("exit_foreground".equals(method)) {
                        library.putExtra(LIBRARY_ALREADY_VISIBLE, true);
                    }
                    transitions.request(library, false);
                }
                result.putBoolean("accepted", true);
            }
        } else throw new IllegalArgumentException("Unknown session operation");
        return result;
    }

    private synchronized void stopSession(long token) {
        if (session == null || sessionToken != token || !transitions.owns(token)) return;
        final IBinder retiring = session;
        final int retiringPid = sessionPid;
        Parcel data = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            retiring.transact(STOP, data, null, IBinder.FLAG_ONEWAY);
        } catch (RemoteException dead) {
            sessionDied(retiring, token);
        } finally { data.recycle(); }

        main.postDelayed(() -> {
            synchronized (GameSessionProvider.this) {
                if (session != retiring || sessionToken != token || !retiring.isBinderAlive()) return;
                Log.e("LSX4Session", "Guest shutdown deadline, token=" + token);
                if (retiringPid != android.os.Process.myPid()) android.os.Process.killProcess(retiringPid);
            }
        }, 12000);
    }

    private synchronized void sessionDied(IBinder dead, long token) {
        if (session != dead || sessionToken != token) return;
        session = null;
        sessionPid = 0;
        sessionToken = 0;
        Log.i("LSX4Session", "process-dead token=" + token);
        transitions.died(token);
    }

    private synchronized void expireUnregisteredLaunch(long token) {
        if (!transitions.waitingForRegistration(token)) return;


        ActivityManager manager = (ActivityManager) getContext().getSystemService(Context.ACTIVITY_SERVICE);
        java.util.List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        boolean found = false;
        if (processes != null) for (ActivityManager.RunningAppProcessInfo process : processes) {
            if (process.uid == android.os.Process.myUid()
                    && (getContext().getPackageName() + ":game").equals(process.processName)) {
                found = true;
                android.os.Process.killProcess(process.pid);
            }
        }
        if (found) {
            main.postDelayed(() -> expireUnregisteredLaunch(token), 250);
        } else {
            Log.e("LSX4Session", "Launch ended before registration, token=" + token);
            transitions.died(token);
            if (transitions.activeToken() == 0) getContext().startActivity(libraryIntent(getContext()));
        }
    }

    @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) { return null; }
    @Override public String getType(Uri u) { return null; }
    @Override public Uri insert(Uri u, ContentValues v) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri u, String s, String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
}
