#!/usr/bin/env python3
"""Review aid for native viewport recordings (requires NumPy, Pillow, FFmpeg).

Analyzes every decoded frame at320x180 and selects outliers for human inspection.
It does not infer gameplay, corruption, Android parity or performance acceptance.
"""
from pathlib import Path
import argparse,json,subprocess,tempfile
import numpy as np
from PIL import Image,ImageDraw
p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('--ffmpeg',default='ffmpeg',help='FFmpeg executable path');a=p.parse_args();run=a.run.resolve();out=run/'review';out.mkdir(exist_ok=True)
ff=a.ffmpeg
video=run/'viewport-video/viewport.mp4';w,h=320,180
raw=tempfile.TemporaryFile()
proc=subprocess.run([str(ff),'-v','error','-i',str(video),'-vf',f'scale={w}:{h}','-fps_mode','passthrough','-f','rawvideo','-pix_fmt','rgb24','pipe:1'],stdout=raw,stderr=subprocess.PIPE,timeout=120)
assert proc.returncode==0,proc.stderr.decode(errors='replace')
raw.seek(0)
rows=[];frames=[];prev=None
while True:
 b=raw.read(w*h*3)
 if not b:break
 if len(b)!=w*h*3:raise RuntimeError('Truncated raw frame')
 pixels=np.frombuffer(b,np.uint8).reshape(h,w,3);q=pixels.astype(np.float32)
 delta=float(np.mean(np.abs(q-prev))) if prev is not None else 0.
 row={'frame':len(rows),'mean':float(q.mean()),'std':float(q.std()),'nearWhiteFraction':float(np.all(pixels>=245,axis=2).mean()),'nearBlackFraction':float(np.all(pixels<=10,axis=2).mean()),'adjacentMeanAbsoluteDifference':delta,'spatialGradient':float((np.abs(q[:,1:]-q[:,:-1]).mean()+np.abs(q[1:]-q[:-1]).mean())/2)}
 rows.append(row);frames.append(b);prev=q
raw.close();rc=proc.returncode
assert rows,'No decoded frames'
# Flag candidates for visual inspection, not an automated corruption verdict.
selected=set()
for key in ('nearWhiteFraction','nearBlackFraction','adjacentMeanAbsoluteDifference','spatialGradient'):
 count=0
 for row in sorted(rows,key=lambda r:r[key],reverse=True):
  if all(abs(row['frame']-x)>3 for x in selected):selected.add(row['frame']);count+=1
  if count==4 or len(selected)==16:break
selected=sorted(selected)
canvas=Image.new('RGB',(w*4,(h+24)*4),'#151515');draw=ImageDraw.Draw(canvas)
for tile,idx in enumerate(selected):
 x,y=(tile%4)*w,(tile//4)*(h+24);canvas.paste(Image.frombytes('RGB',(w,h),frames[idx]),(x,y));row=rows[idx]
 draw.text((x+4,y+h+4),f"frame {idx} | W {row['nearWhiteFraction']:.3f} | delta {row['adjacentMeanAbsoluteDifference']:.1f}",fill='white')
canvas.save(out/'temporal-candidates.png')
summary={'runId':json.loads((run/'probe-launch.json').read_text())['runId'],'decoderExitCode':rc,'framesAnalyzed':len(rows),'analysisResolution':[w,h],'method':'All decoded frames; colour fractions, adjacent MAE and spatial gradient at320x180. Candidate ranking only, not automated correctness acceptance. Original video retained.','reviewRequired':True,'candidateFrameIndices':selected,'exactAdjacentDuplicates':sum(r['adjacentMeanAbsoluteDifference']==0 for r in rows[1:]),'minimumMean':min(r['mean']for r in rows),'maximumMean':max(r['mean']for r in rows),'maximumNearWhiteFraction':max(r['nearWhiteFraction']for r in rows),'maximumAdjacentDifference':max(r['adjacentMeanAbsoluteDifference']for r in rows),'automaticCorruptionVerdict':None,'fpsAcceptanceEligible':False}
(out/'temporal-analysis.json').write_text(json.dumps({'summary':summary,'frames':rows},indent=2)+'\n');print(json.dumps(summary))
