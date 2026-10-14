# Copyright (c) 2026 SugaBlaz
# This software is released under the MIT License.
# https://github.com/SugaBlaz/PRJ_SUGA-27-ANDROID-TERMUX

# Device modules
import sys
import os

# Check if lazy imports are natively supported by the runtime environment
lazy_imports_supported = sys.version_info >= (3, 15)

# Modules (Required on start)
from other.larp import fake_download, fake_compile, simulate_dependencies, main
from yaspin import yaspin
import subprocess

fake_download("libnet-exploit-v4.1.2.tar.gz", 14.8)
fake_download("sg-modules-v2.3.1.zip", 6.2)
fake_download("open-net-v1.1.2.tar.gz", 20.7)
simulate_dependencies()
fake_compile()

if sys.platform.startswith("win"):
    subprocess.run(["cls"], shell=True)
else:
    subprocess.run(["clear"])

spinner = yaspin(text="Loading terminal...", color="yellow")
spinner.start()

if lazy_imports_supported:
    import importlib.util

    def lazy_load(module_name):
        spec = importlib.util.find_spec(module_name)
        loader = importlib.util.LazyLoader(spec.loader)
        spec.loader = loader
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        loader.exec_module(module)
        return module

    # Lazy-loaded modules
    sbcli = lazy_load("cli.sbcli")
    network = lazy_load("helper.network")
    installer = lazy_load("helper.installer")
    config_saver = lazy_load("cli.config_saver")
    wifi_DoS = lazy_load("attacks.wifi_DoS")
    http_DoS = lazy_load("attacks.http_DoS")
    arp_posioner = lazy_load("attacks.arp_posioner")

    # Dynamic target pointers (Mimics direct import variables cleanly)
    ModularTerminal = sbcli.ModularTerminal
    check_for_updates = network.check_for_updates
    download_dependencies = installer.download_dependencies
    set_installer_dirs = installer.set_dirs
    installer_init = installer.init
    save_config = config_saver.save_config
    load_config = config_saver.load_config
    DOS_WIFI = wifi_DoS.DOS_WIFI
    wifi_init = wifi_DoS.init
    set_wifi_dirs = wifi_DoS.set_dirs
    DOS_HTTP = http_DoS.DOS_HTTP
    http_init = http_DoS.init
    set_http_dirs = http_DoS.set_dirs
    ARP_POISON = arp_posioner.ARP_POISON
    arp_init = arp_posioner.init
    set_arp_dirs = arp_posioner.set_dirs

else:
    # main.py
    from cli.sbcli import ModularTerminal

    # Others
    from helper.network import check_for_updates

    # Helpers
    from helper.installer import download_dependencies, set_dirs as set_installer_dirs, init as installer_init
    from cli.config_saver import save_config, load_config

    # Attacks
    from attacks.wifi_DoS import DOS_WIFI, init as wifi_init, set_dirs as set_wifi_dirs
    from attacks.http_DoS import DOS_HTTP, init as http_init, set_dirs as set_http_dirs
    from attacks.arp_posioner import ARP_POISON, init as arp_init, set_dirs as set_arp_dirs

BANNER = r"""                                                                  
                                                                        
█████▄ █████▄     ██       ▄█████ ██  ██  ▄████  ▄████▄    ████▄ ██████ 
██▄▄█▀ ██▄▄██▄    ██       ▀▀▀▄▄▄ ██  ██ ██  ▄▄▄ ██▄▄██ ▄▄▄ ▄██▀   ▄██▀ 
██     ██   ██ ████▀ ▄▄▄▄▄ █████▀ ▀████▀  ▀███▀  ██  ██    ███▄▄  ██▀                                                                         

"""

config: dict = load_config()

username = config.get("username", "user") if config else "user"
sbcli = ModularTerminal(prompt=f"sbcli: {username}> ")

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

NATIVE_DIR = os.path.join(BASE_DIR, "native")
OTHER_DIR = os.path.join(BASE_DIR, "other")

C_FILE_WIFI = os.path.join(NATIVE_DIR, "fast_packet_wifi.c")
LIB_FILE_WIFI = os.path.join(
    NATIVE_DIR,
    "fast_packet_wifi.so"
)

C_FILE_HTTP = os.path.join(NATIVE_DIR, "fast_packet_http.c")
LIB_FILE_HTTP = os.path.join(
    NATIVE_DIR,
    "fast_packet_http.so"
)

C_FILE_ARP = os.path.join(NATIVE_DIR, "fast_packet_arp.c")
LIB_FILE_ARP = os.path.join(
    NATIVE_DIR,
    "fast_packet_arp.so"
)

VERSION_TXT_FILE = os.path.join(OTHER_DIR, "version.txt")

def set_module_dirs():
    set_wifi_dirs(C_FILE_WIFI, LIB_FILE_WIFI)
    set_http_dirs(C_FILE_HTTP, LIB_FILE_HTTP)
    set_arp_dirs(C_FILE_ARP, LIB_FILE_ARP)
    
    set_installer_dirs(
        C_FILE_WIFI,
        LIB_FILE_WIFI,
        C_FILE_HTTP,
        LIB_FILE_HTTP,
        C_FILE_ARP,
        LIB_FILE_ARP
    )

def change_username(new_username: str):
    """Changes the username displayed in the terminal prompt and saves it to the config file."""
    
    dataToSave = {"username": new_username}
    save_config(dataToSave, True)
        
    sbcli.change_localuser(new_username)

def start():
    # Set Modules Directories
    set_module_dirs()
    
    # Initialize loggings bridge
    wifi_init(sbcli)
    http_init(sbcli)
    arp_init(sbcli)
    installer_init(sbcli)

    # Register commands
    sbcli.create_command(
        "larp",
        main,
        aliases=["init", "setup", "protocol"]
    )
    
    sbcli.create_command(
        "DoS-WiFi",
        DOS_WIFI,
        aliases=["wifiDoS", "doswifi", "wifidos"]
    )
    
    sbcli.create_command(
        "DoS-HTTP/S",
        DOS_HTTP,
        aliases=["httpDoS", "doshttp", "httpdos"]
    )
    
    sbcli.create_command(
        "ARP-poison",
        ARP_POISON,
        aliases=["ARP-spoof", "wifipoison", "killwifi", "wifispoof"]
    )
    
    sbcli.create_command(
        "install_dependencies",
        download_dependencies,
        aliases=["download_dependencies", "downloaddep", "installdep"]
    )

    sbcli.create_command(
        "change_username",
        change_username,
        aliases=["changeuser", "username", "newuser"]
    )
    
    sbcli.run_loop()

if __name__ == "__main__":
    spinner.stop()

    sbcli._log(BANNER)
    sbcli._log("SIAPA KITA? nullSBH! UNTUK KEADILAN DUNIA SIBER!\n")
    
    sbcli._log("[*] CLI ini dibuat oleh sbh.cyber dan nullsector.\n")
    
    with yaspin(text="Connecting to server...", color="yellow") as _:
        msg = check_for_updates(VERSION_TXT_FILE)
        
    if msg:
        sbcli._log("[*] " + msg + "\n")
        
    sbcli._log("Di mata nullSBH, kita lihat semua kelakuan kamu. Jangan mejadi kiriminal siber, karena KAMI TIDAK TOLERNASI.")
    sbcli._log("#MakeTheInternetSafeAgain #nullSBH #RedHat #JanganJadiKriminalSiber \n")
    
    start()