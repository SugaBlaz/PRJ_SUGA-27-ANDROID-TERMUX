# Third Party Modules (Required on start)
from yaspin import yaspin

spinner = yaspin(text="Loading terminal...", color="yellow")
spinner.start()

# main.py
from cli.sbcli import ModularTerminal

import other.larp as larp
from helper.network import check_for_updates

# Helpers
import helper.installer as installer
from cli.config_saver import save_config, load_config

# Attacks
import attacks.wifi_DoS as wifi_DoS
import attacks.http_DoS as http_Dos
import attacks.arp_posioner as arp_posioner

# Third Party Modules
import os

BANNER = r"""                                                                  
                                                                        
█████▄ █████▄     ██       ▄█████ ██  ██  ▄████  ▄████▄    ████▄ ██████ 
██▄▄█▀ ██▄▄██▄    ██       ▀▀▀▄▄▄ ██  ██ ██  ▄▄▄ ██▄▄██ ▄▄▄ ▄██▀   ▄██▀ 
██     ██   ██ ████▀ ▄▄▄▄▄ █████▀ ▀████▀  ▀███▀  ██  ██    ███▄▄  ██▀                                                                         

"""

config = load_config()

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
    wifi_DoS.set_dirs(C_FILE_WIFI, LIB_FILE_WIFI)
    http_Dos.set_dirs(C_FILE_HTTP, LIB_FILE_HTTP)
    arp_posioner.set_dirs(C_FILE_ARP, LIB_FILE_ARP)
    
    installer.set_dirs(
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
    
    sbcli.change_localuser(new_username)
    save_config(dataToSave, True)

def start():
    # Set Modules Directories
    set_module_dirs()
    
    # Initialize loggings bridge
    wifi_DoS.init(sbcli)
    http_Dos.init(sbcli)
    installer.init(sbcli)
    arp_posioner.init(sbcli)

    # Register commands
    sbcli.create_command(
        "larp",
        larp.main,
        aliases=["init", "setup", "protocol"]
    )
    
    sbcli.create_command(
        "DoS-WiFi",
        wifi_DoS.DOS_WIFI,
        aliases=["wifiDoS", "doswifi", "wifidos"]
    )
    
    sbcli.create_command(
        "DoS-HTTP/S",
        http_Dos.DOS_HTTP,
        aliases=["httpDoS", "doshttp", "httpdos"]
    )
    
    sbcli.create_command(
        "ARP-poison",
        arp_posioner.ARP_POISON,
        aliases=["ARP-spoof", "wifispoof", "killwifi"]
    )
    
    sbcli.create_command(
        "install_dependencies",
        installer.download_dependencies,
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
    
    with yaspin(text="Connecting to server...", color="yellow") as _:
        msg = check_for_updates(VERSION_TXT_FILE)
        
    if msg:
        sbcli._log(msg)
    
    start()