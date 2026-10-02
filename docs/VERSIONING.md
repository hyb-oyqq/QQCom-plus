# Version Management

`version/version.json` is the source of truth for the product version. The generated `version/version.h` contains the C++ macros used by the application title and Qt application metadata.

When qmake configures `easyDebug.pro`, it runs `scripts/update_version.ps1 -Mode build`. The script keeps the major, minor, and patch values, writes the current local build timestamp as `EASYDEBUG_BUILD_TIMESTAMP`, and regenerates the header before compilation. Re-running qmake therefore produces a new build identifier without requiring a manual version edit.

To release a version increment, run one of these commands from the project root, then run qmake again:

```powershell
pwsh -File scripts/update_version.ps1 -Mode patch
pwsh -File scripts/update_version.ps1 -Mode minor
pwsh -File scripts/update_version.ps1 -Mode major
```

The title bar uses `EASYDEBUG_APP_TITLE`, while `QApplication::applicationVersion()` uses `EASYDEBUG_VERSION_STR`. The version string is also available to future About and diagnostics pages through the same header. The reference `debugTool/version/qtver.exe` is not copied into this repository; the script is deterministic, reviewable, and works in the existing qmake/MSVC build environment.
