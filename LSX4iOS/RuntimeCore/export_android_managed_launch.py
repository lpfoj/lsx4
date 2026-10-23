#!/usr/bin/env python3
"""Execute Android's launch-settings method in a host Java adapter.

Exports only managed switches and scene scale. Presentation/upscaler INI settings
are a separate bridge; this export does not claim to implement those settings.
The original method bodies are compiled, not translated into Python defaults.
"""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / 'android-app/app/src/main/java/app/lsx4/android'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def method(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    pos = opening + 1
    # These selected methods contain no braces in strings/comments. Refuse a
    # malformed extraction when upstream changes rather than using stale output.
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


def constant_closure(source, names):
    declarations = dict(re.findall(
        r'(?:public|private)?\s*static final (?:String|int(?:\[\])?)\s+(\w+)\s*=\s*([^;]+);', source))
    selected = {}
    pending = list(names)
    while pending:
        name = pending.pop()
        if name in selected:
            continue
        value = declarations[name]
        selected[name] = value
        pending.extend(n for n in re.findall(r'\b[A-Z][A-Z_0-9]+\b', value)
                       if n in declarations and n not in selected)
    return '\n'.join('static final ' + ('String' if value.startswith('"') else
                          'int[]' if value.startswith('{') else 'int') + ' ' + name + '=' + value + ';'
                     for name, value in declarations.items() if name in selected)


def generate_java():
    source = {name: (JAVA / (name + '.java')).read_text() for name in
              ('MainActivity', 'SettingsActivity', 'RuntimeBridge', 'GameProfileSettings')}
    main = source['MainActivity']
    managed = method(main, 'private void applyManagedOptimizations()')
    fsr = method(main, 'private boolean requestedFsrEnabled(')
    scale = method(main, 'private int sceneRenderScalePercentFor(')
    settings = source['SettingsActivity']
    height = method(settings, 'public static int resolutionHeight(')
    # Compile the real migration before constructing the title overlay.
    migration = method(settings, 'public static SharedPreferences prefs(Context context)')
    migration = migration[migration.index('if (!preferences.contains'):migration.index('if (context instanceof')]
    methods = '\n'.join(method(settings, sig) for sig in (
        'public static int sanitizeResolutionMode(', 'public static int resolutionModeToPosition(',
        'public static int resolutionModeAtPosition(', 'public static int defaultUpscaledOutputMode(',
        'public static int[] higherUpscaledOutputModes(', 'private static long resolutionPixelCount(',
        'public static int resolutionWidth('))
    refs = set(re.findall(r'SettingsActivity\.(\w+)', managed + fsr + scale))
    refs.discard('resolutionHeight')
    refs.update(('K_RES_MODE', 'DEFAULT_RES_MODE', 'RESOLUTION_HEIGHTS', 'RESOLUTION_WIDTHS', 'RESOLUTION_MODE_ORDER',
                 'K_UPSCALED_OUTPUT_RES_MODE', 'K_LEGACY_FSR_OUTPUT_RES_MODE', 'K_LEGACY_PERSISTENT_JIT_CACHE',
                 'K_PERSISTENT_JIT_CACHE'))
    # Unqualified constant references inside the original settings methods.
    refs.update(re.findall(r'\b(?:K_|RES_|DEFAULT_|RESOLUTION_)[A-Z_0-9]+\b', migration + methods))
    constants = constant_closure(settings, refs)
    keys = set(re.findall(r'SettingsActivity\.(K_\w+)', managed + fsr)) | {'K_RES_MODE'}
    individual = method(source['GameProfileSettings'], 'static boolean isIndividualKey(')
    for key in keys:
        if 'case SettingsActivity.' + key + ':' not in individual:
            raise RuntimeError('Used preference is no longer title-scoped: ' + key)
    read_key = method(source['GameProfileSettings'], 'private String readKey(')
    runtime_constants = re.findall(r'(?:public\s+)?static final int (MANAGED_OPTIMIZATION_\w+)\s*=\s*(\d+)\s*;', source['RuntimeBridge'])
    if len(runtime_constants) != 21 or {int(v) for _, v in runtime_constants} != set(range(1, 22)):
        raise RuntimeError('Managed runtime ABI changed')
    java_names = ('ARM_GPU_FAST_PATH COARSE_FRAGMENT_2X2 DISABLE_VK_ROBUSTNESS TIERED_JIT '
                  'JIT_TRACE_COMPILATION LIMIT_ANISOTROPY_2X READBACK_BATCHING VULKAN_DRIVER_CALLS '
                  'ASYNC_PIPELINE ADAPTIVE_MOBILE_GPU AUDIO_SIMD FORCE_ANISOTROPY_1X FAST_GUEST_MEMORY '
                  'RELAXED_FP_FUSION ANDROID_PERFORMANCE_HINT HOST_FLAGM HOST_SVE2 HOST_RCPC '
                  'GUEST_SPIN_WAIT_EVENT FSR RCAS').split()
    if dict(runtime_constants) != {'MANAGED_OPTIMIZATION_' + name: str(index)
                                    for index, name in enumerate(java_names, 1)}:
        raise RuntimeError('Java managed runtime ABI changed')
    abi_names = ['ArmGpuFastPath','CoarseFragmentShading','DisableVkRobustness','TieredJit',
                 'JitTraceCompilation','LimitAnisotropy2x','ReadbackBatching','VulkanDriverCalls',
                 'AsyncPipeline','AdaptiveMobileGpu','AudioSimd','ForceAnisotropy1x','FastGuestMemory',
                 'RelaxedFpFusion','AndroidPerformanceHint','HostFlagM','HostSve2','HostRcpc',
                 'GuestSpinWaitEvent','Fsr','Rcas']
    native = (ROOT / 'src/executor/native_runtime_api.cpp').read_text()
    native = method(native, 'extern "C" int executor_lsx4_runtime_set_managed_optimization(')
    for index, name in enumerate(abi_names, 1):
        if not re.search(r'constexpr int ' + name + r'\s*=\s*' + str(index) + r';', native):
            raise RuntimeError('Native managed runtime ABI changed: ' + name)
    constants_runtime = '\n'.join('static final int ' + n + '=' + v + ';' for n, v in runtime_constants)
    allowed = ','.join('SettingsActivity.' + key for key in sorted(keys))
    harness = r'''
import java.nio.file.*;
import java.util.*;
import javax.xml.parsers.DocumentBuilderFactory;
import org.w3c.dom.*;
class SharedPreferences {
    final Map<String,Object> values = new HashMap<>();
    public boolean contains(String k) { return values.containsKey(k); }
    public boolean getBoolean(String k,boolean d) { return (Boolean)values.getOrDefault(k,d); }
    public int getInt(String k,int d) { return (Integer)values.getOrDefault(k,d); }
    public SharedPreferences edit() { return this; }
    public SharedPreferences putBoolean(String k,boolean v) { values.put(k,v);return this; }
    public SharedPreferences putInt(String k,int v) { values.put(k,v);return this; }
    public void apply() {}
    public boolean commit() { return true; }
}
class Intent {
    boolean hasExtra(String k) { return false; }
    boolean getBooleanExtra(String k,boolean d) { throw new AssertionError(); }
}
class BuildConfig { static final boolean DEBUG=true; }
class Log { static void i(String tag,String message) {} }
class SettingsActivity {
__SETTINGS__
__HEIGHT__
__METHODS__
static void migrate(SharedPreferences preferences) { __MIGRATION__ }
}
class RuntimeBridge {
__RUNTIME__
static final List<String> calls=new ArrayList<>();
static int setManagedOptimization(int option,boolean enabled) {
    calls.add("["+option+","+(enabled?1:0)+"]"); return 0;
}
}
class ScopedPreferences extends SharedPreferences {
    final SharedPreferences base; final String prefix;
    ScopedPreferences(SharedPreferences b,String title) { base=b;prefix="game.profile."+title+"."; }
    static boolean isIndividualKey(String key) { return Set.of(__ALLOWED__).contains(key); }
    __READKEY__
    public boolean contains(String k) { return base.contains(readKey(k)); }
    public boolean getBoolean(String k,boolean d) { return base.getBoolean(readKey(k),d); }
    public int getInt(String k,int d) { return base.getInt(readKey(k),d); }
}
public class ExportManagedLaunch {
    static final String TAG="export", EXTRA_PROFILE_FSR_ENABLED="unused";
    __REFERENCE__
    SharedPreferences preferences; boolean ps5Runtime;
    SharedPreferences gamePreferences() { return preferences; }
    Intent getIntent() { return null; } // No debug intent overrides in a saved preference snapshot.
    __FSR__
    __SCALE__
    __MANAGED__
    public static void main(String[] args) throws Exception {
        var base=new SharedPreferences();
        var factory=DocumentBuilderFactory.newInstance();
        factory.setFeature("http://apache.org/xml/features/disallow-doctype-decl",true);
        var nodes=factory.newDocumentBuilder().parse(Path.of(args[0]).toFile()).getDocumentElement().getChildNodes();
        for(int i=0;i<nodes.getLength();i++) if(nodes.item(i) instanceof Element e) {
            String key=e.getAttribute("name");
            if(e.getTagName().equals("boolean")) base.values.put(key,Boolean.parseBoolean(e.getAttribute("value")));
            else if(e.getTagName().equals("int")) base.values.put(key,Integer.parseInt(e.getAttribute("value")));
        }
        SettingsActivity.migrate(base);
        for(int i=1;i<args.length;i++) {
            var runner=new ExportManagedLaunch(); String title=args[i];
            runner.ps5Runtime=title.startsWith("PPSA");
            runner.preferences=title.equals("global")?base:new ScopedPreferences(base,title);
            RuntimeBridge.calls.clear();runner.applyManagedOptimizations();
            int mode=SettingsActivity.sanitizeResolutionMode(runner.preferences.getInt(SettingsActivity.K_RES_MODE,SettingsActivity.DEFAULT_RES_MODE));
            System.out.println("{\"title\":\""+title+"\",\"calls\":["+String.join(",",RuntimeBridge.calls)+"],\"sceneScalePercent\":"+runner.sceneRenderScalePercentFor(mode)+"}");
        }
    }
}
'''
    replacements = dict(SETTINGS=constants, HEIGHT=height, METHODS=methods, MIGRATION=migration,
                        RUNTIME=constants_runtime, ALLOWED=allowed, READKEY=read_key, FSR=fsr,
                        SCALE=scale, MANAGED=managed,
                        REFERENCE=constant_closure(main, ['SCENE_SCALE_REFERENCE_HEIGHT']))
    for key, value in replacements.items():
        harness = harness.replace('__' + key + '__', value)
    return harness


def header(profiles, digest):
    lines = ['// Generated by executing the Android Java launch resolver. Do not edit.',
             '#pragma once', '#include <cstring>', '#include <cstdio>', '#include <cstdlib>',
             'extern "C" int executor_lsx4_runtime_set_managed_optimization(int,int);',
             'extern "C" int executor_lsx4_runtime_set_render_scale_percent(int);',
             'namespace AndroidManagedLaunch {', 'struct Call { int option,enabled; };',
             'struct Profile { const char* title; const Call* calls; unsigned count; int scale; };']
    unique = {}
    rows = []
    for profile in profiles:
        key = json.dumps(profile['calls'])
        if key not in unique:
            name = 'calls' + str(len(unique)); unique[key] = name
            lines.append('inline constexpr Call ' + name + '[]={' +
                         ','.join('{%d,%d}' % tuple(call) for call in profile['calls']) + '};')
        name = unique[key]
        rows.append('{"%s",%s,%d,%d}' % (profile['title'], name, len(profile['calls']), profile['sceneScalePercent']))
    lines += ['inline constexpr Profile profiles[]={' + ','.join(rows) + '};',
              'inline const Profile& Find(const char* title) { for(const auto& p:profiles) if(!std::strcmp(p.title,title)) return p; if(!std::strncmp(title,"PPSA",4)) return Find("PPSA00000"); return profiles[0]; }',
              'inline const char* Mode() { const char* m=std::getenv("LSX4_APPLE_MANAGED_LAUNCH"); return m?m:"options"; }',
              'inline bool ApplyBeforeCpu(const char* title) {',
              'const char* mode=Mode(); if(std::strcmp(mode,"off")==0) return true;',
              'if(std::strcmp(mode,"options") && std::strcmp(mode,"full")) return false;',
              'const auto& p=Find(title); for(unsigned i=0;i<p.count;++i) if(executor_lsx4_runtime_set_managed_optimization(p.calls[i].option,p.calls[i].enabled)<0) return false;',
              'std::fprintf(stderr,"[APPLE_ANDROID_MANAGED_LAUNCH] mode=%s profile=%s calls=%u source=' + digest + '\\n",mode,p.title,p.count); return true; }',
              'inline bool ApplyScaleBeforeGame(const char* title) {',
              'if(std::strcmp(Mode(),"full")) return true; const auto& p=Find(title);',
              'const int result=executor_lsx4_runtime_set_render_scale_percent(p.scale);',
              'std::fprintf(stderr,"[APPLE_ANDROID_SCENE_SCALE] requested=%d result=%d\\n",p.scale,result);return result>=0; }', '}']
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--preferences', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Only actual stored per-title overrides need separate rows. Unknown PS4
    # titles use the global row; an explicit PS5 default preserves its exclusions.
    titles = {'global', 'PPSA00000'}
    for entry in ET.parse(args.preferences).getroot():
        found = re.match(r'game\.profile\.((?:CUSA|PPSA)\d{5})\.', entry.get('name', ''))
        if found:
            titles.add(found[1])
    args.output.mkdir(parents=True, exist_ok=True)
    java = generate_java()
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / 'ExportManagedLaunch.java'; path.write_text(java)
        subprocess.run([str(args.jdk / 'bin/javac'), str(path)], check=True)
        result = subprocess.check_output([str(args.jdk / 'bin/java'), '-cp', temp,
                   'ExportManagedLaunch', str(args.preferences.resolve()), 'global', *sorted(titles - {'global'})], text=True)
    profiles = [json.loads(line) for line in result.splitlines()]
    inputs = [JAVA / (name + '.java') for name in ('MainActivity', 'SettingsActivity', 'RuntimeBridge', 'GameProfileSettings')]
    inputs += [ROOT / 'src/executor/native_runtime_api.cpp']
    manifest = {'version': 1, 'sources': {str(path.relative_to(ROOT)): sha(path) for path in inputs},
                'preferencesSha256': sha(args.preferences), 'profiles': profiles,
                'scope': 'managed switches and optional physical scene scale; excludes presentation/upscaler INI'}
    digest = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    generated = args.output / 'AndroidManagedLaunch.h'; generated.write_text(header(profiles, digest))
    manifest['headerSha256'] = sha(generated)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (args.output / 'ExportManagedLaunch.java').write_text(java)
    print(json.dumps({'profiles': len(profiles), 'output': str(args.output), 'headerSha256': sha(generated)}))


if __name__ == '__main__':
    main()
