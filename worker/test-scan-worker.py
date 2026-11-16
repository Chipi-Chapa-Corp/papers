#!/usr/bin/env python3
"""Exercise acquisition using only SANE's virtual backend, never real devices."""
import os
from pathlib import Path
import selectors
import signal
import struct
import subprocess
import sys
import tempfile
from urllib.parse import unquote

worker = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="dokument-scanner-test-") as temporary:
    root = Path(temporary)
    config = root / "config"
    config.mkdir()
    (config / "dll.conf").write_text("test\n")
    (config / "test.conf").write_text('''number_of_devices 1
test-picture "Color pattern"
geometry_min 0.0
geometry_max 40.0
geometry_quant 1.0
br_x 40.0
br_y 40.0
''')
    env = dict(os.environ, SANE_CONFIG_DIR=str(config))
    devices = subprocess.run([worker, "--list"], env=env, capture_output=True, text=True, timeout=15)
    if "DEVICE\ttest%3A0\t" not in devices.stdout:
        print("SANE test backend is unavailable; skipping virtual scanner test")
        sys.exit(77)
    output = root / "single"
    output.mkdir()
    result = subprocess.run([worker, "--device", "test:0", "--directory", str(output),
                             "--kind", "image", "--dpi", "150", "--paper", "auto"],
                            env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    records = [line.split("\t") for line in result.stdout.splitlines()]
    record = next(r for r in records if r[0] == "PAGE")
    image = Path(unquote(record[1]))
    data = image.read_bytes()
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    assert struct.unpack(">II", data[16:24]) == (236, 236)
    assert int(record[2]) == 150
    assert any(r[0] == "PREVIEW" for r in records)
    assert records[-1] == ["DONE"]
    progress = [float(record[1]) for record in records if record[0] == "PROGRESS"]
    assert progress[0] == 0.0 and progress[-1] == 1.0, progress
    assert all(0.0 <= value <= 1.0 for value in progress), progress
    # Device indices can change between discovery and acquisition processes.
    # The shim exposes only e7; opening the old e1 ID directly returns INVAL.
    reindexed = root / "reindexed"
    reindexed.mkdir()
    result = subprocess.run([worker, "--device", "airscan:e1:Virtual scanner",
                             "--directory", str(reindexed), "--dpi", "150"],
                            env=dict(env, LD_PRELOAD=str(Path(sys.argv[2]).resolve())),
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PAGE\t" in result.stdout
    assert "SETTINGS\t200" in result.stdout, result.stdout
    record = next(line.split("\t") for line in result.stdout.splitlines() if line.startswith("PAGE\t"))
    assert record[2] == "200", result.stdout
    # Stop between flatbed pages. A completed page must remain usable.
    batch = root / "multiple"
    batch.mkdir()
    process = subprocess.Popen([worker, "--device", "test:0", "--directory", str(batch),
                                "--mode", "2", "--delay", "3", "--dpi", "50"],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        saw_page = False
        while selector.select(timeout=15):
            line = process.stdout.readline()
            if not line:
                break
            if line.startswith("PAGE\t"):
                saw_page = True
                process.send_signal(signal.SIGINT)
                break
        assert saw_page, "The flatbed session did not produce a page"
        process.communicate(timeout=5)
        assert process.returncode == 0
        assert (batch / "page-000001.png").exists()
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    # Backend errors must be reported rather than looking like successful scans.
    invalid = subprocess.run([worker, "--device", "test:does-not-exist", "--directory", str(output)],
                             env=env, capture_output=True, text=True, timeout=10)
    assert invalid.returncode != 0 and "ERROR\t" in invalid.stdout
    error = unquote(invalid.stdout)
    assert "open the selected scanner" in error and "test:does-not-exist" in error
    assert "dpi" in error and "SANE" in error
    print("Virtual scanner: discovery, preview, image acquisition, physical resolution, stop, and errors passed")
