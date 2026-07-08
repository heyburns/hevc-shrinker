import os
import shutil
import zipfile

def package_portable():
    build_dir = "build"
    dist_dir = "dist"
    portable_name = "hevc_shrinker_portable_1.0_win64"
    temp_dir = os.path.join(dist_dir, portable_name)
    zip_path = os.path.join(dist_dir, f"{portable_name}.zip")
    
    # 1. Clean up old structures
    if os.path.exists(temp_dir):
        shutil.rmtree(temp_dir)
    if os.path.exists(zip_path):
        os.remove(zip_path)
        
    os.makedirs(temp_dir, exist_ok=True)
    
    # 2. Files and folder list to copy
    # We copy hevc_shrinker.exe, app_icon.png, and all DLLs / subdirectories
    print("Copying build files...")
    for item in os.listdir(build_dir):
        item_path = os.path.join(build_dir, item)
        dest_path = os.path.join(temp_dir, item)
        
        # Skip cmake files, temporary build folders, and other non-deployed files
        if item in ["CMakeFiles", "cmake_install.cmake", "CMakeCache.txt", "Makefile"]:
            continue
            
        if os.path.isdir(item_path):
            shutil.copytree(item_path, dest_path)
        else:
            shutil.copy2(item_path, dest_path)
            
    # 3. Create zip archive
    print(f"Creating portable zip archive at: {zip_path}")
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for root, dirs, files in os.walk(temp_dir):
            for file in files:
                filepath = os.path.join(root, file)
                # Store files relative to the portable parent folder
                arcname = os.path.relpath(filepath, dist_dir)
                zipf.write(filepath, arcname)
                
    # 4. Clean up temp folder
    shutil.rmtree(temp_dir)
    print("Portable package successfully created!")

if __name__ == "__main__":
    # Ensure we run relative to the script location
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    
    if os.path.exists("build"):
        package_portable()
    else:
        print("Error: build directory not found. Please compile the project first.")
