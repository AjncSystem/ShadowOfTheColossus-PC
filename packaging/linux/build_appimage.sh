#!/usr/bin/env bash
# Builds dist/SotC-PC-<version>-linux-x86_64.AppImage from the current Linux build.
#   bash packaging/linux/build_ffmpeg.sh   (once: minimal FFmpeg, see that script)
#   ./build.sh
#   bash packaging/linux/build_appimage.sh
# The AppImage carries the loader, the GPU library, the preparation scripts and every library
# they use except the base system every desktop has (glibc, libstdc++, the Vulkan loader). It
# needs python3 on the host. It never contains game or Sony files: the user points it at their
# own dump and sound modules on first start (packaging/linux/AppRun).
# Tools: patchelf, and appimagetool (APPIMAGETOOL=path, else downloaded into out/tools).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so ]] || { echo 'Build first: ./build.sh' >&2; exit 1; }
command -v patchelf >/dev/null || { echo 'Need patchelf (dnf/apt install patchelf).' >&2; exit 1; }
version=$(sed -n 's/^## \([0-9][0-9.]*\).*/\1/p' CHANGELOG.md | head -1)
output=dist/SotC-PC-${version:-dev}-linux-x86_64.AppImage

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
appdir=$work/AppDir
app=$appdir/usr/share/sotc-pc
mkdir -p "$app/bin/gpu" "$app/lib"
install -m755 packaging/linux/AppRun "$appdir/AppRun"
install -m644 sotc.py LICENSE "$app/"
cp -r scripts patches "$app/"
find "$app/scripts" -name __pycache__ -prune -exec rm -rf {} +
install -m755 out/bb-probe "$app/bin/"
install -m755 out/gpu/libbbgpu.so "$app/bin/gpu/"
strip --strip-debug "$app/bin/bb-probe" "$app/bin/gpu/libbbgpu.so"

# Every library the two binaries load, except the base system and the port's own GPU library.
host='^(linux-vdso|ld-linux-x86-64|libc|libm|libdl|librt|libpthread|libresolv|libstdc\+\+|libgcc_s|libvulkan)\.so'
while read -r name path; do
    [[ $name =~ $host || $path == */out/gpu/* ]] && continue
    cp -L "$path" "$app/lib/$name"
done < <(ldd out/bb-probe out/gpu/libbbgpu.so | awk '$2 == "=>" && $3 ~ /^\// {print $1, $3}' | sort -u)
count=$(find "$app/lib" -type f | wc -l)
if (( count > 25 )); then
    echo "Warning: $count bundled libraries; the system FFmpeg pulls in many. For a small, portable" >&2
    echo "AppImage build the minimal one: bash packaging/linux/build_ffmpeg.sh, rm -rf out/gpu, ./build.sh" >&2
fi
strip --strip-unneeded "$app"/lib/*
patchelf --set-rpath '$ORIGIN/gpu:$ORIGIN/../lib' "$app/bin/bb-probe"
patchelf --set-rpath '$ORIGIN/../../lib' "$app/bin/gpu/libbbgpu.so"
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
