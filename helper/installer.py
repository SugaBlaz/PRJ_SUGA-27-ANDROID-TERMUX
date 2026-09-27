import subprocess
import os
import sys
import shutil

C_FILE_WIFI = None
LIB_FILE_WIFI = None
C_FILE_HTTP = None
LIB_FILE_HTTP = None

TERMINAL = None

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
):
    global C_FILE_WIFI, LIB_FILE_WIFI, C_FILE_HTTP, LIB_FILE_HTTP
    C_FILE_WIFI = cfilewifi
    LIB_FILE_WIFI = libfilewifi
    C_FILE_HTTP = cfilehttp
    LIB_FILE_HTTP = libfilehttp

def verify_android_environment():
    """Bypass Xiaomi HyperOS/MIUI storage restrictions to verify Termux."""
    
    # 1. Fallback system platform check
    if sys.platform != "linux":
        raise RuntimeError(
            "This native installer is intended for Android/Termux."
        )

    # 2. BULLETPROOF REDMI/XIAOMI DETECTION: Check Termux internal data storage structure
    # This path is entirely managed inside the Termux sandbox and cannot be blocked by MIUI/HyperOS
    termux_internal_prefix = "/data/data/com.termux/files/usr/bin"
    termux_env_present = "TERMUX_VERSION" in os.environ
    
    # Check if we are physically running inside the Termux folder hierarchy
    is_in_termux_path = any(
        "com.termux" in path for path in [os.getcwd(), sys.executable, os.path.expanduser("~")]
    )

    if not (termux_env_present or os.path.exists(termux_internal_prefix) or is_in_termux_path):
        raise RuntimeError(
            "Android environment not detected. "
            "Please run this installer inside Termux on Android."
        )

    # 3. Check for Clang using Termux absolute path fallbacks
    clang_path = shutil.which("clang") or os.path.join(termux_internal_prefix, "clang")
    if not os.path.exists(clang_path) if clang_path else False:
        raise RuntimeError(
            "Clang compiler not found! "
            "Run 'pkg install clang' in Termux first."
        )

def compile_c_module(c_source: str, lib_target: str, module_name: str):
    verify_android_environment()

    # Enforce .so extension for Android dynamic shared libraries
    if not lib_target.endswith(".so"):
        lib_target = os.path.splitext(lib_target)[0] + ".so"

    log(f"[*] Compiling {module_name} with Clang (-O3) for Android...")

    # High-performance flags for ARM/Android via Clang
    base_flags = [
        "clang",
        "-O3",
        "-shared",
        "-fPIC",
        "-pthread",
        "-o", lib_target,
        c_source
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
        log(f"[!] Primary Clang compilation failed for {c_source}. Retrying with safe fallbacks...")

    # Safe fallback compilation flags
    fallback_flags = [
        "clang",
        "-O2",
        "-shared",
        "-fPIC",
        "-o", lib_target,
        c_source
    ]

    try:
        subprocess.run(
            fallback_flags,
            check=True,
            capture_output=True,
            text=True
        )
        log(f"[+] Successfully built {lib_target} using safe fallback profile.")
    except subprocess.CalledProcessError as e:
        log(f"[-] Critical: Clang Compilation Failed for {c_source}!")
        log(f"[-] Error output:\n{e.stderr}")
        raise RuntimeError(f"Failed to build C extension library: {module_name}")

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
                sys.exit(1)

        compile_c_module(c_source, lib_target, module_name)

def download_dependencies():
    """Complies the native C modules."""
    # 1. WiFi Packet Module
    if C_FILE_WIFI and os.path.exists(C_FILE_WIFI):
        check_and_rebuild(C_FILE_WIFI, LIB_FILE_WIFI, "WiFi DoS Module")

    # 2. HTTP Stress Module
    if C_FILE_HTTP and os.path.exists(C_FILE_HTTP):
        check_and_rebuild(C_FILE_HTTP, LIB_FILE_HTTP, "HTTP DoS Module")

    log("[!] Finished checking and building native Android dependencies.")
