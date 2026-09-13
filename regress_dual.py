#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dual-channel regression test (Windows version)
Run VM interpreter and compile channel for each tests/*.lm,
compare stdout consistency.
Usage: python regress_dual.py
"""

import os
import sys
import subprocess
import tempfile
import shutil
import glob
import re

# Project root (script directory)
PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
os.chdir(PROJECT_ROOT)

# Windows toolchain paths
MINGW_PATH = r"C:\mingw64\bin"
GIT_USR_BIN = r"D:\apps\git\Git\usr\bin"
os.environ["PATH"] = GIT_USR_BIN + os.pathsep + MINGW_PATH + os.pathsep + os.environ.get("PATH", "")

BIN_BIN = os.path.join(PROJECT_ROOT, "bin", "lumyr.exe")
TMP_DIR = os.path.join(tempfile.gettempdir(), "lumyr_regress")

# Tests to skip (not suitable for dual-channel comparison)
SKIP_LIST = {
    "requests_test.lm", "crypto_enc_test.lm", "thread_stress.lm",
    "thread_test.lm", "thread_container_test.lm", "gc_thread_stress.lm",
    "json_type_thread_test.lm", "cond_timeout_test.lm", "lock_test.lm",
    "lock_test2.lm", "threadlocal_test.lm", "gc_return_race.lm",
    "gc_promotion_test.lm", "gc_efficiency_diag.lm", "mem_leak_test.lm",
    "closure_test.lm",  # CC模式暂不支持闭包捕获
    "class_constructor_test.lm", "class_multi_param_test.lm", "class_super_ctor_test.lm", "class_comprehensive_test.lm", "class_interface_test.lm", "class_interface_full_test.lm", "class_interface_missing_test.lm",  # CC模式暂不支持
}


def run_test(fullpath, base, name, tmp_dir):
    """Run a single test in both VM and compile mode, compare output."""
    vmo = os.path.join(tmp_dir, f"{name}_vm.out")
    cmo = os.path.join(tmp_dir, f"{name}_cc.out")
    cbin = os.path.join(tmp_dir, f"{name}_bin_{os.getpid()}_{os.urandom(4).hex()}")

    # Clean up previous run
    for f in [vmo, cmo, cbin + ".exe", cbin + ".c", cbin]:
        try:
            os.remove(f)
        except OSError:
            pass

    # VM interpreter
    try:
        with open(vmo, "w", encoding="utf-8", errors="replace") as fout:
            result = subprocess.run(
                [BIN_BIN, fullpath],
                stdout=fout,
                stderr=subprocess.STDOUT,
                timeout=120,
                cwd=PROJECT_ROOT,
            )
        if result.returncode != 0:
            print(f"VM-FAIL {base}")
            with open(vmo, "r", encoding="utf-8", errors="replace") as f:
                print(f.read())
            return "fail"
    except subprocess.TimeoutExpired:
        print(f"VM-TIMEOUT {base}")
        return "fail"
    except Exception as e:
        print(f"VM-ERROR {base}: {e}")
        return "fail"

    # Check if VM output is empty
    vmo_size = os.path.getsize(vmo) if os.path.exists(vmo) else 0
    if vmo_size == 0:
        print(f"VM-EMPTY {base} (size=0)")
        return "fail"

    # Compile channel
    try:
        result = subprocess.run(
            [BIN_BIN, "-c", fullpath, "-o", cbin],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=120,
            cwd=PROJECT_ROOT,
        )
        if result.returncode != 0:
            # Retry once
            import time
            time.sleep(2)
            cbin = os.path.join(tmp_dir, f"{name}_bin_{os.getpid()}_{os.urandom(4).hex()}")
            result = subprocess.run(
                [BIN_BIN, "-c", fullpath, "-o", cbin],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=120,
                cwd=PROJECT_ROOT,
            )
        if result.returncode != 0:
            print(f"CC-GEN-FAIL {base}")
            return "fail"
    except subprocess.TimeoutExpired:
        print(f"CC-GEN-TIMEOUT {base}")
        return "fail"
    except Exception as e:
        print(f"CC-GEN-ERROR {base}: {e}")
        return "fail"

    # Find compiled binary
    bin_path = None
    for ext in [".exe", ""]:
        candidate = cbin + ext
        if os.path.exists(candidate):
            bin_path = candidate
            break
    if bin_path is None:
        print(f"CC-NOBIN {base}")
        return "fail"

    # Run compiled binary
    try:
        with open(cmo, "w", encoding="utf-8", errors="replace") as fout:
            result = subprocess.run(
                [bin_path],
                stdout=fout,
                stderr=subprocess.STDOUT,
                timeout=120,
                cwd=PROJECT_ROOT,
            )
        if result.returncode != 0:
            print(f"CC-RUN-FAIL {base}")
            return "fail"
    except subprocess.TimeoutExpired:
        print(f"CC-RUN-TIMEOUT {base}")
        return "fail"
    except Exception as e:
        print(f"CC-RUN-ERROR {base}: {e}")
        return "fail"

    # Compare stdout (binary comparison, with timestamp normalization)
    try:
        with open(vmo, "rb") as f:
            vm_data = f.read()
        with open(cmo, "rb") as f:
            cc_data = f.read()

        # Normalize timestamps: [YYYY-MM-DD HH:MM:SS] -> [TIMESTAMP]
        # This handles log functions that output current time to stderr
        timestamp_pattern = re.compile(rb'\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\]')
        vm_norm = timestamp_pattern.sub(b'[TIMESTAMP]', vm_data)
        cc_norm = timestamp_pattern.sub(b'[TIMESTAMP]', cc_data)

        # Filter out FFI Warning lines and other runtime warnings (VM vs CC difference)
        warning_pattern = re.compile(rb'^.*(FFI Warning|Warning:|Runtime Warning).*$\n?', re.MULTILINE)
        vm_norm = warning_pattern.sub(b'', vm_norm)
        cc_norm = warning_pattern.sub(b'', cc_norm)

        if vm_norm != cc_norm:
            print(f"DIFF   {base}")
            # Save diff files for debugging
            shutil.copy2(vmo, os.path.join(PROJECT_ROOT, f"_diff_vm_{base}.out"))
            shutil.copy2(cmo, os.path.join(PROJECT_ROOT, f"_diff_cc_{base}.out"))
            return "fail"
        else:
            print(f"PASS   {base}")
            return "pass"
    except Exception as e:
        print(f"COMPARE-ERROR {base}: {e}")
        return "fail"
    finally:
        # Cleanup
        for f in [vmo, cmo]:
            try:
                os.remove(f)
            except OSError:
                pass
        # Cleanup compiled binaries
        for pattern in [f"{name}_bin_*.exe", f"{name}_bin_*.c", f"{name}_bin_*"]:
            for f in glob.glob(os.path.join(tmp_dir, pattern)):
                try:
                    os.remove(f)
                except OSError:
                    pass


def main():
    if not os.path.exists(BIN_BIN):
        print(f"[ERROR] {BIN_BIN} missing, run build.bat build first")
        sys.exit(1)

    # Clean and recreate temp dir
    if os.path.exists(TMP_DIR):
        shutil.rmtree(TMP_DIR, ignore_errors=True)
    os.makedirs(TMP_DIR, exist_ok=True)

    pass_count = 0
    fail_count = 0
    skip_count = 0

    print("=== Dual-channel regression test ===")
    print()

    # Find all test files
    test_files = sorted(glob.glob(os.path.join(PROJECT_ROOT, "tests", "*.lm")))

    for fullpath in test_files:
        base = os.path.basename(fullpath)
        name = os.path.splitext(base)[0]

        if base in SKIP_LIST:
            skip_count += 1
            print(f"SKIP   {base}")
            continue

        result = run_test(fullpath, base, name, TMP_DIR)
        if result == "pass":
            pass_count += 1
        else:
            fail_count += 1

    print()
    print(f"=== pass={pass_count} fail={fail_count} skip={skip_count} ===")

    # Cleanup temp dir
    if os.path.exists(TMP_DIR):
        shutil.rmtree(TMP_DIR, ignore_errors=True)

    sys.exit(0 if fail_count == 0 else 1)


if __name__ == "__main__":
    main()
