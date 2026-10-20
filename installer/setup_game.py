"""FM2 setup helper. The installer runs it with the Python bundled in the installer.

  setup_game.py check-iso <disc.iso>        exit 0 when the disc matches this build
  setup_game.py check-tu <title update>     exit 0 when the title update matches this build
  setup_game.py install <disc.iso> <game dir> <title update> <dlc folder or -> <fm2 documents dir>
                                            builds the game files, unpacks the title update, copies DLC
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# The files this build was made from. A disc or title update must match these to be used.
REF_DEFAULT_XEX = '9726066f0c546c4e51340ed02a32ae44a89d4175de82a2d55aaaf5451d9b120a'
REF_TITLE_UPDATE = 'b5fd1860c2fbab6e594baf7dd58cbfe867f51f65e41177e15e0b36da0f61aae5'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def run_tool(script, *args):
    subprocess.run([sys.executable, os.path.join(HERE, script)] + list(args), check=True)


def check_iso(iso):
    tmp = tempfile.mkdtemp(prefix='fm2-check-')
    try:
        run_tool('xiso.py', 'extract', iso, tmp, 'default.xex')
        found = os.path.join(tmp, 'default.xex')
        return os.path.exists(found) and sha256(found) == REF_DEFAULT_XEX
    except (subprocess.CalledProcessError, OSError):
        return False  # not a disc image the tool can read
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def check_tu(path):
    return os.path.isfile(path) and sha256(path) == REF_TITLE_UPDATE


def install(iso, game_dir, tu, dlc, docs):
    # 1. The game files, from the disc.
    os.makedirs(game_dir, exist_ok=True)
    run_tool('xiso.py', 'extract', iso, game_dir)
    # 2. The title update: the port loads update\default.xexp from the documents folder.
    update_dir = os.path.join(docs, 'update')
    os.makedirs(update_dir, exist_ok=True)
    run_tool('stfs.py', 'update', tu, update_dir)
    # 3. Optional DLC packages.
    if dlc and dlc != '-' and os.path.isdir(dlc):
        dlc_dir = os.path.join(docs, 'dlc')
        os.makedirs(dlc_dir, exist_ok=True)
        for name in os.listdir(dlc):
            src = os.path.join(dlc, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(dlc_dir, name))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'check-iso':
        return 0 if check_iso(argv[2]) else 1
    if cmd == 'check-tu':
        return 0 if check_tu(argv[2]) else 1
    if cmd == 'install':
        return install(argv[2], argv[3], argv[4], argv[5], argv[6])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
