# Copyright (c) 2026 SugaBlaz
# This software is released under the MIT License.
# https://github.com/SugaBlaz/PRJ_SUGA-27-ANDROID-TERMUX

import subprocess
import os
import sys
import shutil
import platform

C_FILE_WIFI = None
LIB_FILE_WIFI = None
C_FILE_HTTP = None
LIB_FILE_HTTP = None

TERMINAL = None

SSL_REQUIRED = {
    "httpS DoS Module"
}

PTHREAD_REQUIRED = {
    "httpS DoS Module",
    "WiFI DoS Module",
}

def log(msg: str):
    """Safely logs to terminal instance or stdout fallback."""
    if TERMINAL and hasattr(TERMINAL, "_log"):
        TERMINAL._log(msg)
    else:
        print(msg, flush=True)

def init(terminal):
    global TERMINAL
    TERMINAL = terminal

def set_dirs(
    cfilewifi,
    libfilewifi,
    cfilehttp,
    libfilehttp,
    cfilearp,
    libfilearp,
):
    global C_FILE_WIFI, LIB_FILE_WIFI, C_FILE_HTTP, LIB_FILE_HTTP, C_FILE_ARP, LIB_FILE_ARP
    C_FILE_WIFI = cfilewifi
    LIB_FILE_WIFI = libfilewifi
    C_FILE_HTTP = cfilehttp
    LIB_FILE_HTTP = libfilehttp
    C_FILE_ARP = cfilearp
    LIB_FILE_ARP = libfilearp

def verify_android_environment():
    """Bypass strict sys.platform blocks to verify the Android/Termux environment."""

    # 1. Broadly check for Linux kernel signs via platform.system() or environment variables
    # This acts as a fallback if sys.platform is acting weird on your Redmi
    is_linux_like = (
        sys.platform == "linux" 
        or platform.system().lower() == "linux"
        or "ANDROID_DATA" in os.environ
    )

    if not is_linux_like:
        # Let's see what it is actually evaluating to debug if it fails
        raise RuntimeError(
            f"This native installer is intended for Android/Termux. "
            f"(Detected platform string: '{sys.platform}', System: '{platform.system()}')"
        )

    # 2. BULLETPROOF TERMUX DETECTION: Look for sandboxed Termux footprints
    termux_internal_prefix = "/data/data/com.termux/files/usr/bin"
    termux_env_present = "TERMUX_VERSION" in os.environ
    
    # Check if Termux storage system or standard Android environment variables exist
    is_in_termux_path = any(
        "com.termux" in path for path in [os.getcwd(), sys.executable, os.path.expanduser("~")]
    )
    has_android_paths = "ANDROID_ROOT" in os.environ or "ANDROID_DATA" in os.environ

    if not (termux_env_present or os.path.exists(termux_internal_prefix) or is_in_termux_path or has_android_paths):
        raise RuntimeError(
            "Android environment not detected. "
            "Please run this installer inside Termux on Android."
        )

    # 3. Check for Clang using standard lookups and fallback absolute paths
    clang_path = (
        shutil.which("clang")
        or os.path.join(termux_internal_prefix, "clang")
    )

    if not (
        (clang_path and os.path.exists(clang_path))
        or shutil.which("clang")
    ):
        raise RuntimeError(
            "Clang compiler not found! "
            "Run 'pkg install clang' in Termux first."
        )

    openssl_pkg_config = shutil.which("pkg-config")

    if openssl_pkg_config:
        result = subprocess.run(
            [openssl_pkg_config, "--exists", "openssl"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        if result.returncode != 0:
            raise RuntimeError(
                "OpenSSL development files not found! "
                "Run 'pkg install openssl' in Termux first."
            )
    else:
        openssl_header = os.path.join(
            termux_internal_prefix,
            "include",
            "openssl",
            "ssl.h"
        )

        if not os.path.isfile(openssl_header):
            raise RuntimeError(
                "OpenSSL headers not found! "
                "Run 'pkg install openssl' in Termux first."
            )

def compile_c_module(c_source: str, lib_target: str, module_name: str):
    verify_android_environment()

    # Enforce .so extension for Android dynamic shared libraries
    if not lib_target.endswith(".so"):
        lib_target = os.path.splitext(lib_target)[0] + ".so"

    log(f"[*] Compiling {module_name} with Clang (-O3) for Android...")

    termux_prefix = os.environ.get(
        "PREFIX",
        "/data/data/com.termux/files/usr"
    )

    flags = []

    if module_name in SSL_REQUIRED:
        flags.extend([
            f"-I{os.path.join(termux_prefix, 'include')}",
            f"-L{os.path.join(termux_prefix, 'lib')}",
            "-lssl",
            "-lcrypto",
        ])
    elif module_name in PTHREAD_REQUIRED:
        flags.extend([
            "-pthread"
        ])

    # High-performance flags for ARM/Android via Clang
    base_flags = [
        "clang",
        "-O3",
        "-shared",
        "-fPIC",
        "-o", 
        lib_target,
        c_source,
        *flags,
    ]

    try:
        subprocess.run(
            base_flags,
            check=True,
            capture_output=True,
            text=True
        )

        log(f"[+] Successfully built {lib_target}")
        return

    except subprocess.CalledProcessError as e:
        log(
            f"[!] Primary Clang compilation failed for "
            f"{c_source}. Retrying with safe fallbacks..."
        )

    # Safe fallback compilation flags
    fallback_flags = [
        "clang",
        "-O2",
        "-shared",
        "-fPIC",
        "-o", lib_target,
        c_source,
        *flags,
    ]

    try:
        subprocess.run(
            fallback_flags,
            check=True,
            capture_output=True,
            text=True
        )

        log(
            f"[+] Successfully built {lib_target} "
            f"using safe fallback profile."
        )

    except subprocess.CalledProcessError as e:
        log(f"[-] Critical: Clang Compilation Failed for {c_source}!")
        log(f"[-] Error output:\n{e.stderr}")

        raise RuntimeError(
            f"Failed to build C extension library: {module_name}"
        )

def check_and_rebuild(c_source: str, lib_target: str, module_name: str):
    needs_recompile = False

    if not os.path.exists(lib_target):
        needs_recompile = True
    elif os.path.exists(c_source):
        c_mtime = os.path.getmtime(c_source)
        lib_mtime = os.path.getmtime(lib_target)

        if c_mtime > lib_mtime:
            log(f"[*] Detected changes in {c_source}!")
            needs_recompile = True

    if needs_recompile:
        if os.path.exists(lib_target):
            try:
                os.remove(lib_target)
            except PermissionError:
                log(f"[-] Error: Could not delete native library '{lib_target}' (file in use).")
                return

        compile_c_module(c_source, lib_target, module_name)

def download_dependencies():
    """Complies the native C modules."""
    # 1. WiFi Packet Module
    if C_FILE_WIFI and os.path.exists(C_FILE_WIFI):
        check_and_rebuild(C_FILE_WIFI, LIB_FILE_WIFI, "WiFi DoS Module")

    # 2. HTTP Stress Module
    if C_FILE_HTTP and os.path.exists(C_FILE_HTTP):
        check_and_rebuild(C_FILE_HTTP, LIB_FILE_HTTP, "httpS DoS Module")

    log("[!] Finished checking and building native Android dependencies.")
