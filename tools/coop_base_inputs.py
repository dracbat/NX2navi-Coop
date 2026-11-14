"""The binary parts of an NX2navi Co-op release, taken from the upstream release it is made from, for
tools/make_launcher.py (.github/workflows/release-coop.yml).

NX2navi Co-op changes no binary part of a release: the shader translator, AMD's FSR libraries, NVIDIA's DLSS
libraries, DLSS Frame Generation's nx2navi_dlssg.dll, the button art, the shared pipeline list and the toolchain lock
are the upstream release's. They come from its app archive, NX2navi-app-<base>.zip, checked against the size and
SHA-256 its NX2navi-update.json gives. So the release build needs neither .NET (the translator) nor Visual Studio
(nx2navi_dlssg.dll) nor the art, and the toolchain lock stays upstream's: its compiler is the one players' launchers
install, and the prebuilt runtime library matches it. An execution profile the archive may hold (data/, for
make_launcher.py --pgo) is not handed on: it was trained on the upstream recomp/, whose patch table NX2navi Co-op
extends, and make_launcher.py refuses a profile whose record names another recomp/.

    python tools/coop_base_inputs.py --zip NX2navi-app-0.10.1.zip --manifest NX2navi-update.json --out WORK
                                     [--seed-runtime runtime] [--no-prompt-art] [--no-pipeline-list]
    python tools/coop_base_inputs.py --dir <an unpacked app/<version> folder> --out WORK --dry-run

It prints one JSON object: the base version and the arguments that hand those parts to make_launcher.py. The archive
is unpacked into WORK/base, and a folder laid out as the Streamline SDK (what tools/streamline_pin.py checks) is made
in WORK/streamline-sdk from the files the upstream release placed in bin/dlssg unchanged.

--seed-runtime copies the release's generated runtime headers (tools/build_shaders.py: the SPIR-V of the runtime's
shaders and the embedded files, which git ignores) into this checkout's runtime/ where they are missing: each records
the source it was made from, so tools/build_shaders.py rebuilds only one whose source NX2navi Co-op changed, and the
release build needs no shader compiler otherwise. Nothing of the game is involved: the archive holds no game data.
"""
import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_shaders  # noqa: E402
import streamline_pin  # noqa: E402


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def unpack(zip_path, manifest, out):
    """The upstream app archive, checked against its manifest, unpacked into out/base: (folder, version)."""
    m = json.loads(Path(manifest).read_text(encoding='utf-8'))
    app = m['app']
    zip_path = Path(zip_path)
    if zip_path.name != app['name'] or zip_path.stat().st_size != app['size'] or sha256(zip_path) != app['sha256']:
        sys.exit(f'{zip_path} is not the {m["version"]} app archive its manifest names')
    dest = Path(out) / 'base'
    shutil.rmtree(dest, ignore_errors=True)
    with zipfile.ZipFile(zip_path) as z:
        for n in z.namelist():
            parts = n.replace('\\', '/').split('/')
            if n.startswith(('/', '\\')) or '..' in parts or ':' in parts[0]:
                sys.exit(f'unsafe name in {zip_path}: {n}')
        z.extractall(dest)
    return dest, m['version']


def streamline_sdk(base, out, dry):
    """A folder laid out as the Streamline SDK (bin/x64/*.dll and the two licence texts) from bin/dlssg, where
    streamline_pin.place copied the SDK's files unchanged; None and what is missing when they are not all there."""
    src = base / 'bin' / 'dlssg'
    dlls = [n for n in streamline_pin.FILES if n != streamline_pin.DLL]
    missing = [n for n in (*dlls, *streamline_pin.TEXTS) if not (src / n).is_file()]
    if missing:
        return None, missing
    sdk = Path(out) / 'streamline-sdk'
    if not dry:
        shutil.rmtree(sdk, ignore_errors=True)
        (sdk / 'bin' / 'x64').mkdir(parents=True)
        for n in dlls:
            shutil.copy2(src / n, sdk / 'bin' / 'x64' / n)
        for have, want in streamline_pin.TEXTS.items():
            shutil.copy2(src / have, sdk / want)
        bad = streamline_pin.wrong_files(sdk)
        if bad:
            sys.exit('the DLSS Frame Generation files of the upstream release are not those of Streamline '
                     f'{streamline_pin.VERSION}: ' + ', '.join(bad))
    return sdk, []


def make_launcher_args(base, out, dry, art=True, pipelines=True):
    """make_launcher.py's arguments for the parts in base (an unpacked app/<version>), and notes on what is left out."""
    b = base / 'bin'
    if not (b / 'nx2navi_xlate.exe').is_file() or not (b / 'nx2navi_xlate.id').is_file():
        sys.exit(f'{b} lacks the shader translator')
    a, notes = ['--xlate', b], []
    if (b / 'amd_fidelityfx_vk.dll').is_file():
        a += ['--ffx', b / 'amd_fidelityfx_vk.dll']
    if all((b / n).is_file() for n in ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll')):
        a += ['--ffx4', b]
    a += ['--dlss', b / 'nvngx_dlss.dll'] if (b / 'nvngx_dlss.dll').is_file() else ['--no-dlss']
    if (b / 'dlssg' / streamline_pin.DLL).is_file():
        sdk, missing = streamline_sdk(base, out, dry)
        if sdk:
            a += ['--dlssg', b / 'dlssg' / streamline_pin.DLL, '--streamline', sdk]
        else:
            notes.append(f'DLSS Frame Generation left out: bin/dlssg lacks {", ".join(missing)}')
    if art and (base / 'prompt-art').is_dir():
        a += ['--prompt-art', base / 'prompt-art']
    elif art:
        notes.append('no button art in the upstream release: the game shows its own prompts')
    if pipelines and (base / 'tools' / 'data' / 'pipeline_list.bin').is_file():
        a += ['--pipeline-list', base / 'tools' / 'data' / 'pipeline_list.bin']
    if not (base / 'toolchain.lock.json').is_file():
        sys.exit(f'{base} lacks toolchain.lock.json')
    a += ['--lock', base / 'toolchain.lock.json']
    return [str(x) for x in a], notes


def seed_runtime(base, runtime):
    """The generated headers (build_shaders.sources) of the release's runtime/ copied into runtime where they are
    missing; their number."""
    n = 0
    for _src, header in build_shaders.sources(Path(runtime)):
        rel = header.relative_to(Path(runtime))
        have = base / 'runtime' / rel
        if not header.exists() and have.is_file():
            header.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(have, header)
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--zip', type=Path, help="the upstream release's NX2navi-app-<version>.zip")
    ap.add_argument('--manifest', type=Path, help="the upstream release's NX2navi-update.json")
    ap.add_argument('--dir', type=Path, help='an unpacked app/<version> folder instead of --zip and --manifest')
    ap.add_argument('--out', type=Path, default=Path('.'), help='the work folder')
    ap.add_argument('--seed-runtime', type=Path, metavar='RUNTIME', help="this checkout's runtime/ folder")
    ap.add_argument('--no-prompt-art', action='store_true', help='leave the button art out of the release')
    ap.add_argument('--no-pipeline-list', action='store_true', help='leave the shared pipeline list out of the release')
    ap.add_argument('--dry-run', action='store_true', help='check and print only; writes nothing')
    a = ap.parse_args()
    if a.dir:
        base, ver = a.dir, (a.dir / 'VERSION').read_text(encoding='utf-8').strip()
    elif a.zip and a.manifest:
        base, ver = unpack(a.zip, a.manifest, a.out)
    else:
        ap.error('give --zip and --manifest, or --dir')
    args, notes = make_launcher_args(base, a.out, a.dry_run, not a.no_prompt_art, not a.no_pipeline_list)
    for n in notes:
        print('note: ' + n, file=sys.stderr)
    if a.seed_runtime and not a.dry_run:
        print(f'seeded {seed_runtime(base, a.seed_runtime)} generated runtime headers', file=sys.stderr)
    print(json.dumps({'base_version': ver, 'make_launcher_args': args}, indent=1))


if __name__ == '__main__':
    main()
