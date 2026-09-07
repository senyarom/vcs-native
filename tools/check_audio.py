#!/usr/bin/env python3
from pathlib import Path
import argparse,hashlib,json,os,re,statistics,subprocess,time,sys
parser=argparse.ArgumentParser(description='Single-game radio and audio queue check using the real device clock, with silent output.')
parser.add_argument('--name',default=time.strftime('live-%Y%m%d-%H%M%S'))
parser.add_argument('--vblanks',type=int,default=10800)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
report=root/'work/audio-latency'/args.name;report.mkdir(parents=True,exist_ok=True)
def games():
    return [int(p[0]) for line in subprocess.check_output(['ps','-axo','pid=,comm='],text=True).splitlines() if len(p:=line.split(maxsplit=1))==2 and Path(p[1]).name=='VCSNative']
def state():
    return {str(p.relative_to(root/'userdata')):hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'userdata').rglob('*') if p.is_file() and p.name!='.launch.lock'}
assert not games(),'Another game is already running'
ini=report/'test.ini'
ini.write_text((root/'userdata/VCSNative.ini').read_text()+'\n[Textures]\nDirectory='+str(root/'textures/hdtextures')+'\n[Timing]\nFrameRate=60\n[Audio]\nDiagnostics=true\n[Diagnostics]\nLogToFile=false\n')
env=os.environ.copy();env.update(PSPRECOMP_CONFIG=str(ini),PSPRECOMP_WINDOW='0',SDL_VIDEODRIVER='dummy',PSPRECOMP_AUDIO='1',PSPRECOMP_AUDIO_MUTE_DEVICE='1',PSPRECOMP_AUDIO_DIAG='1',PSPRECOMP_AUDIO_SUMMARY='1',PSPRECOMP_AUDIO_WAV=str(report/'audio.wav'),PSPRECOMP_STOP_VBLANK='10800',PSPRECOMP_GE_GPU_COLOR_PREVIEW='1',PSPRECOMP_GE_GPU_DUMP_COUNT='0')
env.pop('SDL_AUDIODRIVER',None)
env['PSPRECOMP_STOP_VBLANK']=str(args.vblanks)
for i,start in enumerate([300,700,1200,1800,2100,2400,2600,3300]):
    prefix='PSPRECOMP_CTRL_PULSE'+(str(i+1) if i else '')
    env.update({prefix+'_BUTTONS':str(0x1000 if i==7 else 0x4000),prefix+'_START_VBLANK':str(start),prefix+'_END_VBLANK':str(start+15)})
before=state();binary=root/('run/VCSNative.app/Contents/MacOS/VCSNative' if sys.platform=='darwin' else 'run/VCSNative');sha=hashlib.sha256(binary.read_bytes()).hexdigest()
start=time.monotonic()
with (report/'game.log').open('w') as log:
    p=subprocess.Popen([sys.executable,str(root/'launch.py')],cwd=report,env=env,stdout=log,stderr=subprocess.STDOUT)
    try:
        while p.poll() is None:
            assert all(pid==p.pid for pid in games()),'Another game appeared'
            assert time.monotonic()-start<300,'Timeout'
            time.sleep(1)
    finally:
        if p.poll() is None:
            p.terminate()
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
result=dict(exit_code=p.returncode,seconds=round(time.monotonic()-start,2),settings_saves_unchanged=before==state(),binary_sha256=sha,remaining_processes=games(),window=False,audible_audio=False,stop_vblank=args.vblanks)
lines=(report/'game.log').read_text().splitlines()
summaries=[];radio_gaps=[];previous_end=None;radio_buffers=0
for line in lines:
    if '[audio-summary]' in line:
        summaries.append({k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)})
    if '[audio] output2 reserve' in line or '[audio] src reserve' in line:previous_end=None
    m=re.search(r'\[audio\] output2 samples=(\d+) channels=(\d+) freq=(\d+) start_us=(\d+)',line)
    if m:
        frames,channels,rate,stamp=map(int,m.groups())
        if stamp>=60_000_000:
            radio_buffers+=1
            if previous_end is not None and stamp-previous_end>1000:radio_gaps.append((stamp-previous_end)/1000)
        previous_end=stamp+frames*1e6/rate
late=[v for v in summaries if v['guest_us']>=60_000_000]
driver=re.search(r'\[audio-host\] driver=(\S+)', '\n'.join(lines))
result.update(audio_driver=driver.group(1) if driver else None,
              radio_buffers_after_60s=radio_buffers,radio_gaps_over_1ms_after_60s=len(radio_gaps),
              largest_radio_gap_ms=max(radio_gaps or [0]),
              late_latency_ms_median=statistics.median(v['latency_ms'] for v in late) if late else None,
              late_latency_ms_max=max((v['latency_ms'] for v in late),default=None),
              late_underruns=late[-1]['underrun_rebuffers']-late[0]['underrun_rebuffers'] if late else None,
              last_audio_summary=summaries[-1] if summaries else {})
(report/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
assert p.returncode==4 and result['settings_saves_unchanged']
assert f'VBlank diagnostic stop at {args.vblanks}' in '\n'.join(lines)
assert result['audio_driver'] and result['audio_driver']!='dummy', 'Real host audio device did not open'
assert radio_buffers>1000 and late, 'Radio path was not exercised'
assert result['late_latency_ms_max']<400 and late[-1]['overrun_frames']==0, 'Audio backlog grew or overflowed'
