#!/usr/bin/env python3
import os
import sys
import shutil
import tarfile
import subprocess
import urllib.request

def run_cmd(cmd, cwd=None):
    print(f"Running: {' '.join(cmd)}")
    res = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        print(f"Error executing command: {res.stderr}")
        return False
    return True

def compile_project():
    print("=== 1. Compiling Release Binary ===")
    os.makedirs("build-linux", exist_ok=True)
    if not run_cmd(["cmake", "-B", "build-linux", "-S", ".", "-DCMAKE_BUILD_TYPE=Release"]):
        return False
    if not run_cmd(["cmake", "--build", "build-linux"]):
        return False
    return True

def package_appimage():
    print("\n=== 2. Packaging AppImage ===")
    os.makedirs("pack-appimage", exist_ok=True)
    os.makedirs("dist", exist_ok=True)
    
    # Download linuxdeploy and its Qt plugin if they don't exist
    ld_path = "pack-appimage/linuxdeploy-x86_64.AppImage"
    plugin_path = "pack-appimage/linuxdeploy-plugin-qt-x86_64.AppImage"
    
    if not os.path.exists(ld_path):
        print("Downloading linuxdeploy...")
        urllib.request.urlretrieve("https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage", ld_path)
        os.chmod(ld_path, 0o755)
        
    if not os.path.exists(plugin_path):
        print("Downloading linuxdeploy-plugin-qt...")
        urllib.request.urlretrieve("https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage", plugin_path)
        os.chmod(plugin_path, 0o755)

    # Create desktop launcher file
    desktop_content = """[Desktop Entry]
Type=Application
Name=HEVC Video Shrinker
Comment=Batch-shrink video libraries into efficient H.265 files
Exec=hevc_shrinker
Icon=hevc_shrinker
Categories=Utility;AudioVideo;Video;
Terminal=false
"""
    with open("hevc_shrinker.desktop", "w") as f:
        f.write(desktop_content)

    # Resize icon to 512x512 using PIL to satisfy linuxdeploy strict resolutions
    try:
        from PIL import Image
        img = Image.open("app_icon.png")
        resample = Image.Resampling.LANCZOS if hasattr(Image, "Resampling") else Image.ANTIALIAS
        img_resized = img.resize((512, 512), resample)
        img_resized.save("hevc_shrinker.png")
        print("Resized icon to 512x512 for linuxdeploy compatibility.")
    except Exception as e:
        print(f"Warning: Failed to resize icon with PIL: {e}. Using original.")
        shutil.copy("app_icon.png", "hevc_shrinker.png")

    # Set up environment variables
    env = os.environ.copy()
    env["QMAKE"] = "/usr/lib64/qt6/bin/qmake"
    env["NO_STRIP"] = "1"
    
    # Build the AppImage
    cmd = [
        os.path.abspath(ld_path),
        "--appdir", "AppDir",
        "-e", "build-linux/hevc_shrinker",
        "-d", "hevc_shrinker.desktop",
        "-i", "hevc_shrinker.png",
        "--plugin", "qt",
        "--output", "appimage"
    ]
    
    # Clean old AppDir
    if os.path.exists("AppDir"):
        shutil.rmtree("AppDir")
        
    print("Running linuxdeploy to build AppImage...")
    res = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    
    # Clean up resized icon
    if os.path.exists("hevc_shrinker.png"):
        os.remove("hevc_shrinker.png")
    
    # Move compiled AppImage to dist/
    found = False
    for f in os.listdir("."):
        if f.endswith(".AppImage") and f.startswith("HEVC_Video_Shrinker"):
            dest = os.path.join("dist", "HEVC_Video_Shrinker-2.0.3-x86_64.AppImage")
            if os.path.exists(dest):
                os.remove(dest)
            shutil.move(f, dest)
            print(f"AppImage successfully built: {dest}")
            found = True
            break
            
    if not found:
        print(f"AppImage build failed. stdout: {res.stdout}\nstderr: {res.stderr}")
        return False
    return True

def package_deb():
    print("\n=== 3. Packaging Debian (.deb) ===")
    os.makedirs("dist", exist_ok=True)
    
    # Setup paths
    pkg_dir = "pack-deb-tmp"
    if os.path.exists(pkg_dir):
        shutil.rmtree(pkg_dir)
        
    os.makedirs(f"{pkg_dir}/DEBIAN")
    os.makedirs(f"{pkg_dir}/usr/bin")
    os.makedirs(f"{pkg_dir}/usr/share/applications")
    os.makedirs(f"{pkg_dir}/usr/share/icons/hicolor/512x512/apps")
    
    # Copy files
    shutil.copy("build-linux/hevc_shrinker", f"{pkg_dir}/usr/bin/")
    shutil.copy("app_icon.png", f"{pkg_dir}/usr/share/icons/hicolor/512x512/apps/hevc_shrinker.png")
    
    # Create desktop launcher
    desktop_content = """[Desktop Entry]
Type=Application
Name=HEVC Video Shrinker
Comment=Batch-shrink video libraries into efficient H.265 files
Exec=/usr/bin/hevc_shrinker
Icon=hevc_shrinker
Categories=Utility;AudioVideo;Video;
Terminal=false
"""
    with open(f"{pkg_dir}/usr/share/applications/hevc_shrinker.desktop", "w") as f:
        f.write(desktop_content)
        
    # Create control file
    control_content = """Package: hevc-shrinker
Version: 2.0.3-1
Section: utils
Priority: optional
Architecture: amd64
Depends: libqt6widgets6, libqt6sql6-sqlite, ffmpeg
Maintainer: HEVC Video Shrinker Developers <developer@example.com>
Description: A native batch video compressor.
 Shrinks video libraries into optimized H.265 (HEVC) files
 using FFmpeg and stores processing metrics in SQLite.
"""
    with open(f"{pkg_dir}/DEBIAN/control", "w") as f:
        f.write(control_content)
        
    # Write debian-binary marker
    with open("debian-binary", "w") as f:
        f.write("2.0\n")
        
    # Compile control.tar.gz
    with tarfile.open("control.tar.gz", "w:gz") as tar:
        # control file must be at root of tarball
        tar.add(f"{pkg_dir}/DEBIAN/control", arcname="control")
        
    # Compile data.tar.xz
    with tarfile.open("data.tar.xz", "w:xz") as tar:
        tar.add(f"{pkg_dir}/usr", arcname="usr")
        
    # Build package using standard ar (dpkg-deb is not required!)
    deb_out = "dist/hevc-shrinker_2.0.3-1_amd64.deb"
    if os.path.exists(deb_out):
        os.remove(deb_out)
        
    # ar requires specific file ordering: debian-binary, control.tar.gz, data.tar.xz
    cmd = ["ar", "rcs", deb_out, "debian-binary", "control.tar.gz", "data.tar.xz"]
    if run_cmd(cmd):
        print(f"Debian package successfully built: {deb_out}")
    else:
        print("Failed to build Debian package.")
        
    # Clean up temp archive files
    for f in ["debian-binary", "control.tar.gz", "data.tar.xz"]:
        if os.path.exists(f):
            os.remove(f)
    shutil.rmtree(pkg_dir)

def package_rpm():
    print("\n=== 4. Packaging Red Hat (.rpm) ===")
    
    # Check if rpmbuild is available
    if not shutil.which("rpmbuild"):
        print("[NOTICE] 'rpmbuild' tool is missing. Skipping RPM packaging.")
        print("To enable RPM support on Gentoo, run: sudo emerge app-arch/rpm")
        return
        
    home = os.path.expanduser("~")
    rpmbuild_dir = os.path.join(home, "rpmbuild")
    for sub in ["BUILD", "RPMS", "SOURCES", "SPECS", "SRPMS"]:
        os.makedirs(os.path.join(rpmbuild_dir, sub), exist_ok=True)
        
    # Copy build items to SOURCES
    shutil.copy("build-linux/hevc_shrinker", os.path.join(rpmbuild_dir, "SOURCES"))
    shutil.copy("hevc_shrinker.desktop", os.path.join(rpmbuild_dir, "SOURCES"))
    shutil.copy("app_icon.png", os.path.join(rpmbuild_dir, "SOURCES"))
    
    # Spec file contents
    spec_content = """Name:           hevc_shrinker
Version:        2.0.3
Release:        1%{?dist}
Summary:        Batch video compressor using HEVC H.265.

License:        GPLv3
URL:            https://github.com/yourusername/hevc_shrinker
BuildArch:      x86_64
Requires:       qt6-qtbase, ffmpeg

%description
A native batch video compressor that shrinks video libraries into H.265 format using FFmpeg.

%prep
# No source unpacking needed

%build
# Pre-compiled binary, no building inside rpmbuild

%install
mkdir -p %{buildroot}%{_bindir}
mkdir -p %{buildroot}%{_datadir}/applications
mkdir -p %{buildroot}%{_datadir}/icons/hicolor/512x512/apps

cp %{_sourcedir}/hevc_shrinker %{buildroot}%{_bindir}/hevc_shrinker
cp %{_sourcedir}/hevc_shrinker.desktop %{buildroot}%{_datadir}/applications/hevc_shrinker.desktop
cp %{_sourcedir}/app_icon.png %{buildroot}%{_datadir}/icons/hicolor/512x512/apps/hevc_shrinker.png

%files
%{_bindir}/hevc_shrinker
%{_datadir}/applications/hevc_shrinker.desktop
%{_datadir}/icons/hicolor/512x512/apps/hevc_shrinker.png

%changelog
* Tue Jul 28 2026 HEVC Video Shrinker Developers <developer@example.com> - 2.0.3-1
- Release version 2.0.3 with aspect ratio / anamorphic scaling fixes
* Fri Jul 17 2026 HEVC Video Shrinker Developers <developer@example.com> - 2.0.2-1
- Release version 2.0.2 with play video context path fix
* Wed Jul 15 2026 HEVC Video Shrinker Developers <developer@example.com> - 2.0.1-1
- Release version 2.0.1 with aq-mode=1
* Sun Jul 05 2026 HEVC Video Shrinker Developers <developer@example.com> - 2.0-1
- Initial C++ package build
"""
    spec_path = os.path.join(rpmbuild_dir, "SPECS", "hevc_shrinker.spec")
    with open(spec_path, "w") as f:
        f.write(spec_content)
        
    # Run rpmbuild
    if run_cmd(["rpmbuild", "-bb", spec_path]):
        rpm_out_dir = os.path.join(rpmbuild_dir, "RPMS", "x86_64")
        for f in os.listdir(rpm_out_dir):
            if f.endswith(".rpm"):
                shutil.copy(os.path.join(rpm_out_dir, f), "dist")
                print(f"RPM package successfully built: dist/{f}")
    else:
        print("Failed to build RPM package.")

if __name__ == "__main__":
    if not compile_project():
        print("Compilation failed. Aborting packaging.")
        sys.exit(1)
    
    package_appimage()
    package_deb()
    package_rpm()
    print("\nPackaging run complete. Check the 'dist/' folder!")
