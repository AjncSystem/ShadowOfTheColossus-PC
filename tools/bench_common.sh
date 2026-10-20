# sotc: settings shared by the benchmark scripts (sourced from the repository root).
#   PY           Windows Python (with Pillow for the shrine benchmark); default /c/PyBuild312/python.exe
#                when present, else python on PATH (not MSYS2's: put clang64/bin after it)
#   BENCH_CVARS  engine cvars; default the culling settings sotc.py and the launcher play with
#                (BENCH_CVARS= measures without them, as the scripts used to)
# bench_env adds --env SOTC_FPS_CAP=off to BENCH_ENV when the modes include fps60: the 30 FPS
# cap is also applied by the port (sotc_fps_cap.h), which the fps60 patch alone does not lift.
if [[ -z ${PY:-} ]]; then
    if [[ -x /c/PyBuild312/python.exe ]]; then PY=/c/PyBuild312/python.exe; else PY=$(command -v python || command -v python3); fi
fi
[[ -n $PY ]] || { echo "Python not found (set PY)" >&2; exit 1; }
BENCH_CVARS=${BENCH_CVARS-doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0}
bench_env() { # bench_env MODES
    BENCH_ENV=()
    [[ ,$1, == *,fps60,* ]] && BENCH_ENV+=(--env SOTC_FPS_CAP=off)
    return 0
}
