"""Stage a Windows runtime for both the portable archive and Inno Setup."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

VERSION = "2.0.3"


def find_deployer(build_dir):
    for name in ("windeployqt", "windeployqt6"):
        found = shutil.which(name)
        if found:
            return Path(found)
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith("Qt6_DIR:"):
                sdk = Path(line.split("=", 1)[1]).parents[2]
                for name in ("windeployqt.exe", "windeployqt6.exe"):
                    if (sdk / "bin" / name).is_file():
                        return sdk / "bin" / name
    raise RuntimeError("windeployqt was not found. Run from the matching Qt build environment.")


def package_portable(build_dir=Path("build"), config="Release"):
    build_dir = Path(build_dir).resolve()
    candidates = (build_dir / config / "hevc_shrinker.exe", build_dir / "hevc_shrinker.exe")
    executable = next((path for path in candidates if path.is_file()), None)
    if executable is None:
        raise RuntimeError(f"No compiled hevc_shrinker.exe found in {build_dir} ({config}).")
    deployer = find_deployer(build_dir)
    dist = Path("dist")
    dist.mkdir(exist_ok=True)
    stage = Path("deploy/windows")
    stage.parent.mkdir(exist_ok=True)
    portable_name = f"hevc_shrinker_portable_{VERSION}_win64"
    # A failed deployment must not replace the previous usable stage/archive.
    with tempfile.TemporaryDirectory(prefix="windows-", dir=stage.parent) as temporary:
        runtime = Path(temporary)
        shutil.copy2(executable, runtime / executable.name)
        subprocess.run([str(deployer), "--release", "--compiler-runtime", "--no-translations",
                        str((runtime / executable.name).resolve())], check=True)
        for plugin in ("platforms/qwindows.dll", "sqldrivers/qsqlite.dll"):
            if not (runtime / plugin).is_file():
                raise RuntimeError(f"Qt deployment is missing {plugin}.")
        for name in ("ffmpeg.exe", "ffprobe.exe"):
            dependency = next((directory / name for directory in (executable.parent, build_dir)
                               if (directory / name).is_file()), None)
            if dependency:
                shutil.copy2(dependency, runtime / name)
        archive = dist / f"{portable_name}.zip"
        pending = archive.with_suffix(".zip.tmp")
        try:
            with zipfile.ZipFile(pending, "w", zipfile.ZIP_DEFLATED) as output:
                for file in sorted(runtime.rglob("*")):
                    if file.is_file():
                        output.write(file, str(Path(portable_name) / file.relative_to(runtime)))
            if stage.exists():
                shutil.rmtree(stage)
            shutil.copytree(runtime, stage)
            os.replace(pending, archive)
        finally:
            pending.unlink(missing_ok=True)
    print(f"Portable archive: {archive}; installer runtime: {stage}")


if __name__ == "__main__":
    os.chdir(Path(__file__).resolve().parent)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release")
    options = parser.parse_args()
    try:
        package_portable(options.build_dir, options.config)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
