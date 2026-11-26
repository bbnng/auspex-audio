"""Build the release archives from build/auspex_audio.dll, package/, and the source tree.

Usage (after build.cmd succeeds): python tools/package_release.py
Writes to dist/:
  AuspexAudio-<version>.zip         BLSE mod folder under Bin/, merged into the game folder
  AuspexAudio-<version>-source.zip  complete source (the GPL's Corresponding Source)
"""
import os
import re
import shutil
import subprocess
import zipfile

plugin_src = open(os.path.join('src', 'plugin_main.cpp'), encoding='utf-8-sig').read()
m = re.search(r'BLSE_PLUGIN_INFO\(\s*"auspex_audio"\s*,\s*"([^"]+)"', plugin_src)
if not m:
    raise SystemExit('Could not parse version from src/plugin_main.cpp')
version = m.group(1)

for name in ('mod.ini', 'README.txt'):
    if version not in open(os.path.join('package', name), encoding='utf-8').read():
        raise SystemExit(f'package/{name} does not mention version {version}')

for req in (os.path.join('build', 'auspex_audio.dll'), os.path.join('build', 'phonon.dll'), 'auspex_audio.ini'):
    if not os.path.isfile(req):
        raise SystemExit(f'Missing build output: {req} (run build.cmd first)')


def crlf(src, dst):
    text = open(src, encoding='utf-8').read().replace('\r\n', '\n').replace('\n', '\r\n')
    open(dst, 'w', encoding='utf-8', newline='').write(text)


def zipdir(folder, archive):
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for parent, dirs, files in os.walk(folder):
            dirs.sort()
            for f in sorted(files):
                path = os.path.join(parent, f)
                z.write(path, os.path.relpath(path, folder).replace(os.sep, '/'))
    print(archive)


os.makedirs('dist', exist_ok=True)

# 1. Install archive.
stage = os.path.join('dist', f'AuspexAudio-{version}')
shutil.rmtree(stage, ignore_errors=True)
mod = os.path.join(stage, 'Bin', 'loader', 'blse', 'mods', 'auspex_audio')
os.makedirs(os.path.join(mod, 'blse', 'plugins'))
shutil.copy2(os.path.join('build', 'auspex_audio.dll'), os.path.join(mod, 'blse', 'plugins', 'auspex_audio.dll'))
shutil.copy2(os.path.join('build', 'phonon.dll'), os.path.join(mod, 'blse', 'plugins', 'phonon.dll'))
crlf(os.path.join('package', 'mod.ini'), os.path.join(mod, 'mod.ini'))
crlf('auspex_audio.ini', os.path.join(mod, 'auspex_audio.ini'))

# README and license notices sit at the top of the archive, next to the Bin folder.
crlf(os.path.join('package', 'README.txt'), os.path.join(stage, 'Auspex Audio README.txt'))
crlf('LICENSE', os.path.join(stage, 'LICENSE.txt'))
crlf('NOTICE', os.path.join(stage, 'NOTICE.txt'))
crlf(os.path.join('include', 'BLSE-LICENSE.txt'), os.path.join(stage, 'BLSE-LICENSE.txt'))
crlf(os.path.join('third_party', 'steamaudio', 'LICENSE'), os.path.join(stage, 'STEAM-AUDIO-LICENSE.txt'))
crlf(os.path.join('third_party', 'miniaudio', 'LICENSE'), os.path.join(stage, 'MINIAUDIO-LICENSE.txt'))

zipdir(stage, os.path.join('dist', f'AuspexAudio-{version}.zip'))

# 2. Source archive: git ls-files if a git repo, otherwise clean walk excluding build/dist/junk.
source = os.path.join('dist', f'AuspexAudio-{version}-source')
shutil.rmtree(source, ignore_errors=True)

source_files = []
if os.path.isdir('.git'):
    tracked = subprocess.run(['git', 'ls-files'], capture_output=True, text=True, check=True).stdout.splitlines()
    source_files = [p for p in tracked if p]

if not source_files:
    skip_dirs = {'.git', 'build', 'dist', '__pycache__'}
    skip_exts = {'.obj', '.exp', '.pdb', '.ilk', '.i64', '.id0', '.id1', '.id2', '.nam', '.til'}
    for parent, dirs, files in os.walk('.'):
        dirs[:] = sorted(d for d in dirs if d not in skip_dirs)
        for f in sorted(files):
            ext = os.path.splitext(f)[1].lower()
            if ext in skip_exts:
                continue
            rel = os.path.relpath(os.path.join(parent, f), '.')
            source_files.append(rel)

for path in source_files:
    dst = os.path.join(source, path)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(path, dst)

zipdir(source, os.path.join('dist', f'AuspexAudio-{version}-source.zip'))
