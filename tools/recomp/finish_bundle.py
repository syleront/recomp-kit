#!/usr/bin/env python3
"""Finish an assembled app bundle: identity, resources, core mods, texture pack, signature."""

import argparse
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import copy_layouts  # noqa: E402
import game_config  # noqa: E402
import macos_icon  # noqa: E402

GENERAL_MIDI = ROOT / "third_party/soundfonts/generaluser-gs/GeneralUser-GS.sf2"
# Mach-O magics: 32/64-bit, both byte orders, and the universal wrappers.
MACHO_MAGICS = {b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}


def rename_identity(plist_path, version, build):
    """Stamp the release version; identity otherwise comes from generated/Info.plist.

    The version and build number are game.toml's single source of truth, so a
    git description never lands in CFBundleVersion (which must be numeric)."""
    data = plistlib.loads(plist_path.read_bytes())
    if version:
        data["CFBundleShortVersionString"] = version.lstrip("v")
    if build:
        data["CFBundleVersion"] = str(build)
    plist_path.write_bytes(plistlib.dumps(data))


def bundle_ffmpeg(contents, libraries):
    """Copy the replaceable shared libraries and set their install names.

    Signing happens once, in sign_bundle, after every nested Mach-O is final."""
    if not libraries:
        return
    frameworks = contents / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "third_party/ffmpeg/NOTICE.md", contents / "Resources/ffmpeg-NOTICE.md")
    for source in libraries:
        # Follow the installed major-version symlink into a standalone file.
        dest = frameworks / source.name
        shutil.copy2(source, dest)
        subprocess.run(["install_name_tool", "-id", "@rpath/" + dest.name, str(dest)], check=True)


def copy_documents(game_dir, resources):
    """Ship the project and kit licences/notices inside the app for redistribution."""
    documents = ((game_dir / "LICENSE", "LICENSE.txt"),
                 (game_dir / "NOTICE", "NOTICE.txt"),
                 (ROOT / "LICENSE", "recomp-kit-LICENSE.txt"),
                 (ROOT / "NOTICE", "recomp-kit-NOTICE.txt"))
    for source, name in documents:
        if source.is_file():
            shutil.copy2(source, resources / name)


def install_icon(game_dir, build_root, resources):
    """Copy the icon extracted from the user's game into Contents/Resources.

    A missing or unreadable executable is not fatal: a checkout built without
    its original/ link (a runtime-only build on another machine) still links,
    and the bundle simply has no custom icon."""
    destination = resources / "AppIcon.icns"
    try:
        exe = game_config.load(game_dir)["developer_exe_path"]
        if not exe.is_file():
            raise FileNotFoundError("game executable %s is not present" % exe)
        icns = macos_icon.ensure_icns(exe, build_root)
        shutil.copy2(icns, destination)
        print("icon: AppIcon.icns from %s" % exe.name)
    except Exception as error:  # fail soft: an icon never blocks a build
        destination.unlink(missing_ok=True)
        print("finish_bundle: warning: no app icon: %s" % error, file=sys.stderr)


def is_macho(path):
    try:
        with open(path, "rb") as handle:
            return handle.read(4) in MACHO_MAGICS
    except OSError:
        return False


def nested_machos(bundle, main_executable):
    """Every Mach-O in the bundle except the app's own executable."""
    found = []
    for path in bundle.rglob("*"):
        if path.is_symlink() or not path.is_file() or path == main_executable:
            continue
        if is_macho(path):
            found.append(path)
    return sorted(found)


def sign_bundle(bundle, app_name):
    """Ad-hoc sign nested code first, then the app, and verify the seal.

    --deep is deliberately absent: it re-signs nested code with the app's
    identity in an order Apple no longer supports and hides a missing nested
    signature. The hardened runtime is also absent: an ad-hoc signature has no
    team identity, so library validation would refuse the FFmpeg dylibs (and
    notarization is impossible without a Developer ID). No entitlement is
    needed: the app runs no JIT and is not sandboxed."""
    if os.environ.get("RECOMP_SKIP_SIGN") == "1":
        print("sign: skipped (RECOMP_SKIP_SIGN=1)")
        return
    if platform.system() != "Darwin":
        return
    executable = bundle / "Contents/MacOS" / app_name
    for path in nested_machos(bundle, executable):
        subprocess.run(["codesign", "--force", "--timestamp=none", "--sign", "-", str(path)], check=True)
    subprocess.run(["codesign", "--force", "--timestamp=none", "--sign", "-", str(bundle)], check=True)
    subprocess.run(["codesign", "--verify", "--strict", "--verbose=2", str(bundle)], check=True)
    details = subprocess.run(["codesign", "-dv", str(bundle)], capture_output=True, text=True)
    if details.returncode:
        raise subprocess.CalledProcessError(details.returncode, details.args,
                                            details.stdout, details.stderr)
    print((details.stderr or details.stdout).strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--version", default="", help="CFBundleShortVersionString (game.toml [game] version)")
    parser.add_argument("--build", default="", help="CFBundleVersion (game.toml [game] build)")
    parser.add_argument("--build-root", type=Path, default=ROOT / "build",
                        help="Where this build's outputs live (texture pack, symbols.json)")
    parser.add_argument("--game-dir", type=Path, required=True, help="The game directory (its mods/core is installed)")
    parser.add_argument("--pack", type=Path, default=None)
    parser.add_argument("--ffmpeg-library", type=Path, action="append", default=[],
                        help="FFmpeg shared library to copy into Contents/Frameworks (repeatable)")
    args = parser.parse_args()
    if args.pack is None:
        args.pack = Path(os.environ.get("RECOMP_TEXTURE_PACK_DIR") or args.build_root / "texture-pack")
    contents = args.bundle / "Contents"
    resources = contents / "Resources"
    resources.mkdir(parents=True, exist_ok=True)
    rename_identity(contents / "Info.plist", args.version, args.build)
    install_icon(args.game_dir, args.build_root, resources)
    copy_documents(args.game_dir, resources)
    # The committed probe list. A missing list is labelled a baseline fallback
    # by the settings layer; packaging never manufactures measurements.
    probes = ROOT / "tools/recomp/baseline/classic-modes.json"
    if probes.is_file():
        shutil.copy(probes, resources / "classic-modes.json")
    # The General MIDI bank for a game that ships none, with its licence.
    shutil.copy2(GENERAL_MIDI, resources / "general-midi.sf2")
    shutil.copy2(GENERAL_MIDI.parent / "LICENSE", resources / "general-midi-LICENSE.txt")
    # The game's shipped on-screen control layouts, when it has any (spec section 5).
    copy_layouts.copy_layouts(args.game_dir, resources / "controls")
    # The translation index the mod loader reads, when this build has one.
    fresh = args.build_root / "recomp/symbols.json"
    if fresh.is_file():
        shutil.copy(fresh, resources / "symbols.json")
    else:
        # A stub build has no translation and therefore no symbol table; the
        # kit never tracks one (spec section 11).
        print("finish_bundle: no symbols.json in this build; mods get no symbol table")
    core = args.game_dir / "mods/core"
    if core.is_dir():
        subprocess.run([sys.executable, str(ROOT / "tools/recomp/build_core.py"), "--source", str(core),
                        "--dest", str(resources / "mods/core"), "--cc", args.cc], check=True, cwd=ROOT)
    if (args.pack / "manifest.json").is_file():
        subprocess.run([sys.executable, str(ROOT / "tools/recomp/package_texture_pack.py"),
                        str(args.pack), str(resources / "texture-pack")], check=True, cwd=ROOT)
    bundle_ffmpeg(contents, args.ffmpeg_library)
    # Seal after every resource is in place: the linker signature alone does
    # not cover the resource envelope.
    sign_bundle(args.bundle, args.name)
    shown = args.bundle.relative_to(ROOT) if args.bundle.is_relative_to(ROOT) else args.bundle
    print("built %s" % shown)


if __name__ == "__main__":
    main()
