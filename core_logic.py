import sys
import pathlib
import requests
import json
import re
import tarfile
import tempfile
import shutil
from packaging.version import parse as parse_semver

GITHUB_API_URL = "https://api.github.com/repos/MixedRealityToolkit/MixedRealityToolkit-Unity/releases"
GRAPHICS_TOOLS_URL_FORMAT = "https://github.com/microsoft/MixedReality-GraphicsTools-Unity/archive/refs/tags/v{}.tar.gz"

# (connect, read) timeouts for every network call. Without these, requests
# blocks forever on a dead network.
HTTP_TIMEOUT = (15, 300)

MRTK_PREFIX = "org.mixedrealitytoolkit."
GRAPHICS_TOOLS_NAME = "com.microsoft.mrtk.graphicstools.unity"

# Matches "<package>-<version>.tgz" where version is N.N.N optionally followed by -pre.N.
PACKAGE_FILENAME_RE = re.compile(r"^(.+?)-([0-9]+\.[0-9]+\.[0-9]+(?:-pre\.[0-9]+)?)\.tgz$")


def parse_package_name_from_tgz(filename: str):
    """Return (package_name, version) parsed from a .tgz filename, or (None, None)."""
    m = PACKAGE_FILENAME_RE.match(filename)
    if not m:
        return None, None
    return m.group(1), m.group(2)


class UnityVersion:
    """A custom class to parse and compare Unity-specific version strings."""
    def __init__(self, version_str="0.0.0f0"):
        self.major, self.minor, self.patch, self.type, self.build = 0, 0, 0, 'f', 0
        if not isinstance(version_str, str) or not version_str:
            return
        match = re.match(r"(\d+)\.(\d+)\.(\d+)([abfp])(\d+)", version_str)
        if match:
            try:
                self.major, self.minor, self.patch, self.type, self.build = (
                    int(match.group(1)), int(match.group(2)), int(match.group(3)),
                    match.group(4), int(match.group(5)),
                )
            except ValueError:
                pass
            return
        match = re.match(r"(\d+)\.(\d+)\.(\d+)", version_str)
        if match:
            try:
                self.major, self.minor, self.patch = (
                    int(match.group(1)), int(match.group(2)), int(match.group(3)),
                )
            except ValueError:
                pass

    def _to_tuple(self):
        return (self.major, self.minor, self.patch, self.type, self.build)
    def __gt__(self, other): return self._to_tuple() > other._to_tuple()
    def __ge__(self, other): return self._to_tuple() >= other._to_tuple()
    def __eq__(self, other): return self._to_tuple() == other._to_tuple()
    def __hash__(self): return hash(self._to_tuple())


def _emit(message: str, progress_callback):
    if progress_callback:
        progress_callback(message)
    else:
        print(message)


def _download_file(url: str, dest_folder: pathlib.Path, progress_callback=None, dest_name: str = None):
    """Download `url` into `dest_folder`. Returns the resulting Path or None on failure."""
    filename = dest_name if dest_name else url.split('/')[-1]
    dest_folder.mkdir(parents=True, exist_ok=True)
    dest_path = dest_folder / filename
    _emit(f"Downloading {filename}...", progress_callback)
    try:
        with requests.get(url, stream=True, timeout=HTTP_TIMEOUT) as r:
            r.raise_for_status()
            with open(dest_path, 'wb') as f:
                for chunk in r.iter_content(chunk_size=8192):
                    f.write(chunk)
        return dest_path
    except requests.exceptions.RequestException as e:
        _emit(f"Error downloading {url}: {e}", progress_callback)
        if dest_path.exists():
            try:
                dest_path.unlink()
            except OSError:
                pass
        return None


def _get_dependencies_from_tgz(tgz_path: pathlib.Path) -> dict:
    dependencies = {}
    try:
        with tarfile.open(tgz_path, "r:gz") as tar:
            for member in tar.getmembers():
                if pathlib.Path(member.name).name == 'package.json':
                    f = tar.extractfile(member)
                    if f:
                        content = json.load(f)
                        dependencies = content.get("dependencies", {}) or {}
                        break
    except Exception as e:
        print(f"Could not read dependencies from {tgz_path}: {e}")
    return dependencies


def _safe_extractall(tar: tarfile.TarFile, dest: pathlib.Path):
    """Extract `tar` into `dest`, refusing entries that escape it (CVE-2007-4559).

    Uses Python 3.12+ `filter='data'` when available, otherwise falls back to a
    manual prefix check.
    """
    try:
        tar.extractall(path=dest, filter='data')
    except TypeError:
        dest_resolved = dest.resolve()
        for member in tar.getmembers():
            target = (dest / member.name).resolve()
            try:
                target.relative_to(dest_resolved)
            except ValueError:
                print(f"Skipping unsafe archive entry: {member.name}")
                continue
            tar.extract(member, path=dest)


def _extract_and_repackage_graphics_tools(tgz_path: pathlib.Path, version: str, work_dir: pathlib.Path, progress_callback=None):
    """Extract the GitHub source tarball, isolate the Unity package subdir, and re-tar as .tgz.

    Returns the produced .tgz Path or None on failure. Cleans up its own temp dirs.
    """
    _emit(f"Repackaging graphics tools v{version}...", progress_callback)

    temp_extract_dir = pathlib.Path(tempfile.mkdtemp(prefix=f"mrtk-gt-{version}-", dir=str(work_dir)))
    package_dir = pathlib.Path(tempfile.mkdtemp(prefix=f"mrtk-gt-pkg-{version}-", dir=str(work_dir)))
    # `mkdtemp` creates the dir; the move/copy below expects it not to exist yet.
    shutil.rmtree(package_dir)
    output_tgz = work_dir / f"{GRAPHICS_TOOLS_NAME}-{version}.tgz"

    try:
        with tarfile.open(tgz_path, "r:gz") as tar:
            _safe_extractall(tar, temp_extract_dir)
        source_package_path = temp_extract_dir / f"MixedReality-GraphicsTools-Unity-{version}" / GRAPHICS_TOOLS_NAME
        if not source_package_path.exists():
            _emit(f"Error: Could not find graphics tools package inside {source_package_path}", progress_callback)
            return None
        shutil.move(str(source_package_path), str(package_dir))
        with tarfile.open(output_tgz, "w:gz") as tar:
            tar.add(package_dir, arcname="package")
        return output_tgz
    except Exception as e:
        _emit(f"Failed to repackage graphics tools: {e}", progress_callback)
        return None
    finally:
        if temp_extract_dir.exists():
            shutil.rmtree(temp_extract_dir, ignore_errors=True)
        if package_dir.exists():
            shutil.rmtree(package_dir, ignore_errors=True)
        if tgz_path.exists():
            try:
                tgz_path.unlink()
            except OSError:
                pass


def _find_download_url(component_name: str, version: str, mrtk_releases_json: list) -> str:
    """Find the asset URL matching <component_name>-<version>.tgz.

    component_name may be either the short suffix (e.g. "core") or the full
    identifier (e.g. "org.mixedrealitytoolkit.core"); both work.
    """
    for release in mrtk_releases_json or []:
        for asset in release.get("assets", []):
            name = asset.get("name", "")
            pkg_name, pkg_version = parse_package_name_from_tgz(name)
            if pkg_name is None or pkg_version != version:
                continue
            if pkg_name == component_name:
                return asset.get("browser_download_url", "")
            if pkg_name.startswith(MRTK_PREFIX) and pkg_name[len(MRTK_PREFIX):] == component_name:
                return asset.get("browser_download_url", "")
    return ""


def _resolve_dependencies_recursively(component_name: str, version: str, mrtk_releases_json: list,
                                       resolved_packages: dict, work_dir: pathlib.Path,
                                       progress_callback=None):
    if component_name in resolved_packages and parse_semver(resolved_packages[component_name]) >= parse_semver(version):
        return
    _emit(f"  Resolving {component_name} v{version}...", progress_callback)

    resolved_packages[component_name] = version

    # Graphics tools has no release asset in the MRTK repo; it is downloaded
    # and repackaged later in download_and_apply_packages. Match the C++ impl
    # by recording it here and returning without traversing further.
    if component_name == GRAPHICS_TOOLS_NAME:
        return

    download_url = _find_download_url(component_name, version, mrtk_releases_json)
    if not download_url:
        return
    downloaded_path = _download_file(download_url, work_dir, progress_callback,
                                     dest_name=f"dep-check-{component_name}-{version}.tgz")
    if not downloaded_path:
        return
    try:
        dependencies = _get_dependencies_from_tgz(downloaded_path)
    finally:
        if downloaded_path.exists():
            try:
                downloaded_path.unlink()
            except OSError:
                pass

    for dep_name, dep_version in dependencies.items():
        if dep_name.startswith("com.unity."):
            continue
        if dep_name.startswith(MRTK_PREFIX):
            dep_component_name = dep_name[len(MRTK_PREFIX):]
            _resolve_dependencies_recursively(dep_component_name, dep_version, mrtk_releases_json,
                                              resolved_packages, work_dir, progress_callback)
        else:
            _resolve_dependencies_recursively(dep_name, dep_version, mrtk_releases_json,
                                              resolved_packages, work_dir, progress_callback)


def get_unity_version(project_path: pathlib.Path) -> str:
    version_file = project_path / "ProjectSettings" / "ProjectVersion.txt"
    if not version_file.exists():
        return ""
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
        response = requests.get(GITHUB_API_URL, timeout=HTTP_TIMEOUT)
        response.raise_for_status()
        mrtk_releases_json = response.json()
        for release in mrtk_releases_json:
            for asset in release.get("assets", []):
                name, version = parse_package_name_from_tgz(asset.get("name", ""))
                if name and name.startswith(MRTK_PREFIX):
                    suffix = name[len(MRTK_PREFIX):]
                    mrtk_components.setdefault(suffix, []).append(version)
        for name in mrtk_components:
            mrtk_components[name].sort(key=parse_semver, reverse=True)
        return mrtk_components, mrtk_releases_json
    except requests.exceptions.RequestException as e:
        print(f"Failed to get data from GitHub API: {e}")
        return {}, None


def resolve_dependencies(selections: dict, mrtk_releases_json: list, progress_callback=None,
                         work_dir: pathlib.Path = None):
    _emit("--- Resolving all dependencies ---", progress_callback)
    owns_work_dir = work_dir is None
    if owns_work_dir:
        work_dir = pathlib.Path(tempfile.mkdtemp(prefix="mrtk-resolve-"))
    else:
        work_dir.mkdir(parents=True, exist_ok=True)
    try:
        resolved_packages = {}
        for component_name, version in selections.items():
            _resolve_dependencies_recursively(component_name, version, mrtk_releases_json,
                                              resolved_packages, work_dir, progress_callback)
        _emit("--- Resolution complete ---", progress_callback)
        return resolved_packages
    finally:
        if owns_work_dir:
            shutil.rmtree(work_dir, ignore_errors=True)


def download_and_apply_packages(project_path: pathlib.Path, resolved_packages: dict,
                                mrtk_releases_json: list, selected_openxr: set,
                                progress_callback=None, work_dir: pathlib.Path = None):
    _emit("--- Downloading final packages ---", progress_callback)
    owns_work_dir = work_dir is None
    if owns_work_dir:
        work_dir = pathlib.Path(tempfile.mkdtemp(prefix="mrtk-install-"))
    else:
        work_dir.mkdir(parents=True, exist_ok=True)

    downloaded_files = []
    try:
        for component_name, version in resolved_packages.items():
            if component_name == GRAPHICS_TOOLS_NAME:
                graphics_tgz_path = _download_file(GRAPHICS_TOOLS_URL_FORMAT.format(version), work_dir,
                                                   progress_callback,
                                                   dest_name=f"{GRAPHICS_TOOLS_NAME}-source-{version}.tar.gz")
                if graphics_tgz_path:
                    repacked = _extract_and_repackage_graphics_tools(graphics_tgz_path, version, work_dir,
                                                                     progress_callback)
                    if repacked:
                        downloaded_files.append(repacked)
            else:
                download_url = _find_download_url(component_name, version, mrtk_releases_json)
                if download_url:
                    path = _download_file(download_url, work_dir, progress_callback)
                    if path:
                        downloaded_files.append(path)
                else:
                    _emit(f"ERROR: Could not find final download URL for {component_name} v{version}", progress_callback)

        _emit("--- Applying changes to project ---", progress_callback)

        mixed_reality_dir = project_path / "Packages" / "MixedReality"
        # Merge into existing folder rather than destroying user content.
        mixed_reality_dir.mkdir(parents=True, exist_ok=True)
        installed_files = []
        for tgz_file in downloaded_files:
            dest = mixed_reality_dir / tgz_file.name
            try:
                # `replace` is atomic on POSIX and overwrites cleanly.
                shutil.move(str(tgz_file), str(dest))
            except shutil.SameFileError:
                pass
            installed_files.append(dest)

        manifest_path = project_path / "Packages" / "manifest.json"
        try:
            with open(manifest_path, 'r') as f:
                manifest_data = json.load(f)
            if "dependencies" not in manifest_data or not isinstance(manifest_data["dependencies"], dict):
                manifest_data["dependencies"] = {}

            for tgz_file in installed_files:
                component_name, _ = parse_package_name_from_tgz(tgz_file.name)
                if component_name:
                    manifest_data["dependencies"][component_name] = f"file:MixedReality/{tgz_file.name}"
                else:
                    _emit(f"Skipping manifest entry (could not parse name): {tgz_file.name}", progress_callback)

            if "com.microsoft.mixedreality.openxr" in selected_openxr:
                manifest_data["dependencies"]["com.microsoft.mixedreality.openxr"] = "1.11.2"
            if "com.unity.xr.meta-openxr" in selected_openxr:
                unity_version_str = get_unity_version(project_path)
                if unity_version_str:
                    current_v = UnityVersion(unity_version_str)
                    if current_v >= UnityVersion("6000.0.0f0"):
                        manifest_data["dependencies"]["com.unity.xr.meta-openxr"] = "2.2.0"
                    elif current_v > UnityVersion("2022.3.0f1"):
                        manifest_data["dependencies"]["com.unity.xr.meta-openxr"] = "1.0.4"

            with open(manifest_path, 'w') as f:
                json.dump(manifest_data, f, indent=4)
            _emit("Successfully updated manifest.json!", progress_callback)
            return True
        except Exception as e:
            _emit(f"Error applying changes: {e}", progress_callback)
            return False
    finally:
        if owns_work_dir:
            shutil.rmtree(work_dir, ignore_errors=True)


# --- Main function for standalone CLI execution ---
def main_cli():
    if len(sys.argv) < 2:
        print("Usage: python core_logic.py <path_to_unity_project>")
        sys.exit(1)
    project_path = pathlib.Path(sys.argv[1])
    if not (project_path.is_dir()
            and (project_path / "Assets").exists()
            and (project_path / "Packages").exists()
            and (project_path / "ProjectSettings").exists()):
        print(f"Error: '{project_path}' is not a valid Unity project directory.")
        sys.exit(1)
    unity_version_str = get_unity_version(project_path)
    if unity_version_str: print(f"Detected Unity Version: {unity_version_str}")
    else: print("Warning: Could not detect Unity version.")
    mrtk_components, mrtk_releases_json = discover_mrtk_components()
    if not mrtk_components:
        sys.exit(1)

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
    for sel_idx in selected_indices:
        if 0 <= sel_idx < len(all_packages):
            pkg = all_packages[sel_idx]
            if pkg.type == 'mrtk':
                latest_version = mrtk_components[pkg.identifier][0]
                mrtk_selections[pkg.identifier] = latest_version
            elif pkg.type == 'openxr':
                openxr_selections.add(pkg.identifier)
        else:
            print(f"Invalid index: {sel_idx}. Skipping.")

    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="mrtk-cli-"))
    try:
        if mrtk_selections:
            resolved = resolve_dependencies(mrtk_selections, mrtk_releases_json, work_dir=work_dir)
            download_and_apply_packages(project_path, resolved, mrtk_releases_json,
                                        openxr_selections, work_dir=work_dir)
        elif openxr_selections:
            download_and_apply_packages(project_path, {}, [], openxr_selections, work_dir=work_dir)
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    main_cli()
