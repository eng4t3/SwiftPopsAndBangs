#pragma once
// Firmware version. Bump BOTH lines on every release: the OTA updater compares
// FW_VERSION_CODE against "code" in version.json on GitHub to decide whether an update is
// available. scripts/post_build.py parses these two lines (keep the format) and writes
// version.json next to firmware.bin in the repository root.
#define FW_VERSION       "2.0.0"
#define FW_VERSION_CODE  20000   // major*10000 + minor*100 + patch

// Default online-update source (changeable at runtime via POST /api/ota/config).
// Files: https://raw.githubusercontent.com/<repo>/<branch>/version.json and .../firmware.bin
#define FW_REPO_DEFAULT    "eng4t3/SwiftPopsAndBangs"
#define FW_BRANCH_DEFAULT  "main"
