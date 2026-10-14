#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Host regression tests for drmadm selection; never touches the running OS.

Run the actual shell script in an alternate root with fake pkg/update_drv/
bootadm commands. Only its fixed PATH is replaced in the temporary copy.
Real DDI attachment and boot-archive integration need a disposable illumos BE.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / "tools/drmadm/drmadm").read_text()
mock = r'''import os
from pathlib import Path
import shlex
import sys
name = Path(sys.argv[0]).name
a = sys.argv[1:]
root = Path(os.environ["TEST_ROOT"])
if name == "pkg":
    if a[:1] == ["-R"]:
        assert Path(a[1]) == root
        a = a[2:]
    if a[0] == "contents":
        p = root / "legacy-manifest"
        if p.exists():
            print(p.read_text())
        sys.exit(0)
    if a[-1] == "driver/graphics/virtio-gpu":
        sys.exit(1)
    sys.exit(0)
if name == "bootadm":
    assert a == ["update-archive", "-R", str(root)]
    sys.exit(0)
assert name == "update_drv"
assert a[:3] == ["-n", "-b", str(root)]
a = a[3:]
operation, flag, values, driver = a
assert flag == "-i" and operation in ("-a", "-d")
assert driver in ("i915", "i915n")
if operation == "-a" and driver == "i915n" and (root / "fail-add").exists():
    sys.exit(1)
p = root / "etc/driver_aliases"
rows = [shlex.split(x) for x in p.read_text().splitlines() if x]
if operation == "-d":
    # Native update_drv removal takes one unquoted alias, not a list.
    assert '"' not in values and " " not in values
    rows = [r for r in rows if r != [driver, values]]
else:
    for value in shlex.split(values):
        assert not any(r[1] == value and r[0] != driver for r in rows)
        if [driver, value] not in rows:
            rows.append([driver, value])
p.write_text("".join(f'{d} "{v}"\n' for d, v in rows))
'''

with tempfile.TemporaryDirectory(prefix="drmadm-alias-test-") as tmp:
    base = Path(tmp)
    bindir = base / "bin"
    bindir.mkdir()
    for name in ("pkg", "update_drv", "bootadm"):
        f = bindir / name
        f.write_text(f"#!{sys.executable}\n" + mock)
        f.chmod(0o755)
    script = base / "drmadm"
    fixed = "PATH=/usr/bin:/usr/sbin:/sbin"
    assert source.count(fixed) == 1
    script.write_text(source.replace(fixed, f"PATH={shlex.quote(str(bindir))}:/usr/bin:/bin:/usr/sbin:/sbin"))

    def fixture(name):
        root = base / name
        (root / "etc/system.d").mkdir(parents=True)
        (root / "usr/share/drmadm").mkdir(parents=True)
        (root / "etc/name_to_major").write_text("i915 200\ni915n 201\n")
        (root / "etc/driver_aliases").write_text('i915 "pci8086,166"\n')
        (root / "legacy-manifest").write_text("driver name=i915 alias=pci8086,166\n")
        (root / "usr/share/drmadm/i915n.aliases").write_text("pci8086,166\npci8086,1916\npci8086,5916\n")
        return root

    def run(root, selection, ok=True):
        result = subprocess.run(["/bin/sh", str(script), "-R", str(root), "select", selection],
                                env=dict(os.environ, TEST_ROOT=str(root)),
                                text=True, capture_output=True)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        assert not (root / "etc/system.d/.drmadm.lock").exists()
        return result.stdout

    def bindings(root):
        return {v: d for d, v in (shlex.split(x) for x in
                (root / "etc/driver_aliases").read_text().splitlines())}

    root = fixture("round-trip")
    run(root, "new")
    assert bindings(root) == {"pci8086,166": "i915n", "pci8086,1916": "i915n", "pci8086,5916": "i915n"}
    assert "Nothing changed" in run(root, "new")
    run(root, "legacy")
    assert bindings(root) == {"pci8086,166": "i915"}
    print("PASS new-only Skylake/Kaby Lake binding, idempotence and legacy round-trip")

    root = fixture("conflict")
    with (root / "etc/driver_aliases").open("a") as f:
        f.write('other "pci8086,1916"\n')
    assert "Left alone" in run(root, "new")
    assert bindings(root)["pci8086,1916"] == "other"
    print("PASS another driver's binding is preserved")

    root = fixture("rollback")
    before = (root / "etc/driver_aliases").read_bytes()
    (root / "fail-add").touch()
    run(root, "new", ok=False)
    assert (root / "etc/driver_aliases").read_bytes() == before
    assert not (root / "etc/system.d/driver:graphics:drmadm").exists()
    print("PASS failed add rolls back aliases and selection")

    root = fixture("removed-packages")
    run(root, "new")
    (root / "usr/share/drmadm/i915n.aliases").unlink()
    (root / "legacy-manifest").unlink()
    run(root, "legacy")
    assert "pci8086,1916" not in bindings(root)
    assert "pci8086,5916" not in bindings(root)
    print("PASS recorded new IDs never leak to legacy without package data")

    root = fixture("older-package")
    (root / "usr/share/drmadm/i915n.aliases").unlink()
    run(root, "new")
    assert bindings(root) == {"pci8086,166": "i915n"}
    print("PASS older i915n packages without alias data remain usable")

    root = fixture("invalid-data")
    before = (root / "etc/driver_aliases").read_bytes()
    (root / "usr/share/drmadm/i915n.aliases").write_text("pci8086,1916\nbad data\n")
    run(root, "new", ok=False)
    assert (root / "etc/driver_aliases").read_bytes() == before
    assert not (root / "etc/system.d/driver:graphics:drmadm").exists()
    print("PASS invalid alias data fails before mutation")
