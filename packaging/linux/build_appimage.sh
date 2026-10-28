#!/usr/bin/env bash
# Builds dist/SotC-PC-<version>-linux-x86_64.AppImage from the current Linux build.
#   bash packaging/linux/build_ffmpeg.sh   (once: minimal FFmpeg, see that script)
#   ./build.sh
#   bash packaging/linux/build_appimage.sh
# The AppImage carries the loader, the GPU library, the preparation scripts, the launcher with
# this machine's Python and Tcl/Tk (so the host needs neither) and every library they use except
# the base system every desktop has (glibc, libstdc++, the Vulkan loader, fontconfig and
# freetype, which read the host's own font setup). It never contains game or Sony files: the
# user points it at their own dump and sound modules on first start (packaging/linux/AppRun).
# Tools: patchelf, python3 with Tk (apt install python3-tk, dnf install python3-tkinter), and
# appimagetool (APPIMAGETOOL=path, else downloaded into out/tools).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so ]] || { echo 'Build first: ./build.sh' >&2; exit 1; }
command -v patchelf >/dev/null || { echo 'Need patchelf (dnf/apt install patchelf).' >&2; exit 1; }
python3 -c 'import tkinter' 2>/dev/null ||
    { echo 'Need python3 with Tk for the launcher (apt install python3-tk, dnf install python3-tkinter).' >&2; exit 1; }
version=$(sed -n 's/^## \([0-9][0-9.]*\).*/\1/p' CHANGELOG.md | head -1)
output=dist/SotC-PC-${version:-dev}-linux-x86_64.AppImage

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
appdir=$work/AppDir
app=$appdir/usr/share/sotc-pc
mkdir -p "$app/bin/gpu" "$app/lib"
install -m755 packaging/linux/AppRun "$appdir/AppRun"
install -m644 sotc.py LICENSE "$app/"
cp -r scripts "$app/"
find "$app/scripts" -name __pycache__ -prune -exec rm -rf {} +
mkdir -p "$app/launcher"
install -m644 launcher/sotc_launcher_win.py launcher/sotc_discovery.py launcher/sotc_i18n.py launcher/sotc_update.py \
    launcher/sotc.png "$app/launcher/"
install -m755 out/bb-probe out/bb-gpu-capabilities "$app/bin/"
install -m755 out/gpu/libbbgpu.so "$app/bin/gpu/"
strip --strip-debug "$app/bin/bb-probe" "$app/bin/bb-gpu-capabilities" "$app/bin/gpu/libbbgpu.so"

# The launcher's Python: this machine's interpreter with its standard library (compiled modules
# kept valid by cp -a) and Tcl/Tk. It finds the library next to itself (lib/python3.X).
read -r py_exe py_stdlib py_dynload py_platlib tcl_dir tk_version < <(python3 -c '
import os, sys, sysconfig, tkinter, _tkinter
print(os.path.realpath(sys.executable), sysconfig.get_path("stdlib"), os.path.dirname(_tkinter.__file__),
      sys.platlibdir, tkinter.Tcl().eval("info library"), tkinter.TkVersion)')
python=$app/python
stdlib=$python/$py_platlib/$(basename "$py_stdlib")
mkdir -p "$python/bin" "$python/$py_platlib"
install -m755 "$py_exe" "$python/bin/python3"
cp -a "$py_stdlib" "$stdlib"
[[ -d $stdlib/lib-dynload ]] || cp -a "$py_dynload" "$stdlib/lib-dynload"
# Not used by the launcher or sotc.py: tests, IDLE, pip bootstrap, the static build configuration,
# the terminal modules (they pull curses and readline) and the dbm databases.
rm -rf "$stdlib"/{test,idlelib,turtledemo,ensurepip,site-packages,dist-packages} "$stdlib"/config-*
rm -f "$stdlib"/lib-dynload/{_test,xxlimited,_xxtestfuzz,_ctypes_test,_curses,readline,_dbm,_gdbm}*
# Links out of the tree (Debian's sitecustomize.py -> /etc/python3.X) would load the host's files.
find "$stdlib" -type l -lname '/*' -delete
# Tcl/Tk's own scripts (init.tcl, tk.tcl): AppRun points TCL_LIBRARY/TK_LIBRARY at these copies.
tk_dir=$(dirname "$tcl_dir")/tk$tk_version
[[ -f $tcl_dir/init.tcl && -f $tk_dir/tk.tcl ]] || { echo "Tcl/Tk scripts not found in $tcl_dir, $tk_dir" >&2; exit 1; }
cp -a "$tcl_dir" "$python/tcl"
cp -a "$tk_dir" "$python/tk"

# Every library the given binaries load, except the base system and the port's own GPU library.
host='^(linux-vdso|ld-linux-x86-64|libc|libm|libdl|librt|libpthread|libresolv|libstdc\+\+|libgcc_s|libvulkan|libfontconfig|libfreetype)\.so'
bundle() {
    while read -r name path; do
        [[ $name =~ $host || $path == */out/gpu/* ]] && continue
        cp -L "$path" "$app/lib/$name"
    done < <(ldd "$@" | awk '$2 == "=>" && $3 ~ /^\// {print $1, $3}' | sort -u)
}
bundle out/bb-probe out/bb-gpu-capabilities out/gpu/libbbgpu.so
count=$(find "$app/lib" -type f | wc -l)
if (( count > 25 )); then
    echo "Warning: $count bundled libraries; the system FFmpeg pulls in many. For a small, portable" >&2
    echo "AppImage build the minimal one: bash packaging/linux/build_ffmpeg.sh, rm -rf out/gpu, ./build.sh" >&2
fi
bundle "$python/bin/python3" "$stdlib"/lib-dynload/*.so
count=$(find "$app/lib" -type f | wc -l)
strip --strip-unneeded "$app"/lib/*
patchelf --set-rpath '$ORIGIN/gpu:$ORIGIN/../lib' "$app/bin/bb-probe"
patchelf --set-rpath '$ORIGIN/../lib' "$app/bin/bb-gpu-capabilities"
patchelf --set-rpath '$ORIGIN/../../lib' "$app/bin/gpu/libbbgpu.so"
patchelf --set-rpath '$ORIGIN/../../lib' "$python/bin/python3"
for module in "$stdlib"/lib-dynload/*.so; do patchelf --set-rpath '$ORIGIN/../../../../lib' "$module"; done
for library in "$app"/lib/*; do patchelf --set-rpath '$ORIGIN' "$library"; done

install -m644 launcher/sotc.png "$appdir/sotc-pc.png"
cat > "$appdir/sotc-pc.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Shadow of the Colossus PC
Comment=Native Linux port of the PS4 Shadow of the Colossus (your own game dump required)
Exec=AppRun
Icon=sotc-pc
Categories=Game;
Terminal=false
EOF

# Sony firmware never goes into the package (same rule as packaging/windows/package_sotc.sh).
if find "$appdir" -iname '*.sprx' -o -iname '*.prx' | grep -q .; then
    echo 'Refusing to package .sprx/.prx files.' >&2; exit 1
fi

tool=${APPIMAGETOOL:-}
if [[ -z $tool ]]; then
    tool=out/tools/appimagetool
    if [[ ! -x $tool ]]; then
        mkdir -p out/tools
        curl -fsSL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
        chmod +x "$tool"
    fi
fi
mkdir -p dist
# APPIMAGE_EXTRACT_AND_RUN: appimagetool itself runs without FUSE (containers, CI).
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$tool" --no-appstream "$appdir" "$output" >/dev/null
echo "Built $output ($count bundled libraries, $(du -h "$output" | cut -f1))"
