#!/usr/bin/env python3
"""Deploy and verify the cross-build directly in the existing Deck game package."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SSH_OPTIONS = ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10']

# Runs in <package>/verification/cross-build. Test binaries and reports persist.
REMOTE_RUNNER = r'''
import fcntl, hashlib, json, os
from pathlib import Path
import resource, shutil, subprocess, time
stage=Path(__file__).resolve().parent
package=stage.parent.parent
request=json.loads((stage/'request.json').read_text())
build=stage/'build'; report=stage/'report'
report.mkdir(exist_ok=True)
(report/'results.json').unlink(missing_ok=True)
assert subprocess.run(['pgrep','-x','VCSNative'],capture_output=True).returncode!=0, 'VCSNative is running; nothing published'
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def snapshot():
    return {str(p.relative_to(package/'userdata')):digest(p)
            for p in (package/'userdata').rglob('*') if p.is_file() and p.name!='.launch.lock'}
before=snapshot()
assert (package/'assets/game/PSP_GAME/USRDIR/RUNDATA/MAIN.SCM').is_file(), 'Existing Deck game dump is missing'
assert (package/'assets/game/PSP_GAME/USRDIR/RUNDATA/MAINLA.IMG').is_file(), 'World catalog test data is missing'
soft,hard=resource.getrlimit(resource.RLIMIT_STACK)
desired=64*1024*1024 if hard==resource.RLIM_INFINITY else min(hard,64*1024*1024)
if soft!=resource.RLIM_INFINITY and soft<desired:resource.setrlimit(resource.RLIMIT_STACK,(desired,hard))
env=os.environ.copy()
env.update(SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',PSPRECOMP_WINDOW='0',PSPRECOMP_AUDIO='0')
def translate(value):
    for old,new in ((request['local_build'],str(build)),(request['local_root'],str(package))):
        if value==old or value.startswith(old+'/'):return new+value[len(old):]
    return value
candidate=build/'bin/Release/VCSNative'
assert digest(candidate)==request['binary_sha256'], 'Transferred binary checksum mismatch'
ldd=subprocess.run(['ldd',str(candidate)],text=True,capture_output=True)
(report/'ldd.txt').write_text(ldd.stdout+ldd.stderr)
assert ldd.returncode==0 and 'not found' not in ldd.stdout, 'Unresolved target libraries'
results=[]
for test in request['tests']:
    name=test['name'];command=[translate(a) for a in test['command']]
    if Path(command[0]).name.startswith('python'):command[0]='python3'
    properties={p['name']:p['value'] for p in test['properties']}
    cwd=Path(translate(properties.get('WORKING_DIRECTORY',request['local_build'])))
    cwd.mkdir(parents=True,exist_ok=True)
    test_env=env.copy()
    for assignment in properties.get('ENVIRONMENT',[]):
        key,value=assignment.split('=',1);test_env[key]=translate(value)
    start=time.monotonic()
    with (report/(name+'.log')).open('w') as log:
        try:
            result=subprocess.run(command,cwd=cwd,env=test_env,stdout=log,stderr=subprocess.STDOUT,
                                  timeout=properties.get('TIMEOUT',180))
            code=result.returncode
        except subprocess.TimeoutExpired:code=124
    results.append(dict(name=name,exit_code=code,seconds=round(time.monotonic()-start,3)))
    print(f'{name}: '+('PASS' if code==0 else f'FAIL ({code})'),flush=True)
summary={'tests':results,'passed':sum(t['exit_code']==0 for t in results),'total':len(results),
         'binary_sha256':request['binary_sha256'],'published':False}
if summary['passed']==summary['total']:
    # Publish atomically while holding the launcher's existing single-instance lock.
    with (package/'userdata/.launch.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        assert subprocess.run(['pgrep','-x','VCSNative'],capture_output=True).returncode!=0, 'Game started during tests; nothing published'
        for path in ('run/NativeWorld/MAINLA.wld','run/NativeWorld/BEACH.wld','run/PSP_DATA'):
            assert (package/path).exists(), f'Missing runtime dependency: {path}'
        current=package/'run/VCSNative'
        summary['previous_binary_sha256']=digest(current)
        previous=package/'run/VCSNative.previous'
        if digest(current)!=request['binary_sha256']:
            shutil.copy2(current,previous)
            shutil.copy2(package/'launch.py',package/'run/launch.py.previous')
        pending=package/'run/VCSNative.new'
        shutil.copy2(candidate,pending);pending.replace(current)
        launcher=package/'launch.py.new'
        shutil.copy2(stage/'launch.py',launcher);launcher.replace(package/'launch.py')
        summary['published']=True
    if request['smoke']:
        with (report/'smoke-runner.log').open('w') as log:
            result=subprocess.run(['python3',str(package/'tools/smoke_test.py'),'--root',str(package)],
                                  stdout=log,stderr=subprocess.STDOUT,timeout=200)
        summary['smoke_exit_code']=result.returncode
        if (package/'work/smoke').exists():shutil.copytree(package/'work/smoke',report/'smoke',dirs_exist_ok=True)
summary['userdata_unchanged']=before==snapshot()
(report/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary),flush=True)
assert summary['userdata_unchanged'], 'User settings/saves changed during test'
assert summary['passed']==summary['total'], 'Target regression failed'
assert summary.get('smoke_exit_code',0)==0, 'Gameplay smoke test failed; previous binary preserved as run/VCSNative.previous'
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-steamdeck')
    parser.add_argument('--host', default='steamdeck')
    parser.add_argument('--package', default='~/Downloads/VCSNative')
    parser.add_argument('--smoke', action='store_true', help='Also run one silent, headless, bounded game test.')
    args = parser.parse_args()
    if args.host.startswith('-'):
        parser.error('Invalid SSH host')
    build = args.build_dir.resolve()
    metadata = json.loads((build / 'cross-build.json').read_text())
    if metadata['target'] != 'Linux x86-64':
        parser.error('Expected a completed Steam Deck cross-build')
    if hashlib.sha256((build / 'bin/Release/VCSNative').read_bytes()).hexdigest() != metadata['binary_sha256']:
        parser.error('Binary changed since cross-build.json was written; finish the build first')
    tests = json.loads(subprocess.check_output(
        ['ctest', '--test-dir', str(build), '--show-only=json-v1'], text=True))['tests']
    if not tests or any('command' not in test for test in tests):
        parser.error('Build all enabled test executables first')
    report = build / 'steamdeck-verification'
    report.mkdir(exist_ok=True)
    archive = report / 'tests.tar.gz'
    request = dict(local_root=str(ROOT), local_build=str(build), tests=tests,
                   smoke=args.smoke, binary_sha256=metadata['binary_sha256'])
    prefix = 'verification/cross-build/'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(ROOT / 'launch.py', arcname=prefix + 'launch.py', recursive=False)
        files = {build / 'bin/Release/VCSNative', build / 'tools/recompiler/psp_recomp'}
        files.update(Path(t['command'][0]) for t in tests if Path(t['command'][0]).is_relative_to(build))
        for path in sorted(files):
            tar.add(path, arcname=prefix + 'build/' + str(path.relative_to(build)), recursive=False)
        for directory in ('tests', 'tools'):
            for path in sorted((ROOT / directory).glob('*.py')):
                tar.add(path, arcname=str(path.relative_to(ROOT)), recursive=False)
        for name, data in [('request.json', json.dumps(request).encode()), ('runner.py', REMOTE_RUNNER.encode())]:
            member = tarfile.TarInfo(prefix + name); member.size = len(data)
            tar.addfile(member, io.BytesIO(data))
    def ssh(command, **kwargs):
        return subprocess.run(['ssh', *SSH_OPTIONS, args.host, command], **kwargs)
    query = ('from pathlib import Path; import subprocess; '
             'assert subprocess.run(["pgrep","-x","VCSNative"],capture_output=True).returncode!=0, "VCSNative is running"; '
             f'p=Path({args.package!r}).expanduser().resolve(); '
             'assert (p/"run/VCSNative").is_file(), "Existing package is missing"; print(p)')
    package = ssh('python3 -c ' + shlex.quote(query), check=True, capture_output=True, text=True).stdout.strip()
    remote = package + '/verification/cross-build'
    print(f'Deploy and test in {args.host}:{package}', flush=True)
    with archive.open('rb') as stream:
        ssh(f'tar -xzf - -C {shlex.quote(package)}', stdin=stream, check=True)
    with (report / 'target-tests.log').open('w') as log:
        result = ssh(f'python3 {shlex.quote(remote + "/runner.py")}', stdout=log, stderr=subprocess.STDOUT)
    with (report / 'results.tar.gz').open('wb') as output:
        ssh(f'tar -czf - -C {shlex.quote(remote)} report', stdout=output, check=True)
    with tarfile.open(report / 'results.tar.gz') as tar:
        tar.extractall(report, filter='data')
    summary_path = report / 'report/results.json'
    if summary_path.exists():
        print(summary_path.read_text())
    if result.returncode:
        raise RuntimeError(f'Steam Deck verification failed; see {report / "target-tests.log"}')
    print(f'Updated: {args.host}:{package}/run/VCSNative')
    print(f'Verification: {report / "report"}')


if __name__ == '__main__':
    main()
