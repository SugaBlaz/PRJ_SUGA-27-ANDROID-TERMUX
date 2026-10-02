from blessed import Terminal
import time
import random
import sys

term = Terminal()

SERVERS = [
    "US-EAST-DARPA-GRID-01",
    "EU-CENTRAL-SWIFT-NODE-09",
    "ASIA-PAC-SATCOMM-RELAY-14",
    "ORBITAL-DEFENSE-ARRAY-XX"
]

def typing_effect(text, speed=0.02):
    """Simulates realistic Hollywood hacker typing."""
    for char in text:
        sys.stdout.write(char)
        sys.stdout.flush()
        time.sleep(speed)
    print()

def fake_download(package_name, size_mb):
    """Simulates a fast, realistic CLI package download with network stutters."""
    print(f"[*] Fetching remote package: {package_name} ({size_mb} MB)...")
    time.sleep(0.3)
    
    bar_length = 40
    for i in range(bar_length + 1):
        percent = int((i / bar_length) * 100)
        downloaded = round((i / bar_length) * size_mb, 1)
        
        bar = "█" * i + "-" * (bar_length - i)
        sys.stdout.write(f"\r    {percent}% [{bar}] {downloaded}/{size_mb} MB  {random.randint(2400, 8500)} KB/s")
        sys.stdout.flush()
        
        # Fast network pacing
        time.sleep(random.choice([0.01, 0.03, 0.08 if i % 12 == 0 else 0.02]))
    print("\n[+] Download complete. Verifying SHA256 checksum... OK")
    time.sleep(0.4)

def simulate_dependencies():
    """Simulates a quick tree check of system components."""
    print("\n[*] Resolving environment dependencies...")
    time.sleep(0.2)
    deps = [
        "glibc >= 2.3.5 ........................................ [FOUND]",
        "libssl-dev >= 3.0.2 ................................... [FOUND]",
        "libcap2-bin ........................................... [FOUND]",
        "iptables-mod-raw ...................................... [FOUND]",
        "kernel-headers-generic ................................ [FOUND]",
        "tor-net >= 2.4.1 ...................................... [FOUND]"
    ]
    for dep in deps:
        print(f"    {dep}")
        time.sleep(0.1)

def fake_compile():
    """Dumps a rapid, massive stream of compiler logs, warnings, and linking stages."""
    print("\n[*] Initializing toolchain configuration...")
    time.sleep(0.3)
    
    configs = [
        "checking for gcc... yes",
        "checking whether the C compiler works... yes",
        "checking for local system architecture... x86_64_uclibc",
        "checking for BPF compiler backend... enabled",
        "checking security sandbox mitigations... bypassed (FORCE_RAW=1)",
        "checking for systemd watchdog timers... masked",
        "creating Makefile... done"
    ]
    for config in configs:
        print(f"    {config}")
        time.sleep(0.05)
        
    print("\n[*] Compiling native C components (gcc -O3 -march=native -fPIC)...")
    time.sleep(0.4)

    # Large array of technical-sounding C files
    modules = [
        "core_hook", "payload_alloc", "mem_inject", "sig_bypass", 
        "net_tunnel", "crypto_layer", "stack_smash", "ptrace_anti",
        "elf_patcher", "vaddr_scanner", "socket_raw", "daemon_fork"
    ]
    
    for mod in modules:
        print(f"  CC      src/{mod}.c")
        # Faster compilation speed, like a real multi-threaded build (make -j8)
        time.sleep(random.uniform(0.05, 0.2))
        
        # Real-looking complex compiler warning
        if mod == "mem_inject":
            print(f"src/{mod}.c:42:11: warning: cast from pointer to integer of different size [-Wpointer-to-int-cast]")
            print(f"   42 |   addr = (uint32_t)sys_call_ptr;")
            print(f"      |           ^")
            time.sleep(0.2)
        elif mod == "stack_smash":
            print(f"src/{mod}.c:118:24: warning: implicit declaration of function ‘__builtin_frame_address’ [-Wimplicit-function-declaration]")
            time.sleep(0.1)

    # Dynamic Memory Mapping Simulation (Looks like active system exploitation)
    print("\n[*] Mapping raw virtual memory offsets for runtime hooks...")
    time.sleep(0.3)
    for _ in range(15):
        base_addr = random.randint(0x7FFF00000000, 0x7FFFFFFFFFFF)
        offset = random.randint(0x1000, 0x9000)
        print(f"  [MEM] Synced page index at 0x{base_addr:X} (+0x{offset:04X}) -> RX-Permission OK")
        time.sleep(0.03)

    # Link phase
    print("\n[*] Linking objects and generating static binary...")
    time.sleep(0.5)
    print(f"  LD      bin/libunhook.so -> [0x{random.randint(268435456, 4294967295):08X}]")
    print(f"  STRIP   bin/payload_daemon (compressed via UPX)")
    time.sleep(0.3)
    
    # Finalize setup
    print("\n[+] Build successful.")
    print("[*] Setting binary execution privileges (chmod +x bin/payload_daemon)...")
    time.sleep(0.2)
    print("[*] Injecting local environment stubs into /dev/shm/...")
    time.sleep(0.4)
    
    print("\n" + "="*65)
    print("  SUCCESS: Tool successfully compiled and deployed.")
    print("  Local configuration locked."))
    print("="*65 + "\n")

def fake_progress_bar(task_name, duration=2.5):
    """Renders a high-tech progress loading bar using Blessed styling."""
    print(term.cyan(f"[*] {task_name}..."))
    bar_length = 32
    for i in range(bar_length + 1):
        percent = int((i / bar_length) * 100)
        filled = "=" * i
        spaces = " " * (bar_length - i)
        
        # Color transition from yellow to bold green
        color = term.yellow if percent < 70 else term.bold_green
        bar_str = color(f"[{filled}{spaces}]")
        
        sys.stdout.write(f"\r    {bar_str} {term.bold(str(percent))}%")
        sys.stdout.flush()
        time.sleep(duration / bar_length)
    print(f" {term.bold_green('[COMPLETE]')}\n")

def generate_hex_dump(lines=8):
    """Outputs random memory hex addresses with styled highlights."""
    for _ in range(lines):
        address = f"0x{random.randint(0x10000000, 0xFFFFFFFF):08X}"
        bytes_hex = " ".join(f"{random.randint(0, 255):02X}" for _ in range(8))
        
        addr_str = term.darkorange(address)
        hex_str = term.gray(bytes_hex)
        status_str = term.bold_red("OVERWRITE_OK") if random.random() > 0.7 else term.green("MEM_OK")
        
        print(f"  {addr_str}  |  {hex_str}  |  {status_str}")
        time.sleep(0.04)

def main():  
    print(term.center(term.bold_black_on_green(" [ INITIALIZING PROJECT: PRJ_SUGA-27 ] ")))
    print("\n" + term.darkorange("=" * term.width))
    time.sleep(0.5)

    typing_effect(term.bold_white("[+] Establishing encrypted SSH tunnel over TOR nodes..."))
    time.sleep(0.3)
    
    # Target Selection
    target = random.choice(SERVERS)
    typing_effect(f"{term.bold_white('[+] Target Locked:')} {term.bold_red(target)}")
    print(term.darkorange("-" * term.width))

    # Simulated Execution Steps
    fake_progress_bar("Bypassing Firewall & Intrusion Detection Systems")
    
    print(term.cyan("[*] Injecting Zero-Day Payload into kernel memory..."))
    generate_hex_dump(10)
    print(term.bold_green("[+] Memory buffer exploited. Shell code injected successfully.\n"))

    fake_progress_bar("Rerouting Satellite Uplink")
    fake_progress_bar("Clearing System Logs & Wiping /var/log/auth.log")

    # Dramatic Alarm Interruption
    print(term.darkorange("-" * term.width))
    time.sleep(0.4)
    print(term.blink(term.bold_white_on_red(" [!] ALERT: TRACE DETECTED BY COUNTER-MEASURES! ")))
    print()
    
    for i in range(3, 0, -1):
        sys.stdout.write(f"\r{term.bold_red(f'    >>> PURGING PROXY NODES IN {i} SECONDS <<<')}")
        sys.stdout.flush()
        time.sleep(0.8)
    print("\n")

    # Final Success Message
    print(term.bold_green_on_black("=" * term.width))
    print(term.center(term.bold_green("ACCESS GRANTED. ROOT PRIVILEGES OBTAINED.")))
    print(term.bold_green_on_black("=" * term.width) + "\n")