import importlib.util
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


portable = module("package_portable")
linux = module("package_linux")


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.original_cwd = Path.cwd()
        self.temp = tempfile.TemporaryDirectory()
        os.chdir(self.temp.name)

    def tearDown(self):
        os.chdir(self.original_cwd)
        self.temp.cleanup()

    def build(self):
        build = Path("build with spaces")
        (build / "Release").mkdir(parents=True)
        (build / "Release/hevc_shrinker.exe").write_bytes(b"app")
        (build / "CMakeCache.txt").write_text("build data")
        (build / "unused.dll").write_bytes(b"unrelated")
        return build

    @staticmethod
    def deploy(command, **kwargs):
        stage = Path(command[-1]).parent
        for name in ("platforms/qwindows.dll", "sqldrivers/qsqlite.dll", "Qt6Core.dll"):
            file = stage / name
            file.parent.mkdir(exist_ok=True)
            file.write_bytes(b"runtime")
        return subprocess.CompletedProcess(command, 0)

    def test_release_directory_and_runtime_only(self):
        build = self.build()
        with mock.patch.object(portable, "find_deployer", return_value=Path("windeployqt.exe")), \
             mock.patch.object(portable.subprocess, "run", side_effect=self.deploy):
            portable.package_portable(build)
        archive = next(Path("dist").glob("*.zip"))
        with zipfile.ZipFile(archive) as output:
            names = output.namelist()
        self.assertTrue(any(name.endswith("sqldrivers/qsqlite.dll") for name in names))
        self.assertFalse(any("CMake" in name or "unused.dll" in name for name in names))
        self.assertTrue(Path("deploy/windows/hevc_shrinker.exe").is_file())

    def test_failed_deployment_preserves_previous_release(self):
        build = self.build()
        stage = Path("deploy/windows")
        stage.mkdir(parents=True)
        (stage / "hevc_shrinker.exe").write_bytes(b"previous")
        archive = Path(f"dist/hevc_shrinker_portable_{portable.VERSION}_win64.zip")
        archive.parent.mkdir()
        archive.write_bytes(b"previous archive")
        with mock.patch.object(portable, "find_deployer", return_value=Path("windeployqt.exe")), \
             mock.patch.object(portable.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "deploy")):
            with self.assertRaises(subprocess.CalledProcessError):
                portable.package_portable(build)
        self.assertEqual(archive.read_bytes(), b"previous archive")
        self.assertEqual((stage / "hevc_shrinker.exe").read_bytes(), b"previous")

    @unittest.skipUnless(shutil.which("dpkg-deb") and shutil.which("dpkg"), "dpkg tools required")
    def test_debian_package_can_be_unpacked_by_dpkg(self):
        if not hasattr(os, "geteuid") or os.geteuid() != 0:
            self.skipTest("isolated dpkg unpack requires root or a mapped-root namespace")
        build = Path("build-release")
        build.mkdir()
        executable = build / "hevc_shrinker"
        executable.write_bytes(b"synthetic executable")
        executable.chmod(0o755)
        # Fractional timestamps trigger PAX headers with Python's default format.
        os.utime(executable, (1700000000.25, 1700000000.25))
        Path("app_icon.png").write_bytes(b"synthetic icon")
        with mock.patch.object(linux, "BUILD_DIR", str(build)):
            self.assertTrue(linux.package_deb())
        archive = next(Path("dist").glob("*.deb")).resolve()
        info = subprocess.run(["dpkg-deb", "--info", str(archive)], check=True, capture_output=True, text=True)
        self.assertIn(f"Version: {linux.VERSION}-1", info.stdout)
        subprocess.run(["dpkg-deb", "--contents", str(archive)], check=True, capture_output=True)
        install_root = Path("isolated-install").resolve()
        database = install_root / "var/lib/dpkg"
        database.mkdir(parents=True)
        (database / "status").write_text("")
        # dpkg's own extractor rejects unsupported PAX headers; listing with tar
        # alone would miss the bug. No package is configured or installed on host.
        result = subprocess.run(["dpkg", f"--root={install_root}", "--unpack", str(archive)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((install_root / "usr/bin/hevc_shrinker").read_bytes(), executable.read_bytes())

    def test_failed_appimage_does_not_publish_old_artifact(self):
        Path("pack-appimage").mkdir()
        for name in ("linuxdeploy-x86_64.AppImage", "linuxdeploy-plugin-qt-x86_64.AppImage"):
            Path("pack-appimage", name).write_bytes(b"tool")
        Path("app_icon.png").write_bytes(b"placeholder")
        stale = Path("HEVC_Video_Shrinker-old.AppImage")
        stale.write_bytes(b"old release")
        with mock.patch.object(linux.shutil, "which", return_value="qmake6"), \
             mock.patch.dict(os.environ, {"QMAKE": "qmake6"}), \
             mock.patch.object(linux.subprocess, "run", side_effect=[
                 subprocess.CompletedProcess([], 0, "6.11.1\n", ""),
                 subprocess.CompletedProcess([], 1, "", "deployment failed")]):
            self.assertFalse(linux.package_appimage())
        self.assertTrue(stale.is_file())
        self.assertEqual(list(Path("dist").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
