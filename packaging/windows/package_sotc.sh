#!/usr/bin/env bash
# sotc: builds dist/SotC-PC/ and dist/SotC-PC.zip: SotC.exe (the launcher, frozen with PyInstaller
# so players need no Python), bb-probe.exe with the MSYS2 CLANG64 DLLs it needs, the preparation
# scripts, sotc.py and SotC.bat. Players need their own game dump; sound needs their own console's
# libSceNgs2/libSceUlt (never packaged here). Run after `bash build.sh`.
# Freezing uses a Windows Python with PyInstaller and Pillow (WINPYTHON, default C:\PyBuild312).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
export MSYS2_ROOT=${MSYS2_ROOT:-/i/msys64}
export PATH="$MSYS2_ROOT/clang64/bin:$MSYS2_ROOT/usr/bin:$PATH"
[[ -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
python=${WINPYTHON:-/c/PyBuild312/python.exe}
"$python" -m PyInstaller --version >/dev/null || { echo "Need PyInstaller in $python (WINPYTHON)" >&2; exit 1; }
# The scripts run inside SotC.exe (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil struct; do
    hidden+=(--hidden-import "$module")
done
"$python" -m PyInstaller --noconfirm --clean --log-level WARN --windowed --name SotC \
    --icon "$(cygpath -w "$PWD/launcher/sotc.ico")" --distpath out/pyi-dist --workpath out/pyi-work \
    --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher/sotc_launcher_win.py")"

dest=out/stage/SotC-PC
rm -rf -- out/stage
mkdir -p "$dest/bin" "$dest/scripts" "$dest/launcher"
cp -r out/pyi-dist/SotC/. "$dest/"
cp launcher/sotc.ico launcher/sotc.png launcher/window_icon.png "$dest/launcher/"
# Xbox-style button icons drawn for the port (tools/make_button_icons.py).
cp -r glyphs "$dest/"
# Without debug information (out/ keeps the symbols for crash reports).
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the loader takes from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe" | awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
    cp -u "$dll" "$dest/bin/"
done
# DLSS Frame Generation (RTX 40/50): the official NVIDIA Streamline runtime next to bb-probe.exe,
# with NVIDIA's licenses (redistributable under the NVIDIA RTX SDKs license and Streamline's MIT
# license). Not in git: STREAMLINE_DIR (default ../SotC-deps/streamline-2.14.16) holds the files
# taken from github.com/NVIDIA-RTX/Streamline releases v2.14.16 (bin/x64). Without it the package
# still works; frame generation then offers FSR 3 only.
streamline=${STREAMLINE_DIR:-../SotC-deps/streamline-2.14.16}
if [[ -f "$streamline/sl.interposer.dll" ]]; then
    cp "$streamline"/*.dll "$dest/bin/"
    mkdir -p "$dest/bin/licenses-nvidia"
    cp "$streamline"/licenses-nvidia/* "$dest/bin/licenses-nvidia/"
    echo "DLSS-G: NVIDIA Streamline runtime included"
else
    echo "DLSS-G: no Streamline runtime in $streamline (FSR 3 only)" >&2
fi
cp scripts/prepare.py scripts/link_libc.py scripts/link_modules.py scripts/content_profile.py \
   scripts/sotc_cpu.py scripts/sotc_modes.py "$dest/scripts/"
cp sotc.py SotC.bat LICENSE CHANGELOG.md "$dest/"
cp docs/sotc/README-SotC.md "$dest/README.md"
cat > "$dest/sotc.ini" <<'EOF'
# Shadow of the Colossus PC - settings (see README.md)
# Folder of your game dump (the one with eboot.bin):
SOTC_GAME_DIR=..\CUSA08809
# Folder with libSceNgs2.sprx and libSceUlt.sprx from YOUR OWN console (for sound):
# SOTC_SYS_MODULES=C:\path\to\sys_modules
# Optional modes: af16 (sharp textures), fps60 (no 30 FPS cap, experimental):
# SOTC_MODES=af16
EOF
# Sony firmware must never be in the package.
if find "$dest" -iname '*.sprx' -o -iname '*.prx' | grep -q .; then
    echo 'refusing to package firmware modules' >&2; exit 1
fi

mkdir -p dist
rm -rf dist/SotC-PC.zip
python -c 'import shutil, sys; shutil.make_archive("dist/SotC-PC", "zip", "out/stage", "SotC-PC")'
# The playable copy keeps its settings and saves.
mkdir -p dist/SotC-PC
cp -r "$dest/bin" "$dest/scripts" "$dest/launcher" "$dest/_internal" dist/SotC-PC/
cp "$dest/SotC.exe" "$dest/sotc.py" "$dest/SotC.bat" "$dest/README.md" "$dest/LICENSE" "$dest/CHANGELOG.md" dist/SotC-PC/
[[ -f dist/SotC-PC/sotc.ini ]] || cp "$dest/sotc.ini" dist/SotC-PC/
echo "dist/SotC-PC.zip ($(du -h dist/SotC-PC.zip | cut -f1)) and dist/SotC-PC/"
