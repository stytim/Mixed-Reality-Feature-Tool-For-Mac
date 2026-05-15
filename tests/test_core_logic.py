"""Regression tests for the P0/P1 bugs fixed in the audit.

These tests do not touch the network. _download_file and the graphics-tools
helpers are monkeypatched to return controlled fixtures.
"""
import json
import pathlib
import sys
import tarfile

import pytest

# Make the repo root importable when pytest is invoked from anywhere.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import core_logic as core


# --- parse_package_name_from_tgz -------------------------------------------------

def test_parse_handles_graphics_tools_prerelease():
    name, ver = core.parse_package_name_from_tgz(
        "com.microsoft.mrtk.graphicstools.unity-1.0.0-pre.1.tgz"
    )
    assert name == "com.microsoft.mrtk.graphicstools.unity"
    assert ver == "1.0.0-pre.1"


def test_parse_handles_plain_mrtk_release():
    name, ver = core.parse_package_name_from_tgz("org.mixedrealitytoolkit.core-3.0.0.tgz")
    assert name == "org.mixedrealitytoolkit.core"
    assert ver == "3.0.0"


def test_parse_rejects_unrelated_filename():
    assert core.parse_package_name_from_tgz("README.md") == (None, None)
    assert core.parse_package_name_from_tgz("foo.tgz") == (None, None)


# --- UnityVersion ----------------------------------------------------------------

def test_unity_version_parser_handles_malformed_strings():
    # Should not raise on any of these inputs.
    for bad in ["", "garbage", "1.x.0f0", "1.0", None]:
        try:
            core.UnityVersion(bad)
        except Exception as e:
            pytest.fail(f"UnityVersion({bad!r}) raised {e!r}")


def test_unity_version_ordering():
    assert core.UnityVersion("6000.0.0f0") > core.UnityVersion("2022.3.0f1")
    assert core.UnityVersion("2022.3.0f1") > core.UnityVersion("2022.2.10f1")
    assert core.UnityVersion("2022.3.0f1") == core.UnityVersion("2022.3.0f1")
    assert not (core.UnityVersion("2022.3.0f1") > core.UnityVersion("2022.3.0f1"))


# --- Dependency resolution -------------------------------------------------------

def _make_fake_release(filename, url="https://example.invalid/asset"):
    return {"assets": [{"name": filename, "browser_download_url": url}]}


def _write_fake_tgz(path: pathlib.Path, package_json: dict):
    """Write a minimal .tgz containing only `package/package.json`."""
    import io
    payload = json.dumps(package_json).encode()
    with tarfile.open(path, "w:gz") as tar:
        info = tarfile.TarInfo(name="package/package.json")
        info.size = len(payload)
        tar.addfile(info, io.BytesIO(payload))


def test_dependency_resolution_terminates_on_cycle(monkeypatch, tmp_path):
    """A → B → A loop must not infinite-loop."""
    releases = [
        _make_fake_release("org.mixedrealitytoolkit.a-1.0.0.tgz", "https://example.invalid/a"),
        _make_fake_release("org.mixedrealitytoolkit.b-1.0.0.tgz", "https://example.invalid/b"),
    ]

    def fake_download(url, dest_folder, progress_callback=None, dest_name=None):
        fname = dest_name or url.split("/")[-1] + ".tgz"
        dest = pathlib.Path(dest_folder) / fname
        if url.endswith("/a"):
            _write_fake_tgz(dest, {"dependencies": {"org.mixedrealitytoolkit.b": "1.0.0"}})
        else:
            _write_fake_tgz(dest, {"dependencies": {"org.mixedrealitytoolkit.a": "1.0.0"}})
        return dest

    monkeypatch.setattr(core, "_download_file", fake_download)

    resolved = core.resolve_dependencies({"a": "1.0.0"}, releases, work_dir=tmp_path)
    assert "a" in resolved
    assert "b" in resolved


def test_graphics_tools_is_recorded_when_pulled_in_as_dep(monkeypatch, tmp_path):
    """Graphics-tools is fetched from its own repo at download time and has no
    release asset in the MRTK repo. When an MRTK package declares it as a dep,
    it must still be recorded in resolved_packages so download_and_apply_packages
    knows to repackage it. (Parity with C++ behaviour: record, don't recurse.)"""
    releases = [
        _make_fake_release("org.mixedrealitytoolkit.foo-1.0.0.tgz", "https://example.invalid/foo"),
    ]

    def fake_download(url, dest_folder, progress_callback=None, dest_name=None):
        fname = dest_name or url.split("/")[-1]
        dest = pathlib.Path(dest_folder) / fname
        # `foo` declares graphics-tools as a dependency.
        _write_fake_tgz(dest, {"dependencies": {core.GRAPHICS_TOOLS_NAME: "1.5.0"}})
        return dest

    monkeypatch.setattr(core, "_download_file", fake_download)
    resolved = core.resolve_dependencies({"foo": "1.0.0"}, releases, work_dir=tmp_path)
    assert "foo" in resolved
    assert core.GRAPHICS_TOOLS_NAME in resolved
    assert resolved[core.GRAPHICS_TOOLS_NAME] == "1.5.0"


# --- Install side effects --------------------------------------------------------

def _make_fake_unity_project(root: pathlib.Path):
    root.mkdir(parents=True, exist_ok=True)
    (root / "Assets").mkdir()
    (root / "Packages").mkdir()
    (root / "ProjectSettings").mkdir()
    (root / "Packages" / "manifest.json").write_text(json.dumps({"dependencies": {}}))
    return root


def test_install_preserves_existing_mixed_reality_folder(monkeypatch, tmp_path):
    """Issue 3: do NOT rmtree <project>/Packages/MixedReality before writing."""
    project = _make_fake_unity_project(tmp_path / "proj")
    mr = project / "Packages" / "MixedReality"
    mr.mkdir()
    preexisting = mr / "preexisting.tgz"
    preexisting.write_text("untouched")

    work_dir = tmp_path / "work"
    work_dir.mkdir()

    new_pkg = work_dir / "org.mixedrealitytoolkit.core-3.0.0.tgz"
    new_pkg.write_text("new")

    def fake_download(url, dest_folder, progress_callback=None, dest_name=None):
        return new_pkg

    monkeypatch.setattr(core, "_download_file", fake_download)

    releases = [_make_fake_release("org.mixedrealitytoolkit.core-3.0.0.tgz",
                                    "https://example.invalid/core")]
    ok = core.download_and_apply_packages(
        project, {"core": "3.0.0"}, releases, set(), work_dir=work_dir
    )
    assert ok
    assert preexisting.exists(), "Pre-existing MixedReality file was destroyed"
    assert preexisting.read_text() == "untouched"
    assert (mr / "org.mixedrealitytoolkit.core-3.0.0.tgz").exists()


def test_install_ignores_unrelated_tgz_in_cwd(monkeypatch, tmp_path):
    """Issue 2: install must NOT glob the CWD/work_dir for tgz files. Only files
    actually produced by _download_file should end up in the project."""
    project = _make_fake_unity_project(tmp_path / "proj")
    work_dir = tmp_path / "work"
    work_dir.mkdir()

    # Plant an unrelated tgz in work_dir — install must not pick it up.
    unrelated = work_dir / "unrelated-other-1.0.0.tgz"
    unrelated.write_text("not mine")

    new_pkg = work_dir / "org.mixedrealitytoolkit.core-3.0.0.tgz"
    new_pkg.write_text("new")

    def fake_download(url, dest_folder, progress_callback=None, dest_name=None):
        return new_pkg

    monkeypatch.setattr(core, "_download_file", fake_download)

    releases = [_make_fake_release("org.mixedrealitytoolkit.core-3.0.0.tgz",
                                    "https://example.invalid/core")]
    core.download_and_apply_packages(project, {"core": "3.0.0"}, releases, set(), work_dir=work_dir)

    installed = list((project / "Packages" / "MixedReality").iterdir())
    installed_names = {p.name for p in installed}
    assert "org.mixedrealitytoolkit.core-3.0.0.tgz" in installed_names
    assert "unrelated-other-1.0.0.tgz" not in installed_names

    manifest = json.loads((project / "Packages" / "manifest.json").read_text())
    assert "org.mixedrealitytoolkit.core" in manifest["dependencies"]
    # The unrelated file's name must not have leaked into the manifest either.
    assert "unrelated-other" not in manifest["dependencies"]
