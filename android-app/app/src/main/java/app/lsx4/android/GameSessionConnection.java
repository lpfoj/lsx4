package app.lsx4.android;

import android.app.Activity;
import android.content.ContentProviderClient;
import android.os.Binder;
import android.os.Bundle;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;


final class GameSessionConnection {
    private final Activity activity;
    private final ContentProviderClient broker;
    private final Binder callback;
    private final long token;

    GameSessionConnection(Activity activity, Runnable stop) {
        this.activity = activity;
        broker = activity.getContentResolver().acquireContentProviderClient(
                GameSessionProvider.uri(activity));
        if (broker == null) throw new IllegalStateException("Missing game session provider");
        callback = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                if (code == GameSessionProvider.STOP) {
                    if (Binder.getCallingUid() != android.os.Process.myUid()) throw new SecurityException();
                    data.enforceInterface(GameSessionProvider.DESCRIPTOR);
                    activity.runOnUiThread(stop);
                    return true;
                }
                return super.onTransact(code, data, reply, flags);
            }
        };
        Bundle registration = new Bundle();
        registration.putBinder("session", callback);
        registration.putLong(GameSessionProvider.TOKEN,
                activity.getIntent().getLongExtra(GameSessionProvider.TOKEN, 0));
        try {
            Bundle result = broker.call("register", null, registration);
            token = result == null ? 0 : result.getLong(GameSessionProvider.TOKEN);
            if (token == 0) throw new IllegalStateException("Game process already owns a session");
        } catch (RemoteException | RuntimeException failure) {
            broker.close();
            throw new IllegalStateException("Game session registration failed", failure);
        }
    }

    void exitToLibrary() {
        try {


            activity.startActivity(GameSessionProvider.foregroundExitIntent(activity, token, callback));
        } catch (RuntimeException launchFailure) {
            Log.w("LSX4Session", "Could not foreground library; using broker exit", launchFailure);
            Bundle args = new Bundle();
            args.putLong(GameSessionProvider.TOKEN, token);
            args.putBinder("session", callback);
            try { broker.call("exit", null, args); }
            catch (RemoteException failure) { throw new IllegalStateException(failure); }
        }
    }
}
