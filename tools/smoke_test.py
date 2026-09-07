#!/usr/bin/env python3
"""Bounded single-process gameplay check; optional visible window and EBOOT-free resource root."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1],
                    help='Checkout or packaged Linux game directory to verify.')
parser.add_argument('--visible',action='store_true',help='Show one window; audio remains muted.')
parser.add_argument('--without-eboot',action='store_true',help='Expose USRDIR resources without SYSDIR/EBOOT.')
parser.add_argument('--report',type=Path,help='Persistent diagnostic output directory.')
args=parser.parse_args()
root=args.root.resolve()
report=args.report.resolve() if args.report else root/'work/smoke'
report.mkdir(parents=True, exist_ok=True)
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def games():
    lines=subprocess.check_output(['ps','-axo','pid=,comm='],text=True).splitlines()
    return [int(fields[0]) for line in lines if len(fields:=line.strip().split(maxsplit=1))==2 and Path(fields[1]).name=='VCSNative']
assert not games(), 'A VCSNative process is already running; smoke test not started'
settings_before=digest(root/'userdata/VCSNative.ini')
saves_before={str(p.relative_to(root/'userdata/SAVEDATA')):digest(p) for p in (root/'userdata/SAVEDATA').rglob('*') if p.is_file()}
# Test the player's actual configuration instead of requiring HD to be on.
# The game treats section and option names case-insensitively, with last value winning.
config_path=Path(os.environ.get('PSPRECOMP_CONFIG',root/'userdata/VCSNative.ini'))
config_section=''
hd_enabled=True
hd_directory=root/'textures/hdtextures'
for line in config_path.read_text().splitlines():
    line=line.split('#',1)[0].split(';',1)[0].strip()
    if line.startswith('[') and line.endswith(']'):config_section=line[1:-1].strip().lower()
    elif config_section=='textures' and '=' in line:
        key,value=map(str.strip,line.split('=',1))
        if key.lower()=='enabled':hd_enabled=value.lower() in ('true','yes','on','1')
        elif key.lower()=='directory':hd_directory=(config_path.parent/Path(value).expanduser()).resolve()
section=''
manifest_keys=set()
for line in (root/'textures/hdtextures/textures.ini').read_text().splitlines():
    line=line.split('#',1)[0].split(';',1)[0].strip()
    if line.startswith('['):section=line.lower()
    elif section=='[hashes]' and '=' in line:manifest_keys.add(line.split('=',1)[0].strip())
assert manifest_keys, 'HD texture manifest is empty'
env=os.environ.copy()
env.update(PSPRECOMP_WINDOW='0',PSPRECOMP_AUDIO='1',SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy',
           PSPRECOMP_STOP_VBLANK='3600',PSPRECOMP_GE_GPU_COLOR_PREVIEW='1',
           PSPRECOMP_GE_GPU_DUMP_VBLANK='3300',PSPRECOMP_GE_GPU_DUMP_COUNT='1',
           PSPRECOMP_GE_GPU_DUMP_PATH=str(report/'standalone-frame.ppm'))
if args.visible:
    env['PSPRECOMP_WINDOW']='1'
    env.pop('SDL_VIDEODRIVER',None)
    visible_config=report/'visible.ini'
    visible_config.write_text(config_path.read_text()+'\n[Display]\nEnabled=true\nFullscreen=false\n'
                              'ResolutionMode=Custom\nWidth=960\nHeight=544\n'
                              '[Textures]\nDirectory='+str(hd_directory)+'\n')
    env['PSPRECOMP_CONFIG']=str(visible_config)
launch=['python3',str(root/'launch.py')]
if args.without_eboot:
    resource_root=report/'resources-only'
    psp=resource_root/'PSP_GAME'
    psp.mkdir(parents=True,exist_ok=True)
    usrdir=psp/'USRDIR'
    if not usrdir.exists(): usrdir.symlink_to(root/'assets/game/PSP_GAME/USRDIR',target_is_directory=True)
    assert not (psp/'SYSDIR').exists(), 'Diagnostic root must not contain EBOOT/SYSDIR'
    launch.append(str(resource_root))
for index,start in enumerate([300,700,1200,1800,2100,2400,2700,3000]):
    prefix='PSPRECOMP_CTRL_PULSE'+(str(index+1) if index else '')
    env.update({prefix+'_BUTTONS':'16384',prefix+'_START_VBLANK':str(start),prefix+'_END_VBLANK':str(start+15)})
start=time.monotonic()
with (report/'smoke.log').open('w') as log:
    process=subprocess.Popen(launch,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
    try:
        while process.poll() is None:
            assert not [pid for pid in games() if pid!=process.pid], 'Another game appeared; closing only the smoke-test process'
            assert time.monotonic()-start<180, 'Smoke test timeout'
            time.sleep(1)
    finally:
        if process.poll() is None:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill();process.wait()
saves_after={str(p.relative_to(root/'userdata/SAVEDATA')):digest(p) for p in (root/'userdata/SAVEDATA').rglob('*') if p.is_file()}
log=(report/'smoke.log').read_text()
result={'exit_code':process.returncode,'seconds':round(time.monotonic()-start,2),
        'binary_sha256':digest(root/('run/VCSNative.app/Contents/MacOS/VCSNative' if sys.platform=='darwin' else 'run/VCSNative')),
        'settings_unchanged':settings_before==digest(root/'userdata/VCSNative.ini'),
        'saves_unchanged':saves_before==saves_after,
        'hd_manifest_entries':len(manifest_keys),
        'hd_enabled_config':hd_enabled,
        'hd_manifest_loaded':f'enabled entries={len(manifest_keys)} ' in log,
        'remaining_vcs_processes':games(),'stop_vblank':3600,'window':args.visible,'audible_audio':False,
        'without_eboot':args.without_eboot,'builtin_image':'Game image: built-in ULUS10160 1.03' in log}
(report/'smoke.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
assert result['settings_unchanged'] and result['saves_unchanged']
if args.without_eboot: assert result['builtin_image'], 'Built-in image was not used'
assert result['hd_manifest_loaded'] or not hd_enabled, 'Enabled HD pack did not load'
assert process.returncode==4 and 'VBlank diagnostic stop at 3600' in log, 'Game did not reach the diagnostic stop'
