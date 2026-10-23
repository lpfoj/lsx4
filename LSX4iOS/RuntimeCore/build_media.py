#!/usr/bin/env python3
"""Build Apple's FFmpeg archives from Android's pinned source and codec inventory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

PROJECT=Path(__file__).resolve().parent.parent
REPO=PROJECT.parent
def run(args,**kwargs): subprocess.run([str(a) for a in args],check=True,**kwargs)
def tool(sdk,name): return subprocess.check_output(['xcrun','--sdk',sdk,'--find',name],text=True).strip()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',choices=['iphoneos','macosx'],default='iphoneos')
    args=parser.parse_args()
    android=(REPO/'scripts/build-ffmpeg-android-pic.ps1').read_text()
    version=re.search(r'\$version = "([^"]+)"',android).group(1)
    expected=re.search(r'\$archiveSha256 = "([A-Fa-f0-9]{64})"',android).group(1).lower()
    # Keep the complete Android codec/demuxer/parser feature set in one place.
    features=re.findall(r'--enable-(?:decoder|encoder|demuxer|muxer|parser|protocol|bsf)=[a-z0-9_]+',android)
    root=PROJECT/'.build/media-work';root.mkdir(parents=True,exist_ok=True)
    archive=root/f'ffmpeg-{version}.tar.xz'
    if not archive.exists():run(['curl','--fail','--location','--retry','3','--output',archive,
        f'https://ffmpeg.org/releases/ffmpeg-{version}.tar.xz'])
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=expected:raise RuntimeError('FFmpeg source hash differs from Android pin')
    source=root/f'ffmpeg-{version}'
    if not source.exists():run(['tar','-xf',archive,'-C',root])
    build=root/f'build-{args.sdk}';build.mkdir(exist_ok=True)
    install=PROJECT/f'.build/media-{args.sdk}'
    sdk=subprocess.check_output(['xcrun','--sdk',args.sdk,'--show-sdk-path'],text=True).strip()
    minimum='-miphoneos-version-min=17.0' if args.sdk=='iphoneos' else '-mmacosx-version-min=14.0'
    flags=f'-arch arm64 {minimum} -fPIC -fvisibility=hidden'
    command=[source/'configure',f'--prefix={install}','--target-os=darwin','--arch=aarch64',
        '--enable-cross-compile',f'--cc={tool(args.sdk,"clang")}',f'--cxx={tool(args.sdk,"clang++")}',
        f'--ar={tool(args.sdk,"ar")}',f'--nm={tool(args.sdk,"nm")}',f'--ranlib={tool(args.sdk,"ranlib")}',
        f'--strip={tool(args.sdk,"strip")}',f'--sysroot={sdk}',f'--extra-cflags={flags}',
        f'--extra-cxxflags={flags}',f'--extra-ldflags={flags}','--enable-pic',
        '--disable-shared','--enable-static','--disable-autodetect','--disable-doc',
        '--disable-programs','--disable-network','--disable-everything',
        '--enable-avcodec','--enable-avfilter','--enable-avformat','--enable-avutil',
        '--enable-swresample','--enable-swscale',*features]
    stamp=build/'apple-build-inputs.json'
    inputs={'sourceSHA256':expected,'command':[str(x) for x in command]}
    if not stamp.exists() or json.loads(stamp.read_text())!=inputs:
        run(command,cwd=build);stamp.write_text(json.dumps(inputs,indent=2)+'\n')
    run(['make','-j6'],cwd=build);run(['make','install'],cwd=build)
    print('Apple media libraries:',install)

if __name__=='__main__':main()
