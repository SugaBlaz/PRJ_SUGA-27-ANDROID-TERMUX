import requests
import subprocess
import socket
import os
import struct
from concurrent.futures import ThreadPoolExecutor

def get_public_ip():
    try:
        # Query the ipify API for a plain text IP response
        response = requests.get('https://api.ipify.org')
        return response.text
    except requests.RequestException as e:
        return "127.0.0.1"
    
def interface_exists(interface_name):
    """Checks the Linux network subsystem directly for interface presence."""
    return os.path.exists(f"/sys/class/net/{interface_name}")

def get_iface_mac(iface):
    try:
        with open(f"/sys/class/net/{iface}/address") as f: return f.read().strip()
    except: return None

def get_gateway_info():
    try:
        with open("/proc/net/route") as f:
            for line in f.readlines()[1:]:
                parts = line.split()
                if parts[1] == "00000000": 
                    ip_hex = int(parts[2], 16)
                    return socket.inet_ntoa(struct.pack("<L", ip_hex))
    except: pass
    return None

def get_mac_from_arp(ip):
    try:
        with open("/proc/net/arp") as f:
            for line in f.readlines()[1:]:
                parts = line.split()
                if parts[0] == ip and parts[3] != "00:00:00:00:00:00":
                    return parts[3]
    except: pass
    return None

def ping_sweeper(ip):
    subprocess.call(["ping", "-c", "1", "-W", "1", ip], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def scan_network(iface, gateway_ip):
    print(f"[+] Scanning network subnet (Gateway: {gateway_ip})...")
    base_ip = ".".join(gateway_ip.split(".")[:3])
    ips_to_scan = [f"{base_ip}.{i}" for i in range(1, 255)]
    
    with ThreadPoolExecutor(max_workers=50) as executor:
        executor.map(ping_sweeper, ips_to_scan)
    
    targets = []
    try:
        with open("/proc/net/arp") as f:
            for line in f.readlines()[1:]:
                parts = line.split()
                if len(parts) >= 6:
                    ip, mac, dev = parts[0], parts[3], parts[5]
                    if dev == iface and mac != "00:00:00:00:00:00" and ip != gateway_ip:
                        targets.append((ip, mac))
    except: pass
    return targets