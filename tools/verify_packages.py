"""Validate release archives against their sources and private runtime evidence."""
from __future__ import annotations

import argparse
import configparser
import datetime as dt
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys

import pefile

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT.parent / "Dynamic Texture LOD Core"
SKYRIM = ROOT.parent / "Dynamic Texture LOD Skyrim"
LAB = ROOT / ".runtime-tests/20261003-texture-lod"
sys.path.insert(0, str(ROOT / "tools/benchmark"))
from lab import binary_version


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ini_values(text: str) -> dict:
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(text)
    return {section: dict(parser[section]) for section in parser.sections()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repack", action="store_true",
                        help="Rebuild archives from existing DLLs; never compile or deploy")
    parser.add_argument("--fo4-cases", nargs="+", help="Successful cases containing this exact Fallout 4 DLL")
    parser.add_argument("--skyrim-cases", nargs="+", help="Successful cases containing this exact Skyrim DLL")
    args = parser.parse_args()
    fo4_cache = (ROOT / "build/CMakeCache.txt").read_text(encoding="utf-8")
    skyrim_cache = (SKYRIM / "build/release/CMakeCache.txt").read_text(encoding="utf-8")
    assert "DEPLOY_PLUGIN:BOOL=OFF\n" in fo4_cache
    assert "DEPLOY_DIR:PATH=\n" in skyrim_cache

    default_source = (CORE / "src/core/Settings.cpp").read_text(encoding="utf-8")
    default_ini = default_source.split('R"(', 1)[1].split(')";', 1)[0]
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    evidence = LAB / "evidence" / ("packages-" + stamp)
    evidence.mkdir(parents=True, exist_ok=False)
    results = []
    for game, root, directory, extender, labels in [
        ("fo4", ROOT, ROOT / "build/src", "F4SE",
         args.fo4_cases),
        ("skyrim", SKYRIM, SKYRIM / "build/release", "SKSE",
         args.skyrim_cases),
    ]:
        dll = directory / "DynamicTextureLOD.dll"
        ini = root / "package/DynamicTextureLOD.ini"
        version = binary_version(dll)
        archive = directory / ("DynamicTextureLOD-" + '.'.join(version.split('.')[:3]) + ".7z")
        if not labels:
            if version != "1.0.0.0":
                raise ValueError("Supply --" + game + "-cases for a new product version; previous cases do not validate it")
            labels = (["fo4-default-v2b1", "fo4-default-v2b2", "fo4-ae-default-v3b1"] if game == "fo4" else
                      ["skyrim-default-v2b1", "skyrim-default-v2b2"])
        source_ini = ini.read_text(encoding="utf-8")
        assert ini_values(source_ini) == ini_values(default_ini), "Embedded default values differ"
        tested = []
        for label in labels:
            run = LAB / "runs" / label
            case = json.loads((run / "case.json").read_text(encoding="utf-8"))
            result = json.loads((run / "result.json").read_text(encoding="utf-8"))
            analysis = json.loads((run / "analysis.json").read_text(encoding="utf-8"))
            assert result["success"], label
            assert analysis["treatment_validation"]["valid"], label
            assert case["plugin_sha256"].lower() == digest(dll), "DLL differs from tested artifact"
            tested.append({"case": label, "dll_sha256": case["plugin_sha256"],
                           "runtime": case["expected_runtime_version"],
                           "measurement_windows": len(analysis.get("measurements", [])),
                           "valid_frame_windows": sum(
                               bool(window.get("presentmon", {}).get("valid_for_frame_time_comparison"))
                               for window in analysis.get("measurements", []))})

        if args.repack:
            stage = evidence / game / "stage"
            plugins = stage / extender / "Plugins"
            plugins.mkdir(parents=True, exist_ok=False)
            shutil.copy2(dll, plugins / dll.name)
            shutil.copy2(ini, plugins / ini.name)
            subprocess.run(["cmake", "-E", "tar", "cf", str(archive),
                            "--format=7zip", "--", "."], cwd=stage, check=True)

        listing = subprocess.check_output(["cmake", "-E", "tar", "tf", str(archive)], text=True)
        files = []
        for line in listing.splitlines():
            relative = PurePosixPath(line.replace("\\", "/"))
            assert not relative.is_absolute() and ".." not in relative.parts and ":" not in line
            if not line.endswith(("/", "\\")):
                files.append(str(relative))
        assert sorted(files) == sorted([f"{extender}/Plugins/{dll.name}",
                                        f"{extender}/Plugins/{ini.name}"]), files
        unpack = evidence / game / "unpacked"
        unpack.mkdir(parents=True, exist_ok=False)
        subprocess.run(["cmake", "-E", "tar", "xf", str(archive)], cwd=unpack, check=True)
        plugin = unpack / extender / "Plugins"
        assert digest(plugin / dll.name) == digest(dll)
        assert (plugin / ini.name).read_text(encoding="utf-8") == source_ini
        with pefile.PE(str(dll)) as binary:
            assert binary.FILE_HEADER.Machine == 0x8664
            exports = sorted(e.name.decode() for e in binary.DIRECTORY_ENTRY_EXPORT.symbols if e.name)
            assert all(extender + "Plugin_" + name in exports for name in ["Load", "Query", "Version"])
        results.append({
            "game": game, "version": version, "archive": str(archive), "dll": str(dll),
            "archive_sha256": digest(archive), "dll_sha256": digest(dll),
            "dll_bytes": dll.stat().st_size, "exports": exports,
            "archive_contents": listing.splitlines(), "packaged_ini_matches_source": True,
            "packaged_default_values_match_embedded": True,
            "embedded_default_text_matches_packaged": source_ini == default_ini,
            "runtime_tested_artifact": tested,
        })
        print(f"{game}: x64 exports, exact tested DLL, archive contents and default values verified.")

    source_hashes = {}
    for group, root in [("fo4", ROOT), ("skyrim", SKYRIM), ("core", CORE)]:
        for path in sorted((root / "src").rglob("*")):
            if path.is_file() and path.suffix in {".cpp", ".h", ".hpp"}:
                source_hashes[f"{group}/{path.relative_to(root).as_posix()}"] = digest(path)
    report = {
        "validated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "deployment_enabled": False, "compiled_during_validation": False,
        "evidence_directory": str(evidence), "packages": results,
        "source_hashes_at_validation": source_hashes,
    }
    output = ROOT / "docs/research/package-validation.json"
    if output.exists():
        shutil.copy2(output, evidence / "previous-package-validation.json")
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Evidence: {output}")


if __name__ == "__main__":
    main()
