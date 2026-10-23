#!/usr/bin/env python3
"""Export the current Android GameDB/preferences using its original Java writer.

Requires a local JDK, not an Android SDK. Only CPU preferences enter the export.
Auto host policy stays unresolved until the common native parser runs on iPhone.
"""
import argparse, hashlib, json, subprocess, tempfile, time, shutil
import xml.etree.ElementTree as ET
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'android-app/app/src/main/java/app/lsx4/android/CpuTuningSettings.java'
ADAPTER=Path(__file__).with_name('AndroidCpuProfiles')
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--jdk',type=Path,required=True)
    p.add_argument('--preferences',type=Path,required=True)
    p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    titles=['global']+sorted({g['id'] for g in json.loads(a.catalog.read_text())})
    with tempfile.TemporaryDirectory(dir=a.output,prefix='.export-') as temp:
        stage=Path(temp);classes=stage/'classes';classes.mkdir()
        subprocess.run([str(a.jdk/'bin/javac'),'-d',str(classes),str(SOURCE),
            *map(str,sorted(ADAPTER.rglob('*.java')))],check=True)
        generated=stage/'profiles'
        subprocess.run([str(a.jdk/'bin/java'),'-cp',str(classes),
            'app.lsx4.android.ExportCpuProfiles',str(a.preferences.resolve()),str(generated),*titles],check=True)
        files={title:sha(generated/(title+'.ini')) for title in titles}
        manifest={'version':1,'generatedAt':time.time(),'resolverSource':str(SOURCE),
            'resolverSha256':sha(SOURCE),'preferencesSha256':sha(a.preferences),
            'androidRequestedBackend':next((int(e.get('value')) for e in ET.parse(a.preferences).getroot()
                if e.get('name')=='managed_guest_cpu_backend'),None),
            'automaticPolicy':'resolved by shared native code on destination hardware',
            'profiles':files}
        for title in titles: shutil.copyfile(generated/(title+'.ini'),a.output/(title+'.ini'))
        temporary=a.output/'manifest.json.tmp';temporary.write_text(json.dumps(manifest,indent=2)+'\n')
        temporary.replace(a.output/'manifest.json')
    print(json.dumps({'profiles':len(titles),'resolverSha256':sha(SOURCE),'output':str(a.output)}))
if __name__=='__main__':main()
