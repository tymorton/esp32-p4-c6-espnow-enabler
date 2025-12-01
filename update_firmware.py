#!/usr/bin/env python3
import os
import subprocess
import sys
import shutil

# Configuration
REPO_URL = "https://github.com/espressif/esp-hosted.git"
REPO_DIR = os.path.expanduser("~/esp-hosted-repo")
SLAVE_PROJECT_PATH = "esp_hosted_ng/esp/esp_driver/network_adapter"
OTA_HOST_PROJECT = os.path.expanduser("~/esp32-p4-c6-ota")
ENABLER_PROJECT = os.path.expanduser("~/projects/esp32-p4-examples/esp32-p4-c6-espnow-enabler")
SLAVE_SDKCONFIG_DEFAULTS = os.path.expanduser("~/slave/sdkconfig.defaults")

def run_cmd(cmd, cwd=None, exit_on_error=True):
    print(f"Running: {cmd}")
    try:
        subprocess.check_call(cmd, shell=True, cwd=cwd)
    except subprocess.CalledProcessError as e:
        print(f"Error running command: {cmd}")
        if exit_on_error:
            sys.exit(1)
        return False
    return True

def setup_repo():
    if not os.path.exists(REPO_DIR):
        print(f"Cloning {REPO_URL} to {REPO_DIR}...")
        run_cmd(f"git clone {REPO_URL} {REPO_DIR}")
    else:
        print(f"Fetching latest tags in {REPO_DIR}...")
        run_cmd("git fetch --tags", cwd=REPO_DIR)

def list_versions():
    # List NG versions
    cmd = "git tag -l 'release/ng-v*'"
    output = subprocess.check_output(cmd, shell=True, cwd=REPO_DIR).decode("utf-8")
    tags = [t for t in output.splitlines() if t.strip()]
    # Sort tags (simple sort might be enough, or use packaging.version)
    tags.sort(reverse=True)
    return tags

def build_firmware(tag):
    print(f"Checking out {tag}...")
    run_cmd(f"git checkout {tag}", cwd=REPO_DIR)
    
    target_dir = os.path.join(REPO_DIR, SLAVE_PROJECT_PATH)
    if not os.path.exists(target_dir):
        print(f"Error: Slave project path {target_dir} does not exist in this version.")
        sys.exit(1)
        
    print("Applying ESP-NOW configuration...")
    # Read our defaults
    with open(SLAVE_SDKCONFIG_DEFAULTS, "r") as f:
        defaults = f.read()
    
    # Append to repo defaults
    repo_defaults_path = os.path.join(target_dir, "sdkconfig.defaults")
    with open(repo_defaults_path, "a") as f:
        f.write("\n# --- Added by Enabler Script ---\n")
        f.write(defaults)
        
    print("Building C6 Firmware...")
    # Source export.sh is tricky in python subprocess. We assume idf.py is in PATH or user sourced it.
    # But better to try to source it if we know where it is.
    # Assuming user runs this from a shell with IDF exported.
    
    # Clean build
    run_cmd("rm -rf build", cwd=target_dir)
    run_cmd("idf.py set-target esp32c6", cwd=target_dir)
    run_cmd("idf.py build", cwd=target_dir)
    
    bin_path = os.path.join(target_dir, "build", "network_adapter.bin")
    if not os.path.exists(bin_path):
        print("Error: Build failed, network_adapter.bin not found.")
        sys.exit(1)
        
    return bin_path

def repackage_storage(bin_path):
    print("Updating OTA Host payload...")
    dest_dir = os.path.join(OTA_HOST_PROJECT, "components", "ota_littlefs", "slave_fw_bin")
    dest_path = os.path.join(dest_dir, "network_adapter.bin")
    
    if not os.path.exists(dest_dir):
        os.makedirs(dest_dir)
        
    shutil.copy2(bin_path, dest_path)
    print(f"Copied firmware to {dest_path}")
    
    print("Rebuilding OTA Host Storage Image...")
    # We only need to build the storage image, but 'idf.py build' is safest
    run_cmd("idf.py build", cwd=OTA_HOST_PROJECT)
    
    storage_path = os.path.join(OTA_HOST_PROJECT, "build", "storage.bin")
    if not os.path.exists(storage_path):
        print("Error: Storage image build failed.")
        sys.exit(1)
        
    return storage_path

def deploy_binaries(storage_path):
    print("Deploying new binaries to Enabler...")
    dest = os.path.join(ENABLER_PROJECT, "binaries", "storage.bin")
    shutil.copy2(storage_path, dest)
    print(f"Updated {dest}")

def main():
    print("=== ESP32-C6 Firmware Updater ===")
    
    # Check for IDF
    if shutil.which("idf.py") is None:
        print("Error: idf.py not found. Please export ESP-IDF environment variables.")
        sys.exit(1)
        
    setup_repo()
    tags = list_versions()
    
    print("\nAvailable Versions:")
    for i, tag in enumerate(tags):
        print(f"{i+1}. {tag}")
        
    choice = input("\nSelect version to install (number): ")
    try:
        idx = int(choice) - 1
        if idx < 0 or idx >= len(tags):
            raise ValueError
        selected_tag = tags[idx]
    except ValueError:
        print("Invalid selection.")
        sys.exit(1)
        
    print(f"\nSelected: {selected_tag}")
    
    bin_path = build_firmware(selected_tag)
    storage_path = repackage_storage(bin_path)
    deploy_binaries(storage_path)
    
    print("\n✅ Update Complete!")
    print(f"The 'binaries/storage.bin' file has been updated with version {selected_tag}.")
    print("Run './flash_c6_firmware.sh' to flash it to your board.")

if __name__ == "__main__":
    main()
