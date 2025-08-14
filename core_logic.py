import sys
import pathlib
import requests
import json
import re
import tarfile
import shutil
from packaging.version import parse as parse_semver

# (All helper and public functions from the previous version remain here...)
# --- Configuration ---
GITHUB_API_URL = "https://api.github.com/repos/MixedRealityToolkit/MixedRealityToolkit-Unity/releases"
GRAPHICS_TOOLS_URL_FORMAT = "https://github.com/microsoft/MixedReality-GraphicsTools-Unity/archive/refs/tags/v{}.tar.gz"

class UnityVersion:
    """A custom class to parse and compare Unity-specific version strings."""
    def __init__(self, version_str="0.0.0f0"):
        self.major, self.minor, self.patch, self.type, self.build = 0, 0, 0, 'f', 0
        match = re.match(r"(\d+)\.(\d+)\.(\d+)([abfp])(\d+)", version_str)
        if match:
            self.major, self.minor, self.patch, self.type, self.build = (
                int(match.group(1)), int(match.group(2)), int(match.group(3)),
                match.group(4), int(match.group(5)),
            )
        elif re.match(r"(\d+)\.(\d+)\.(\d+)", version_str):
             self.major, self.minor, self.patch = map(int, version_str.split('.'))

    def _to_tuple(self):
        return (self.major, self.minor, self.patch, self.type, self.build)
    def __gt__(self, other): return self._to_tuple() > other._to_tuple()
    def __ge__(self, other): return self._to_tuple() >= other._to_tuple()

def _download_file(url: str, dest_folder: pathlib.Path, progress_callback=None):
    filename = url.split('/')[-1]
    dest_path = dest_folder / filename
    if progress_callback: progress_callback(f"Downloading {filename}...")
    else: print(f"Downloading {filename}...")
    try:
        with requests.get(url, stream=True) as r:
            r.raise_for_status()
            with open(dest_path, 'wb') as f:
                for chunk in r.iter_content(chunk_size=8192): f.write(chunk)
        return dest_path
    except requests.exceptions.RequestException as e:
        message = f"Error downloading {url}: {e}"
        if progress_callback: progress_callback(message)
        else: print(message)
        return None

def _get_dependencies_from_tgz(tgz_path: pathlib.Path) -> dict:
    dependencies = {}
    try:
        with tarfile.open(tgz_path, "r:gz") as tar:
            for member in tar.getmembers():
                if pathlib.Path(member.name).name == 'package.json' and 'package/package.json' in member.name:
                    f = tar.extractfile(member)
                    if f:
                        content = json.load(f)
                        dependencies = content.get("dependencies", {})
                        break
    except Exception as e:
        print(f"Could not read dependencies from {tgz_path}: {e}")
    return dependencies

def _extract_and_repackage_graphics_tools(tgz_path: pathlib.Path, version: str, progress_callback=None):
    message = f"Repackaging graphics tools v{version}..."
    if progress_callback: progress_callback(message)
    else: print(message)

    temp_extract_dir = pathlib.Path("./temp_graphics_extract")
    package_dir = pathlib.Path("./package")
    if temp_extract_dir.exists(): shutil.rmtree(temp_extract_dir)
    temp_extract_dir.mkdir()
    try:
        # After
        with tarfile.open(tgz_path, "r:gz") as tar: tar.extractall(path=temp_extract_dir, filter='data')
        source_package_path = temp_extract_dir / f"MixedReality-GraphicsTools-Unity-{version}" / "com.microsoft.mrtk.graphicstools.unity"
        if not source_package_path.exists():
            message = f"Error: Could not find graphics tools package inside {source_package_path}"
            if progress_callback: progress_callback(message)
            else: print(message)
            return
        if package_dir.exists(): shutil.rmtree(package_dir)
        shutil.move(str(source_package_path), str(package_dir))
        output_tgz_name = f"com.microsoft.mrtk.graphicstools.unity-{version}.tgz"
        with tarfile.open(output_tgz_name, "w:gz") as tar: tar.add(package_dir, arcname="package")
    finally:
        if temp_extract_dir.exists(): shutil.rmtree(temp_extract_dir)
        if package_dir.exists(): shutil.rmtree(package_dir)
        if tgz_path.exists(): tgz_path.unlink()

def _find_download_url(component_name: str, version: str, mrtk_releases_json: dict) -> str:
    asset_filename_re = re.compile(f"org\\.mixedrealitytoolkit\\.{re.escape(component_name)}-{re.escape(version)}\\.tgz")
    for release in mrtk_releases_json:
        for asset in release.get("assets", []):
            if asset_filename_re.match(asset.get("name", "")):
                return asset.get("browser_download_url", "")
    return ""

def _resolve_dependencies_recursively(component_name: str, version: str, mrtk_releases_json: dict, resolved_packages: dict, progress_callback=None):
    if component_name in resolved_packages and parse_semver(resolved_packages[component_name]) >= parse_semver(version):
        return
    message = f"  Resolving {component_name} v{version}..."
    if progress_callback: progress_callback(message)
    else: print(message)

    resolved_packages[component_name] = version
    temp_dir = pathlib.Path("./temp_resolve")
    temp_dir.mkdir(exist_ok=True)
    download_url = _find_download_url(component_name, version, mrtk_releases_json)
    if not download_url: return
    downloaded_path = _download_file(download_url, temp_dir)
    if not downloaded_path: return
    dependencies = _get_dependencies_from_tgz(downloaded_path)
    downloaded_path.unlink()
    for dep_name, dep_version in dependencies.items():
        if dep_name.startswith("org.mixedrealitytoolkit"):
            dep_component_name = dep_name[24:]
            _resolve_dependencies_recursively(dep_component_name, dep_version, mrtk_releases_json, resolved_packages, progress_callback)
        elif dep_name == "com.microsoft.mrtk.graphicstools.unity":
             if dep_name not in resolved_packages or parse_semver(dep_version) > parse_semver(resolved_packages[dep_name]):
                resolved_packages[dep_name] = dep_version

def get_unity_version(project_path: pathlib.Path) -> str:
    version_file = project_path / "ProjectSettings" / "ProjectVersion.txt"
    if not version_file.exists(): return ""
    try:
        content = version_file.read_text()
        match = re.search(r"m_EditorVersion:\s*(.*)", content)
        return match.group(1).strip() if match else ""
    except Exception as e:
        print(f"Error reading Unity version file: {e}")
        return ""

def discover_mrtk_components():
    print("Fetching MRTK release information from GitHub...")
    mrtk_components = {}
    try:
        response = requests.get(GITHUB_API_URL)
        response.raise_for_status()
        mrtk_releases_json = response.json()
        component_info_re = re.compile(r"org\.mixedrealitytoolkit\.(.+?)-(.+?)\.tgz")
        for release in mrtk_releases_json:
            for asset in release.get("assets", []):
                match = component_info_re.match(asset.get("name", ""))
                if match:
                    name, version = match.groups()
                    if name not in mrtk_components: mrtk_components[name] = []
                    mrtk_components[name].append(version)
        for name in mrtk_components:
            mrtk_components[name].sort(key=parse_semver, reverse=True)
        return mrtk_components, mrtk_releases_json
    except requests.exceptions.RequestException as e:
        print(f"Failed to get data from GitHub API: {e}")
        return {}, None

def resolve_dependencies(selections: dict, mrtk_releases_json: dict, progress_callback=None):
    message = "--- Resolving all dependencies ---"
    if progress_callback: progress_callback(message)
    else: print(message)
    resolved_packages = {}
    for component_name, version in selections.items():
        _resolve_dependencies_recursively(component_name, version, mrtk_releases_json, resolved_packages, progress_callback)
    temp_dir = pathlib.Path("./temp_resolve")
    if temp_dir.exists(): shutil.rmtree(temp_dir)
    message = "--- Resolution complete ---"
    if progress_callback: progress_callback(message)
    else: print(message)
    return resolved_packages

def download_and_apply_packages(project_path: pathlib.Path, resolved_packages: dict, mrtk_releases_json: dict, selected_openxr: set, progress_callback=None):
    message = "--- Downloading final packages ---"
    if progress_callback: progress_callback(message)
    else: print(message)
    for component_name, version in resolved_packages.items():
        if component_name.startswith("com.microsoft.mrtk.graphicstools.unity"):
            graphics_tgz_path = _download_file(GRAPHICS_TOOLS_URL_FORMAT.format(version), pathlib.Path('.'), progress_callback)
            if graphics_tgz_path: _extract_and_repackage_graphics_tools(graphics_tgz_path, version, progress_callback)
        else:
            download_url = _find_download_url(component_name, version, mrtk_releases_json)
            if download_url: _download_file(download_url, pathlib.Path('.'), progress_callback)
            else:
                message = f"ERROR: Could not find final download URL for {component_name} v{version}"
                if progress_callback: progress_callback(message)
                else: print(message)

    message = "--- Applying changes to project ---"
    if progress_callback: progress_callback(message)
    else: print(message)

    mixed_reality_dir = project_path / "Packages" / "MixedReality"
    if mixed_reality_dir.exists(): shutil.rmtree(mixed_reality_dir)
    mixed_reality_dir.mkdir(parents=True)
    downloaded_files = list(pathlib.Path('.').glob("*.tgz"))
    for tgz_file in downloaded_files: shutil.move(str(tgz_file), str(mixed_reality_dir))

    manifest_path = project_path / "Packages" / "manifest.json"
    try:
        with open(manifest_path, 'r') as f: manifest_data = json.load(f)
        if "dependencies" not in manifest_data: manifest_data["dependencies"] = {}
        for tgz_file in downloaded_files:
            component_name_match = re.match(r"(.+?)-[0-9].*\.tgz", tgz_file.name)
            if component_name_match:
                manifest_data["dependencies"][component_name_match.group(1)] = f"file:MixedReality/{tgz_file.name}"
        if "com.microsoft.mixedreality.openxr" in selected_openxr:
            manifest_data["dependencies"]["com.microsoft.mixedreality.openxr"] = "1.11.2"
        if "com.unity.xr.meta-openxr" in selected_openxr:
            unity_version_str = get_unity_version(project_path)
            if unity_version_str:
                current_v = UnityVersion(unity_version_str)
                if current_v >= UnityVersion("6000.0.0f0"): manifest_data["dependencies"]["com.unity.xr.meta-openxr"] = "2.2.0"
                elif current_v > UnityVersion("2022.3.0f1"): manifest_data["dependencies"]["com.unity.xr.meta-openxr"] = "1.0.4"
        with open(manifest_path, 'w') as f: json.dump(manifest_data, f, indent=4)
        message = f"✅ Successfully updated manifest.json!"
        if progress_callback: progress_callback(message)
        else: print(message)
        return True
    except Exception as e:
        message = f"Error applying changes: {e}"
        if progress_callback: progress_callback(message)
        else: print(message)
        return False

# --- Main function for standalone CLI execution ---
def main_cli():
    """The main function for running the tool from the command line."""
    if len(sys.argv) < 2:
        print("Usage: python core_logic.py <path_to_unity_project>")
        sys.exit(1)
    project_path = pathlib.Path(sys.argv[1])
    if not (project_path.is_dir() and (project_path / "Assets").exists() and (project_path / "Packages").exists()):
        print(f"Error: '{project_path}' is not a valid Unity project directory.")
        sys.exit(1)
    unity_version_str = get_unity_version(project_path)
    if unity_version_str: print(f"✅ Detected Unity Version: {unity_version_str}")
    else: print("⚠️ Warning: Could not detect Unity version.")
    mrtk_components, mrtk_releases_json = discover_mrtk_components()
    if not mrtk_components: sys.exit(1)
    
    class SelectablePackage:
        def __init__(self, display_name, identifier, pkg_type):
            self.display_name, self.identifier, self.type = display_name, identifier, pkg_type
    all_packages = []
    for name in sorted(mrtk_components.keys()):
        all_packages.append(SelectablePackage(name, name, 'mrtk'))
    all_packages.append(SelectablePackage("Microsoft Mixed Reality OpenXR", "com.microsoft.mixedreality.openxr", 'openxr'))
    all_packages.append(SelectablePackage("Meta OpenXR", "com.unity.xr.meta-openxr", 'openxr'))
    
    print("\nAvailable MRTK Components:\n" + "-"*24)
    idx = 0
    for pkg in all_packages:
        if pkg.type == 'mrtk':
            print(f"[{idx}] {pkg.display_name}")
            idx += 1
    print("\nOpenXR Components:\n" + "-"*24)
    for pkg in all_packages:
        if pkg.type == 'openxr':
            print(f"[{idx}] {pkg.display_name}")
            idx += 1
    try:
        selection_str = input("\nEnter the index numbers of packages to install (e.g., 2 7 14): ")
        selected_indices = {int(i) for i in selection_str.split()}
    except ValueError:
        print("Invalid input. Please enter numbers separated by spaces.")
        sys.exit(1)
    if not selected_indices:
        print("No packages selected. Exiting.")
        sys.exit(0)
    
    mrtk_selections = {}
    openxr_selections = set()
    for idx in selected_indices:
        if 0 <= idx < len(all_packages):
            pkg = all_packages[idx]
            if pkg.type == 'mrtk':
                latest_version = mrtk_components[pkg.identifier][0]
                mrtk_selections[pkg.identifier] = latest_version
            elif pkg.type == 'openxr':
                openxr_selections.add(pkg.identifier)
        else:
            print(f"Invalid index: {idx}. Skipping.")
    
    if mrtk_selections:
        resolved = resolve_dependencies(mrtk_selections, mrtk_releases_json)
        download_and_apply_packages(project_path, resolved, mrtk_releases_json, openxr_selections)
    elif openxr_selections: # Handle case where only OpenXR packages are selected
        download_and_apply_packages(project_path, {}, {}, openxr_selections)

if __name__ == "__main__":
    main_cli()