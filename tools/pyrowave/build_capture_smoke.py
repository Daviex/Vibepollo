"""Build the capture harness from a completed MinGW/Ninja server build.

First build the server with Ninja's `-d keeprsp` so its response file remains.
Only copies of the response and main object are modified. The resulting binary
has its own main and never invokes the renamed production entry point.
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
    args = parser.parse_args()
    build = args.server_build.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    command = next(item["command"] for item in commands if item["file"].replace("\\", "/").endswith("/src/platform/windows/display_vram.cpp"))
    compiler, flags = command.split(" ", 1)
    compiler = compiler.strip('"')
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment["PATH"]
    flags = flags.rsplit(" -o ", 1)[0]
    obj = output / "capture_smoke.cpp.obj"
    compile_rsp = output / "compile.rsp"
    compile_rsp.write_text(flags + " -o " + quoted(obj) + " -c " + quoted(root / "tools/pyrowave/capture_smoke.cpp"), encoding="utf-8")
    subprocess.run([compiler, "@" + str(compile_rsp)], cwd=build, env=environment, check=True)
    original_main = build / "CMakeFiles/sunshine.dir/src/main.cpp.obj"
    copied_main = output / "server_main_unused.cpp.obj"
    shutil.copy2(original_main, copied_main)
    subprocess.run([str(Path(compiler).with_name("objcopy.exe")), "--redefine-sym", "main=pyrowave_unused_server_main", str(copied_main)], env=environment, check=True)
    response = (build / "CMakeFiles/sunshine.rsp").read_text(encoding="utf-8")
    main_token = "CMakeFiles/sunshine.dir/src/main.cpp.obj"
    if response.count(main_token) != 1:
        raise RuntimeError("Unexpected server response file: main object must appear once")
    response = response.replace(main_token, quoted(copied_main) + " " + quoted(obj)) + " -ldxguid"
    link_rsp = output / "link.rsp"
    link_rsp.write_text(response, encoding="utf-8")
    executable = output / "pyrowave-capture-smoke.exe"
    subprocess.run([compiler, "-O3", "-DNDEBUG", "-static", "@" + str(link_rsp), "-o", str(executable)], cwd=build, env=environment, check=True)
    shutil.copy2(build / "libpyrowave-shared-0.dll", output)
    helper = build / "tools/sunshine_wgc_capture.exe"
    if helper.is_file():
        (output / "tools").mkdir(exist_ok=True)
        shutil.copy2(helper, output / "tools")
    print(f"Built {executable}. Run from {build} so the actual server shaders resolve.")


if __name__ == "__main__":
    main()
