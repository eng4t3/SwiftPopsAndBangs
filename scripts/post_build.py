# PlatformIO post-build hook (platformio.ini: extra_scripts = post:scripts/post_build.py)
#
# After every successful build of firmware.bin it
#   1. copies .pio/build/<env>/firmware.bin to the repository root (firmware.bin),
#   2. writes version.json next to it for the online updater:
#        {"version": "2.0.0", "code": 20000, "size": 880000, "md5": "...", "built": "...Z"}
#      version / code come from swift_show_tuning/version.h.
# It also provides $OTA_MD5 for the wireless upload command (env:esp32dev_wifi) and makes
# ota_update.cpp recompile whenever another project object changes, so the "built" stamp
# (__DATE__ __TIME__) reported by /api/info belongs to the whole image.
#
# Nothing in here may break the build: every failure is reported as a warning.

Import("env")  # noqa: F821  (provided by PlatformIO / SCons)

import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess

TAG = "[post_build]"
APP_SLOT_BYTES = 1310720  # default.csv app0/app1 size


def _warn(msg):
    print("%s WARNING: %s" % (TAG, msg))


def _md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _read_version(src_dir):
    path = os.path.join(src_dir, "version.h")
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    m_ver = re.search(r'^\s*#define\s+FW_VERSION\s+"([^"]+)"', text, re.M)
    m_code = re.search(r"^\s*#define\s+FW_VERSION_CODE\s+(\d+)", text, re.M)
    if not m_ver or not m_code:
        raise ValueError("FW_VERSION / FW_VERSION_CODE not found in %s" % path)
    version, code = m_ver.group(1), int(m_code.group(1))
    parts = re.match(r"^(\d+)\.(\d+)\.(\d+)", version)
    if parts:
        expected = int(parts.group(1)) * 10000 + int(parts.group(2)) * 100 + int(parts.group(3))
        if expected != code:
            _warn("version.h: FW_VERSION %s does not match FW_VERSION_CODE %d (expected %d)"
                  % (version, code, expected))
    return version, code


def _committed_manifest(project_dir):
    """version.json as committed in git HEAD, or None."""
    try:
        out = subprocess.run(["git", "show", "HEAD:version.json"], cwd=project_dir,
                             capture_output=True, timeout=10)
        if out.returncode == 0:
            return json.loads(out.stdout.decode("utf-8"))
    except Exception:
        pass
    return None


def publish_release_files(source, target, env):
    try:
        bin_path = target[0].get_abspath()
        project_dir = env.subst("$PROJECT_DIR")
        version, code = _read_version(env.subst("$PROJECT_SRC_DIR"))

        size = os.path.getsize(bin_path)
        md5 = _md5_of(bin_path)
        if size > APP_SLOT_BYTES:
            _warn("firmware.bin is %d bytes, larger than the %d-byte OTA slot!" % (size, APP_SLOT_BYTES))
        elif size > APP_SLOT_BYTES * 0.92:
            _warn("firmware.bin uses %.1f%% of the OTA slot" % (100.0 * size / APP_SLOT_BYTES))

        out_bin = os.path.join(project_dir, "firmware.bin")
        out_json = os.path.join(project_dir, "version.json")

        # Catch the classic mistake: new code published under the version code that is already
        # committed (= what devices see after a push), which they would never offer as an update.
        prev = _committed_manifest(project_dir)
        if prev:
            try:
                prev_code = int(prev.get("code", -1))
                if prev_code == code and prev.get("md5") != md5:
                    _warn("FW_VERSION_CODE is still %d (same as the committed version.json) but the "
                          "firmware changed: bump version.h before publishing, otherwise devices "
                          "will not see this as an update" % code)
                elif prev_code > code:
                    _warn("committed version.json has code %d, this build %d (lower): devices "
                          "will not update to it" % (prev_code, code))
            except (ValueError, TypeError):
                pass

        shutil.copyfile(bin_path, out_bin)
        manifest = {
            "version": version,
            "code": code,
            "size": size,
            "md5": md5,
            "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        with open(out_json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")

        print("%s firmware.bin -> %s (%d bytes, %.1f%% of the OTA slot)"
              % (TAG, out_bin, size, 100.0 * size / APP_SLOT_BYTES))
        print("%s version.json -> version %s, code %d, md5 %s" % (TAG, version, code, md5))
        print("%s To publish: commit firmware.bin + version.json and push." % TAG)
    except Exception as exc:  # never break the build
        _warn("could not write firmware.bin / version.json: %s" % exc)


def _ota_md5(target, source, env, for_signature):
    """$OTA_MD5 for upload_command: MD5 of the image being uploaded ('' if unknown)."""
    if for_signature:
        return ""
    try:
        return _md5_of(source[0].get_abspath())
    except Exception:
        return ""


def _refresh_build_stamp():
    """Recompile ota_update.cpp whenever any other project object is rebuilt."""
    objs = [o for o in env.Flatten(env.get("PIOBUILDFILES", [])) if hasattr(o, "get_abspath")]
    ota = [o for o in objs if os.path.basename(o.get_abspath()) == "ota_update.cpp.o"]
    others = [o for o in objs if o not in ota]
    if ota and others:
        env.Depends(ota, others)


try:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin",
                      env.VerboseAction(publish_release_files, "Writing firmware.bin + version.json"))
    env["OTA_MD5"] = _ota_md5
    _refresh_build_stamp()
except Exception as exc:
    _warn("post-build hook not installed: %s" % exc)
