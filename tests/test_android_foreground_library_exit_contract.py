"""Execute actual foreground-exit methods with deterministic Android IPC/Activity doubles."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = "android-app/app/src/main/java/app/lsx4/android/"

def block(text,marker):
 start=text.index(marker);brace=text.index('{',start);level=1;end=brace+1
 while level:
  level+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def body(text,marker):
 b=block(text,marker);return b[b.index('{')+1:-1]

class AndroidForegroundLibraryExitContract(unittest.TestCase):
    def test_actual_foreground_exit_capability_and_retirement_order(self):
        javac = shutil.which("javac")
        java = shutil.which("java")
        for candidate in [os.environ.get("JAVA_HOME", ""), "C:/Program Files/Android/Android Studio/jbr"]:
            if not candidate:
                continue
            suffix = ".exe" if os.name == "nt" else ""
            compiler = Path(candidate) / "bin" / ("javac" + suffix)
            runner = Path(candidate) / "bin" / ("java" + suffix)
            if compiler.is_file() and runner.is_file():
                javac, java = str(compiler), str(runner)
                break
        if not javac or not java:
            self.skipTest("JDK is required for the actual Java control-flow fixture")
        root, base = ROOT, BASE
        provider=(root/(base+'GameSessionProvider.java')).read_text(encoding='utf8')
        connection=(root/(base+'GameSessionConnection.java')).read_text(encoding='utf8')
        main=(root/(base+'MainActivity.java')).read_text(encoding='utf8')
        transitions=(root/(base+'GameSessionTransitions.java')).read_text(encoding='utf8').replace('package app.lsx4.android;','')
        harness=r"""
        import java.util.*;
        public class ExitHarness {
         static final List<Intent> launches=new ArrayList<>();
         static class RemoteException extends Exception {}
         interface IBinder { boolean isBinderAlive(); }
         static class Owner implements IBinder { boolean alive=true;public boolean isBinderAlive(){return alive;} }
         static class Bundle {
          final Map<String,Object> map=new HashMap<>();
          void putLong(String k,long v){map.put(k,v);}long getLong(String k){return map.containsKey(k)?(Long)map.get(k):0;}
          void putBinder(String k,IBinder v){map.put(k,v);}IBinder getBinder(String k){return (IBinder)map.get(k);}
          void putBoolean(String k,boolean v){map.put(k,v);}boolean getBoolean(String k){return Boolean.TRUE.equals(map.get(k));}
         }
         static class Intent {
          static final String ACTION_MAIN="main",CATEGORY_LAUNCHER="launcher";
          static final int FLAG_ACTIVITY_NEW_TASK=1,FLAG_ACTIVITY_SINGLE_TOP=2;
          final Map<String,Object> extras=new HashMap<>();String label="library";
          Intent(Context c,Class<?> kind){} Intent(Intent other){extras.putAll(other.extras);label=other.label;}
          Intent setAction(String s){return this;}Intent addCategory(String s){return this;}Intent addFlags(int x){return this;}
          Intent setFlags(int f){return this;}Intent setClass(Context c,Class<?> kind){return this;}
          Intent putExtra(String k,Object v){extras.put(k,v);return this;}
          Bundle getBundleExtra(String k){return (Bundle)extras.get(k);}void removeExtra(String k){extras.remove(k);}
          boolean getBooleanExtra(String k,boolean d){return extras.containsKey(k)?(Boolean)extras.get(k):d;}
         }
         static class MainActivity {}static class GameActivity {}
         static class Log {static void i(String a,String b){}static void w(String a,String b,Throwable e){}static void e(String a,String b,Throwable e){}}
         static class Handler {void post(Runnable r){r.run();}void postDelayed(Runnable r,long delay){}}
         static class Context {
          GameSessionProvider provider;boolean failLaunch;
          void startActivity(Intent i){if(failLaunch)throw new IllegalStateException("launch rejected");launches.add(i);}
          Resolver getContentResolver(){return new Resolver(provider);}
         }
         static class Activity extends Context {}
         static class Resolver {
          final GameSessionProvider provider;Resolver(GameSessionProvider p){provider=p;}
          Bundle call(String uri,String method,String arg,Bundle extras){return provider.dispatch(method,extras);}
         }
         static class Broker {
          final GameSessionProvider provider;Broker(GameSessionProvider p){provider=p;}
          Bundle call(String method,String arg,Bundle extras)throws RemoteException{return provider.dispatch(method,extras);}
         }
         static class GameSessionProvider {
          static final String TOKEN="lsx4_game_session_token";
          static final String FOREGROUND_EXIT="lsx4_foreground_library_exit";
          static final String LIBRARY_ALREADY_VISIBLE="lsx4_library_already_visible";
          final Context context=new Context();final Handler main=new Handler();
          final GameSessionTransitions<Intent> transitions;
          IBinder session;long sessionToken;int stops,calls;
          GameSessionProvider(){context.provider=this;transitions=new GameSessionTransitions<>(new GameSessionTransitions.Host<Intent>(){
            public void launch(Intent target,long token){LAUNCH_BODY}
            public void stop(long token){stops++;}
          });}
          Context getContext(){return context;}static String uri(Context c){return "provider";}
          void expireUnregisteredLaunch(long token){}
          FOREGROUND_INTENT_METHOD
          CONSUME_METHOD
          LIBRARY_INTENT_METHOD
          Bundle dispatch(String method,Bundle extras){calls++;Bundle result=new Bundle();EXIT_BODY return result;}
          void bind(long token,IBinder owner){sessionToken=transitions.register(token);require(sessionToken!=0,"registration");session=owner;}
         }
         static class Connection {
          final Activity activity;final Broker broker;final long token;final IBinder callback;
          Connection(GameSessionProvider p,Owner owner){activity=new Activity();activity.provider=p;broker=new Broker(p);token=p.sessionToken;callback=owner;}
          CONNECTION_METHOD
         }
         static void require(boolean ok,String label){if(!ok)throw new AssertionError(label);}
         static GameSessionProvider fresh(Owner owner){launches.clear();GameSessionProvider p=new GameSessionProvider();p.bind(0,owner);return p;}
         static Bundle request(long token,IBinder owner){Bundle b=new Bundle();b.putLong(GameSessionProvider.TOKEN,token);b.putBinder("session",owner);return b;}
         static Intent game(Context c,String label){Intent i=new Intent(c,GameActivity.class);i.label=label;return i;}
         public static void main(String[] args){
          int cases=0;Owner owner=new Owner();GameSessionProvider p=fresh(owner);Connection c=new Connection(p,owner);
          c.exitToLibrary();
          require(launches.size()==1,"Exit must start visible library before asking broker to retire");
          require(p.calls==0&&p.stops==0,"no stop before library receives request");cases++;
          Intent navigation=launches.get(0),duplicate=new Intent(navigation);
          require(GameSessionProvider.consumeForegroundExit(p.context,navigation),"valid capability accepted");
          require(!navigation.extras.containsKey(GameSessionProvider.FOREGROUND_EXIT),"consume removes intent capability");
          require(p.stops==1,"old game stop requested once");cases++;
          require(!GameSessionProvider.consumeForegroundExit(p.context,navigation),"same intent cannot repeat Exit");cases++;
          long old=p.sessionToken;
          p.transitions.request(game(p.context,"next-A"),true);
          p.transitions.request(game(p.context,"next-B"),true);
          require(launches.size()==1,"next game waits for old death");
          require(GameSessionProvider.consumeForegroundExit(p.context,duplicate),"late matching Exit remains idempotent");
          require(p.stops==1,"late Exit cannot issue second stop");
          p.transitions.died(old);
          require(launches.size()==2&&launches.get(1).label.equals("next-B"),"late Exit must preserve latest queued next game");cases++;
          Owner next=new Owner();p.bind(p.transitions.activeToken(),next);int before=p.stops;
          require(!p.dispatch("exit_foreground",request(old,owner)).getBoolean("accepted"),"stale generation rejected");
          require(!p.dispatch("exit_foreground",request(p.sessionToken,new Owner())).getBoolean("accepted"),"guessed current token without owner rejected");
          require(p.stops==before&&p.transitions.canResume(),"rejected request cannot tear down new game");cases++;
          Intent malformed=GameSessionProvider.foregroundExitIntent(p.context,p.sessionToken,next);
          malformed.getBundleExtra(GameSessionProvider.FOREGROUND_EXIT).map.put(GameSessionProvider.TOKEN,"not a long");
          require(!GameSessionProvider.consumeForegroundExit(p.context,malformed),"malformed request rejected");
          require(!malformed.extras.containsKey(GameSessionProvider.FOREGROUND_EXIT),"malformed extra consumed");cases++;
          owner=new Owner();p=fresh(owner);c=new Connection(p,owner);c.exitToLibrary();
          require(GameSessionProvider.consumeForegroundExit(p.context,launches.get(0)),"library accepted");
          p.transitions.died(p.sessionToken);
          require(launches.size()==1&&p.transitions.activeToken()==0,"no duplicate library launch after death");cases++;
          owner=new Owner();p=fresh(owner);c=new Connection(p,owner);c.activity.failLaunch=true;c.exitToLibrary();
          require(launches.isEmpty()&&p.stops==1,"failed Activity start uses safe broker exit");
          p.transitions.died(p.sessionToken);
          require(launches.size()==1&&p.transitions.activeToken()==0,"fallback keeps original broker library path");cases++;
          System.out.println("PASS "+cases+" executed foreground-exit cases");
         }
        }
        """
        for marker,val in {
         'LAUNCH_BODY':body(provider,'@Override public void launch(Intent target, long token)'),
         'FOREGROUND_INTENT_METHOD':block(provider,'    static Intent foregroundExitIntent('),
         'CONSUME_METHOD':block(provider,'    static boolean consumeForegroundExit('),
         'LIBRARY_INTENT_METHOD':block(provider,'    private static Intent libraryIntent('),
         'EXIT_BODY':body(provider,'} else if ("exit".equals(method) || "exit_foreground".equals(method))'),
        }.items():harness=harness.replace(marker,val)

        for mark in ['protected void onCreate(Bundle state)','protected void onNewIntent(Intent intent)']:
         actual=block(main,mark)
         assert actual.index('consumeForegroundExit')<actual.index('resumeExisting')
         assert 'if (!foregroundExit && GameSessionProvider.resumeExisting(this))' in actual
         assert 'if (foregroundExit || isLauncherHomeIntent' in actual

        with tempfile.TemporaryDirectory(prefix="lsx4-foreground-exit-") as directory:
            dest = Path(directory)
            (dest / "GameSessionTransitions.java").write_text(transitions, encoding="utf8")
            (dest / "ExitHarness.java").write_text(harness.replace(
                "CONNECTION_METHOD", block(connection, "    void exitToLibrary()")), encoding="utf8")
            compiled = subprocess.run([javac, "-encoding", "UTF-8", "-d", str(dest),
                str(dest / "ExitHarness.java"), str(dest / "GameSessionTransitions.java")],
                capture_output=True, text=True, encoding="utf8", timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            run = subprocess.run([java, "-cp", str(dest), "ExitHarness"], capture_output=True,
                text=True, encoding="utf8", timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS 8 executed foreground-exit cases", run.stdout)

if __name__ == "__main__":
    unittest.main()
