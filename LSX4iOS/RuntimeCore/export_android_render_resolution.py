#!/usr/bin/env python3
"""Execute original Android presentation resolvers and INI writer off-device.

Exports source/output/upscaler configuration. Does not install or change either
platform's live configuration. Native render-surface integration is separate.
"""
import argparse,hashlib,json,re,subprocess,tempfile,xml.etree.ElementTree as ET
from pathlib import Path
from export_android_managed_launch import JAVA,ROOT,generate_java,method,constant_closure,sha
from android_display_scaling import native_header


def generate_render_java():
    java=generate_java();settings=(JAVA/'SettingsActivity.java').read_text();main=(JAVA/'MainActivity.java').read_text()
    writer=method(main,'private void materializeRenderResolutionSetting(')
    resolvers='\n'.join(method(main,sig)for sig in ('private static int resolveRenderResolutionMode(',
        'private static int resolveUpscaledOutputResolutionMode(','private static int resolveUpscalerMode(',
        'private static int percentPreference(','private int effectiveOutputResolutionMode(',
        'private static int resolveDisplayScalingMode('))
    sanitizers='\n'.join(method(settings,sig)for sig in ('static int sanitizeUpscalerMode(',
        'public static int sanitizeUpscaledOutputMode('))
    refs=set(re.findall(r'SettingsActivity\.([A-Z][A-Z_0-9]+)',writer+resolvers))
    refs.update(re.findall(r'\b(?:UPSCALER_|DEFAULT_)[A-Z_0-9]+\b',sanitizers))
    constants=constant_closure(settings,refs)
    existing=set(re.findall(r'static final (?:String|int(?:\[\])?)\s+(\w+)\s*=',java))
    constants='\n'.join(line for line in constants.splitlines()if re.search(r'\b(\w+)=',line).group(1)not in existing)
    java=java.replace('class SettingsActivity {', (JAVA/'DisplayScaling.java').read_text().replace('package app.lsx4.android;','',1)+'\nclass SettingsActivity {',1)
    java=java.replace('class SettingsActivity {','class SettingsActivity {\n'+constants+'\n'+sanitizers,1)
    # Preserve original per-title key routing for every newly read render setting.
    keys=set(re.findall(r'SettingsActivity\.(K_\w+)',writer+resolvers))
    individual=method((JAVA/'GameProfileSettings.java').read_text(),'static boolean isIndividualKey(')
    for key in keys:
        if 'case SettingsActivity.'+key+':'not in individual:raise RuntimeError('Render key not title-scoped: '+key)
    match=re.search(r'return Set.of\(([^;]+)\).contains\(key\);',java);allowed=set(match.group(1).split(','));allowed.update('SettingsActivity.'+k for k in keys)
    java=java[:match.start(1)]+','.join(sorted(allowed))+java[match.end(1):]
    extras=constant_closure(main,set(re.findall(r'\bEXTRA_[A-Z_0-9]+\b',resolvers))|{'RENDER_RESOLUTION_CONFIG'})
    fields='''
    int renderResolutionMode,upscaledOutputResolutionMode,upscalerMode;
    void append(String ignored) {}
    void writeSmallFile(File file,String content) throws Exception {
        Files.writeString(file.toPath(),content);
    }
'''
    java=java.replace('import java.nio.file.*;','import java.nio.file.*;\nimport java.io.File;',1)
    java=java.replace('class Intent {','class Intent {\n int getIntExtra(String k,int d){throw new AssertionError();}',1)
    java=java.replace('public class ExportManagedLaunch {','public class ExportManagedLaunch {\n'+extras+'\n'+fields+'\n'+writer+'\n'+resolvers,1)
    needle='RuntimeBridge.calls.clear();runner.applyManagedOptimizations();'
    replacement='''RuntimeBridge.calls.clear();runner.applyManagedOptimizations();
            runner.renderResolutionMode=resolveRenderResolutionMode(null,runner.preferences);
            runner.upscaledOutputResolutionMode=resolveUpscaledOutputResolutionMode(null,runner.preferences,runner.renderResolutionMode);
            runner.upscalerMode=resolveUpscalerMode(null,runner.preferences);
            File output=new File(System.getProperty("lsx4.export.root"),title);
            runner.materializeRenderResolutionSetting(output);
            Files.writeString(output.toPath().resolve("surface.json"),"{\\"outputWidth\\":"+SettingsActivity.resolutionWidth(runner.effectiveOutputResolutionMode())+",\\"outputHeight\\":"+SettingsActivity.resolutionHeight(runner.effectiveOutputResolutionMode())+",\\\"displayScalingMode\\\":"+resolveDisplayScalingMode(runner.preferences)+"}");'''
    assert java.count(needle)==1;return java.replace(needle,replacement,1)


def native_profiles(profiles):
    rows=[]
    for p in profiles:
        rows.append('{'+','.join([json.dumps(p['title']),str(p['outputWidth']),str(p['outputHeight']),str(p['displayScalingMode']),json.dumps(p['ini'])])+'}')
    return r'''#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "AndroidDisplayScaling.h"
namespace AndroidRenderLaunch {
struct Profile { const char* title; int outputWidth,outputHeight,displayMode; const char* ini; };
inline constexpr Profile profiles[]={__ROWS__};
inline bool Enabled(){const char* p=std::getenv("LSX4_APPLE_RENDER_LAUNCH");return p&&std::strcmp(p,"on")==0;}
inline const Profile& Find(const char* title){
 for(const auto& p:profiles)if(std::strcmp(p.title,title)==0)return p;
 if(std::strncmp(title,"PPSA",4)==0)return Find("PPSA00000");
 return profiles[0];
}
inline const Profile& Current(){
 const char* game=std::getenv("LSX4_CORE_GAME");
 const auto title=game?std::filesystem::path(game).parent_path().filename().string():std::string{};
 return Find(title.c_str());
}
inline bool WriteBeforeCpu(const std::filesystem::path& root,const char* title){
 if(!Enabled())return true;
 const auto& p=Find(title);std::error_code error;
 const auto directory=root/"runtime-config";std::filesystem::create_directories(directory,error);
 if(error)return false;
 const auto target=directory/"android-render-resolution.ini",pending=directory/"android-render-resolution.ini.pending";
 {std::ofstream output(pending,std::ios::trunc|std::ios::binary);output<<p.ini;output.close();if(!output)return false;}
 std::filesystem::rename(pending,target,error);if(error)return false;
 std::fprintf(stderr,"[APPLE_ANDROID_RENDER_LAUNCH] profile=%s output=%dx%d displayMode=%d\n",p.title,p.outputWidth,p.outputHeight,p.displayMode);
 return true;
}
}
'''.replace('__ROWS__',','.join(rows))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--preferences',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--java-home',type=Path,required=True);p.add_argument('--titles',nargs='*',default=[]);a=p.parse_args()
    if a.output.exists():raise RuntimeError('Refusing to overwrite export directory')
    titles=['global','PPSA00000',*a.titles]
    for node in ET.parse(a.preferences).getroot():
        match=re.match(r'game\.profile\.((?:CUSA|PPSA)\d{5})\.',node.attrib.get('name',''))
        if match:titles.append(match.group(1))
    titles=list(dict.fromkeys(titles));assert all(t=='global'or re.fullmatch(r'(CUSA|PPSA)\d{5}',t)for t in titles)
    java=generate_render_java()
    with tempfile.TemporaryDirectory()as directory:
        temp=Path(directory);source=temp/'ExportManagedLaunch.java';source.write_text(java);out=temp/'render'
        subprocess.run([str(a.java_home/'bin/javac'),'-d',str(temp),str(source)],check=True)
        subprocess.run([str(a.java_home/'bin/java'),'-Dlsx4.export.root='+str(out),'-cp',str(temp),'ExportManagedLaunch',str(a.preferences.resolve()),*titles],check=True,capture_output=True,text=True)
        profiles=[]
        for title in titles:
            ini=(out/title/'runtime-config/android-render-resolution.ini').read_text();surface=json.loads((out/title/'surface.json').read_text())
            values=dict(line.split('=',1)for line in ini.splitlines()if'='in line)
            assert int(values['version'])==4 and surface['outputWidth']==int(values['output_width']) and surface['outputHeight']==int(values['output_height'])
            profiles.append({'title':title,'ini':ini,**surface,'sourceWidth':int(values['source_width']),'sourceHeight':int(values['source_height'])})
        a.output.mkdir(parents=True);(a.output/'ExportManagedLaunch.java').write_text(java)
        (a.output/'profiles.json').write_text(json.dumps(profiles,indent=2)+'\n')
        geometry=a.output/'AndroidDisplayScaling.h';geometry.write_text(native_header((JAVA/'DisplayScaling.java').read_text()))
        native=a.output/'AndroidRenderLaunch.h';native.write_text(native_profiles(profiles))
        inputs={str((JAVA/(name+'.java')).relative_to(ROOT)):sha(JAVA/(name+'.java'))for name in ['MainActivity','SettingsActivity','RuntimeBridge','GameProfileSettings','DisplayScaling']}
        (a.output/'manifest.json').write_text(json.dumps({'version':1,'sources':inputs,'preferencesSha256':sha(a.preferences),'profilesSha256':sha(a.output/'profiles.json'),'displayScalingHeaderSha256':sha(geometry),'renderLaunchHeaderSha256':sha(native),'nativeIntegrationImplemented':False},indent=2)+'\n')
        print(json.dumps([{'title':x['title'],'source':[x['sourceWidth'],x['sourceHeight']],'output':[x['outputWidth'],x['outputHeight']]}for x in profiles]))
if __name__=='__main__':main()
