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