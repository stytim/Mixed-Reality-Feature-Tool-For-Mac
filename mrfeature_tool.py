import sys
import os
import pathlib
import requests
import json
import re
import tarfile
import shutil
from packaging.version import parse as parse_semver

# --- Configuration ---
GITHUB_API_URL = "https://api.github.com/repos/MixedRealityToolkit/MixedRealityToolkit-Unity/releases"
GRAPHICS_TOOLS_URL_FORMAT = "https://github.com/microsoft/MixedReality-GraphicsTools-Unity/archive/refs/tags/v{}.tar.gz"


# --- START: Corrected Unity Version Handling ---
class UnityVersion:
    """A custom class to parse and compare Unity-specific version strings."""
    def __init__(self, version_str="0.0.0f0"):
        self.major, self.minor, self.patch, self.type, self.build = 0, 0, 0, 'f', 0
        # Regex to capture major, minor, patch, type (a,b,f,p), and build number
        match = re.match(r"(\d+)\.(\d+)\.(\d+)([abfp])(\d+)", version_str)
        if match:
            self.major, self.minor, self.patch, self.type, self.build = (
                int(match.group(1)),
                int(match.group(2)),
                int(match.group(3)),
                match.group(4),
                int(match.group(5)),
            )
        # Fallback for simpler versions like "6000.0.0"
        elif re.match(r"(\d+)\.(\d+)\.(\d+)", version_str):
             self.major, self.minor, self.patch = map(int, version_str.split('.'))


    def _to_tuple(self):
        """Converts version to a comparable tuple."""
        # Simple character comparison works for 'f' > 'p' > 'b' > 'a'
        return (self.major, self.minor, self.patch, self.type, self.build)

    def __gt__(self, other):
        return self._to_tuple() > other._to_tuple()

    def __ge__(self, other):
        return self._to_tuple() >= other._to_tuple()

# --- END: Corrected Unity Version Handling ---


class SelectablePackage:
    """A simple class to hold information about a selectable package."""
    def __init__(self, display_name, identifier, pkg_type):
        self.display_name = display_name
        self.identifier = identifier
        self.type = pkg_type # 'mrtk' or 'openxr'


def get_unity_version(project_path: pathlib.Path) -> str:
    """Reads the Unity version from ProjectSettings/ProjectVersion.txt."""
    version_file = project_path / "ProjectSettings" / "ProjectVersion.txt"
    if not version_file.exists():
        return ""
    try:
        content = version_file.read_text()
        match = re.search(r"m_EditorVersion:\s*(.*)", content)
        if match:
            return match.group(1).strip()
    except Exception as e:
        print(f"Error reading Unity version file: {e}")
    return ""


def download_file(url: str, dest_folder: pathlib.Path):
    """Downloads a file from a URL into a destination folder."""
    filename = url.split('/')[-1]
    dest_path = dest_folder / filename
    print(f"Downloading {filename}...")
    try:
        with requests.get(url, stream=True) as r:
            r.raise_for_status()
            with open(dest_path, 'wb') as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)
        return dest_path
    except requests.exceptions.RequestException as e:
        print(f"Error downloading {url}: {e}")
        return None


def get_dependencies_from_tgz(tgz_path: pathlib.Path) -> dict:
    """Reads and parses package.json from within a .tgz file without full extraction."""
    dependencies = {}
    try:
        with tarfile.open(tgz_path, "r:gz") as tar:
            for member in tar.getmembers():
                if pathlib.Path(member.name).name == 'package.json':
                    if 'package/package.json' in member.name:
                        f = tar.extractfile(member)
                        if f:
                            content = json.load(f)
                            if "dependencies" in content:
                                dependencies = content["dependencies"]
                            break
    except (tarfile.TarError, json.JSONDecodeError, KeyError) as e:
        print(f"Could not read dependencies from {tgz_path}: {e}")
    return dependencies


def extract_and_repackage_graphics_tools(tgz_path: pathlib.Path, version: str):
    """Extracts the graphics tools source, finds the Unity package, and re-archives it."""
    print(f"Repackaging graphics tools v{version}...")
    temp_extract_dir = pathlib.Path("./temp_graphics_extract")
    package_dir = pathlib.Path("./package")

    if temp_extract_dir.exists():
        shutil.rmtree(temp_extract_dir)
    temp_extract_dir.mkdir()

    try:
        with tarfile.open(tgz_path, "r:gz") as tar:
            tar.extractall(path=temp_extract_dir)

        source_package_path = temp_extract_dir / f"MixedReality-GraphicsTools-Unity-{version}" / "com.microsoft.mrtk.graphicstools.unity"

        if not source_package_path.exists():
            print(f"Error: Could not find graphics tools package inside {source_package_path}")
            return

        if package_dir.exists():
            shutil.rmtree(package_dir)
        shutil.move(str(source_package_path), str(package_dir))

        output_tgz_name = f"com.microsoft.mrtk.graphicstools.unity-{version}.tgz"
        with tarfile.open(output_tgz_name, "w:gz") as tar:
            tar.add(package_dir, arcname="package")

    finally:
        if temp_extract_dir.exists():
            shutil.rmtree(temp_extract_dir)
        if package_dir.exists():
            shutil.rmtree(package_dir)
        if tgz_path.exists():
            tgz_path.unlink()


def process_mrtk_component(component_name: str, version: str, mrtk_releases_json: dict, downloaded_components: dict):
    """
    Recursively downloads an MRTK component and its dependencies.
    """
    component_key = f"{component_name}-{version}"
    if component_key in downloaded_components:
        return

    download_url = ""
    asset_filename_re = re.compile(f"org\\.mixedrealitytoolkit\\.{re.escape(component_name)}-{re.escape(version)}\\.tgz")
    for release in mrtk_releases_json:
        for asset in release.get("assets", []):
            if asset_filename_re.match(asset.get("name", "")):
                download_url = asset.get("browser_download_url")
                break
        if download_url:
            break

    if not download_url:
        print(f"Warning: Could not find download URL for {component_name} v{version}")
        return

    downloaded_path = download_file(download_url, pathlib.Path('.'))
    if not downloaded_path:
        return

    downloaded_components[component_key] = True

    dependencies = get_dependencies_from_tgz(downloaded_path)
    for dep_name, dep_version in dependencies.items():
        print(f"  Found dependency: {dep_name} v{dep_version}")
        if dep_name.startswith("org.mixedrealitytoolkit"):
            dep_component_name = dep_name[24:]
            process_mrtk_component(dep_component_name, dep_version, mrtk_releases_json, downloaded_components)
        elif dep_name == "com.microsoft.mrtk.graphicstools.unity":
            graphics_tgz_path = download_file(GRAPHICS_TOOLS_URL_FORMAT.format(dep_version), pathlib.Path('.'))
            if graphics_tgz_path:
                extract_and_repackage_graphics_tools(graphics_tgz_path, dep_version)


def main():
    """Main function to run the tool."""
    if len(sys.argv) < 2:
        print("Usage: python mrfeature_tool.py <path_to_unity_project>")
        sys.exit(1)

    project_path = pathlib.Path(sys.argv[1])
    if not (project_path.is_dir() and (project_path / "Assets").exists() and (project_path / "Packages").exists()):
        print(f"Error: '{project_path}' is not a valid Unity project directory.")
        sys.exit(1)

    unity_version_str = get_unity_version(project_path)
    if unity_version_str:
        print(f"✅ Detected Unity Version: {unity_version_str}")
    else:
        print("⚠️ Warning: Could not detect Unity version. Meta OpenXR logic may not work correctly.")

    print("Fetching MRTK release information from GitHub...")
    try:
        response = requests.get(GITHUB_API_URL)
        response.raise_for_status()
        mrtk_releases_json = response.json()
    except requests.exceptions.RequestException as e:
        print(f"Failed to get data from GitHub API: {e}")
        sys.exit(1)

    mrtk_components = {}
    component_info_re = re.compile(r"org\.mixedrealitytoolkit\.(.+?)-(.+?)\.tgz")
    for release in mrtk_releases_json:
        for asset in release.get("assets", []):
            match = component_info_re.match(asset.get("name", ""))
            if match:
                name, version = match.groups()
                if name not in mrtk_components:
                    mrtk_components[name] = []
                mrtk_components[name].append(version)

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

    downloaded_mrtk_components = {}
    selected_openxr_packages = set()

    for idx in selected_indices:
        if idx < 0 or idx >= len(all_packages):
            print(f"Invalid index: {idx}. Skipping.")
            continue

        pkg = all_packages[idx]
        if pkg.type == 'mrtk':
            versions = sorted(mrtk_components[pkg.identifier], key=parse_semver, reverse=True)
            latest_version = versions[0]
            print(f"\n⚙️ Processing {pkg.display_name} (latest: v{latest_version})...")
            process_mrtk_component(pkg.identifier, latest_version, mrtk_releases_json, downloaded_mrtk_components)

        elif pkg.type == 'openxr':
            print(f"\n🗒️ Queueing {pkg.display_name} for manifest update.")
            selected_openxr_packages.add(pkg.identifier)

    mixed_reality_dir = project_path / "Packages" / "MixedReality"
    if mixed_reality_dir.exists():
        shutil.rmtree(mixed_reality_dir)
    mixed_reality_dir.mkdir(parents=True)

    for tgz_file in pathlib.Path('.').glob("*.tgz"):
        shutil.move(str(tgz_file), str(mixed_reality_dir))

    manifest_path = project_path / "Packages" / "manifest.json"
    try:
        with open(manifest_path, 'r') as f:
            manifest_data = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Error reading manifest.json: {e}")
        sys.exit(1)

    if "dependencies" not in manifest_data:
        manifest_data["dependencies"] = {}

    for tgz_file in mixed_reality_dir.glob("*.tgz"):
        component_name_match = re.match(r"(.+?)-[0-9].*\.tgz", tgz_file.name)
        if component_name_match:
            component_name = component_name_match.group(1)
            manifest_data["dependencies"][component_name] = f"file:MixedReality/{tgz_file.name}"

    if selected_openxr_packages:
        print("Adding selected OpenXR packages to manifest...")
        if "com.microsoft.mixedreality.openxr" in selected_openxr_packages:
            manifest_data["dependencies"]["com.microsoft.mixedreality.openxr"] = "1.11.2"

        if "com.unity.xr.meta-openxr" in selected_openxr_packages:
            if not unity_version_str:
                print("⚠️ Warning: Skipping Meta OpenXR because Unity version is unknown.")
            else:
                current_v = UnityVersion(unity_version_str)
                if current_v >= UnityVersion("6000.0.0f0"):
                    # NOTE: This version for Unity 6+ may need to be updated in the future.
                    print("Unity 6+ detected. Adding Meta OpenXR v2.2.0.")
                    manifest_data["dependencies"]["com.unity.xr.meta-openxr"] = "2.2.0"
                elif current_v > UnityVersion("2022.3.0f1"):
                    print("Unity 2022.3+ detected. Adding Meta OpenXR v1.0.4.")
                    manifest_data["dependencies"]["com.unity.xr.meta-openxr"] = "1.0.4"
                else:
                    print("Older Unity version detected. Meta OpenXR package not added.")

    try:
        with open(manifest_path, 'w') as f:
            json.dump(manifest_data, f, indent=4)
        print(f"\n✅ Successfully updated {manifest_path}")
    except IOError as e:
        print(f"Error writing to manifest.json: {e}")

if __name__ == "__main__":
    main()