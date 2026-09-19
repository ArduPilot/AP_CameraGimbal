#!/usr/bin/env python3
"""Generate the exact allowlist shared by serving and firmware installation."""
from pathlib import Path
import json
import sys

root, output = map(Path, sys.argv[1:])
names = sorted(path.name for path in root.iterdir() if path.is_file())
if not names or any(not name.replace("-", "").replace("_", "").replace(".", "").isalnum()
                    or Path(name).suffix not in {".html", ".css", ".js"} for name in names):
    raise SystemExit("invalid webroot asset name")
text = "#pragma once\nstatic const char *const apcam_web_assets[] = {\n" + \
       "".join("    " + json.dumps(name) + ",\n" for name in names) + "};\n"
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)
