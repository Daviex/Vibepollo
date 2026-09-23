"""Build/run isolated RTSP ANNOUNCE checks from server MinGW objects.

Preserve sunshine.rsp with `cmake --build <build> --target sunshine -- -d keeprsp`.
Only a copied main object is modified. The RTSP source is recompiled with its
explicit harness macro to use private Windows HDR event names, preventing
interaction with an already-running server; parser/handler code stays intact.
No stream::start stub or production main is invoked. Each case gets a 30-second
internal watchdog plus a 35-second parent timeout. Logs remain in output-dir.
With --positive, stream.cpp is recompiled with the same harness macro to
isolate platform/display/driver-profile/tray callbacks while preserving real
session allocation/start/threads/ownership. This is not a client end-to-end run.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess


def quoted(path):
    return '"' + Path(path).as_posix() + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-build", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--feature-off", action="store_true")
    parser.add_argument("--positive", action="store_true")
    parser.add_argument("--media", action="store_true", help="After real ANNOUNCE 200, receive and reference-decode three video frames in RAM")
    args = parser.parse_args()
    if args.media and not args.positive:
        parser.error("--media requires --positive")
    build, output = args.server_build.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    command = next(item["command"] for item in commands if item["file"].replace("\\", "/").endswith("/src/rtsp.cpp"))
    compiler, flags = command.split(" ", 1)
    compiler = compiler.strip('"')
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment["PATH"]
    obj = output / "rtsp_smoke.cpp.obj"
    compile_rsp = output / "compile.rsp"
    compile_rsp.write_text(flags.rsplit(" -o ", 1)[0] + " -DSUNSHINE_PYROWAVE_RTSP_HARNESS=1 -o " + quoted(obj) + " -c " + quoted(root / "tools/pyrowave_rtsp_smoke.cpp"), encoding="utf-8")
    subprocess.run([compiler, "@" + str(compile_rsp)], cwd=build, env=environment, check=True)
    copied_main = output / "server_main_unused.cpp.obj"
    shutil.copy2(build / "CMakeFiles/sunshine.dir/src/main.cpp.obj", copied_main)
    subprocess.run([str(Path(compiler).with_name("objcopy.exe")), "--redefine-sym", "main=pyrowave_unused_server_main", str(copied_main)], env=environment, check=True)
    copied_rtsp = output / "rtsp_isolated_events.cpp.obj"
    rtsp_compile_rsp = output / "rtsp_compile.rsp"
    rtsp_compile_rsp.write_text(flags.rsplit(" -o ", 1)[0] + " -DSUNSHINE_PYROWAVE_RTSP_HARNESS=1 -o " + quoted(copied_rtsp) + " -c " + quoted(root / "src/rtsp.cpp"), encoding="utf-8")
    subprocess.run([compiler, "@" + str(rtsp_compile_rsp)], cwd=build, env=environment, check=True)
    isolated_stream = output / "stream_isolated_callbacks.cpp.obj"
    stream_command = next(item["command"] for item in commands if item["file"].replace("\\", "/").endswith("/src/stream.cpp"))
    stream_flags = stream_command.split(" ", 1)[1].rsplit(" -o ", 1)[0]
    stream_compile_rsp = output / "stream_compile.rsp"
    stream_compile_rsp.write_text(stream_flags + " -DSUNSHINE_PYROWAVE_RTSP_HARNESS=1 -o " + quoted(isolated_stream) + " -c " + quoted(root / "src/stream.cpp"), encoding="utf-8")
    subprocess.run([compiler, "@" + str(stream_compile_rsp)], cwd=build, env=environment, check=True)
    response = (build / "CMakeFiles/sunshine.rsp").read_text(encoding="utf-8")
    for token, replacement in (("CMakeFiles/sunshine.dir/src/main.cpp.obj", quoted(copied_main) + " " + quoted(obj)),
                               ("CMakeFiles/sunshine.dir/src/rtsp.cpp.obj", quoted(copied_rtsp)),
                               ("CMakeFiles/sunshine.dir/src/stream.cpp.obj", quoted(isolated_stream))):
        if response.count(token) != 1:
            raise RuntimeError("Unexpected response file object occurrence: " + token)
        response = response.replace(token, replacement)
    link_rsp = output / "link.rsp"
    link_rsp.write_text(response, encoding="utf-8")
    executable = output / "pyrowave-rtsp-smoke.exe"
    subprocess.run([compiler, "-O3", "-DNDEBUG", "-static", "@" + str(link_rsp), "-o", str(executable)], cwd=build, env=environment, check=True)
    if args.positive:
        if args.feature_off:
            raise RuntimeError("--positive requires an enabled server build")
        shutil.copy2(build / "libpyrowave-shared-0.dll", output)
    if not args.run:
        print(f"Built {executable}")
        return
    cases = ["unknown-codec", "malformed-codec", "no-launch-opt-in", "duplicate-extension", "last-line-no-newline"]
    if args.feature_off:
        cases += ["feature-off"]
    else:
        cases += ["host-disabled", "wrong-profile", "wrong-revision", "wrong-version", "missing-version", "hdr-profile", "malformed-number", "overflow-number",
                  "encryption-empty", "encryption-negative", "encryption-junk", "encryption-overflow", "encryption-unknown-bits", "duplicate-number", "mismatched-fps"]
    if args.positive:
        cases += ["positive-media" if args.media else "positive-start-stop"]
    results = []
    for case in cases:
        # Positive GPU probing uses the real server shader assets.
        run = subprocess.run([str(executable), case], cwd=build if case.startswith("positive-") else output,
                             env=environment, capture_output=True, text=True, timeout=35)
        transcript = run.stdout + run.stderr
        (output / f"{case}.log").write_text(transcript, encoding="utf-8")
        if run.returncode != 0:
            raise RuntimeError(f"{case} failed with {run.returncode}: {transcript}")
        if case in ("wrong-profile", "wrong-revision", "wrong-version", "missing-version"):
            if "Unsupported PyroWave protocol version, bitstream revision or profile" not in transcript:
                raise RuntimeError(f"{case} did not reach the expected version/profile rejection: {transcript}")
        if case == "host-disabled" and "PyroWave is disabled on the host" not in transcript:
            raise RuntimeError("Host-disabled case did not reach the host configuration check")
        if case == "mismatched-fps" and "PyroWave integer and fractional framerates are inconsistent" not in transcript:
            raise RuntimeError("Mismatched-FPS case did not reach the framerate consistency check")
        summary = next(line for line in transcript.splitlines() if line.startswith("PASS case="))
        metrics = [line for line in transcript.splitlines() if line.startswith(("UDP ", "CONTROL ", "PROBE "))]
        for line in metrics:
            print(line, flush=True)
        print(summary, flush=True)
        results.append({"case": case, "exit_code": run.returncode, "summary": summary, "metrics": metrics})
    (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"PASS {len(results)} real loopback RTSP cases; positive OS-isolated startup enabled={args.positive}")


if __name__ == "__main__":
    main()
