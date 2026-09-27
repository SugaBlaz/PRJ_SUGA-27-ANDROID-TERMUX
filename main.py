# main.py
from cli.sbcli import ModularTerminal

import other.larp as larp
import helper.installer as installer
import attacks.attacks as attacks

# Third Party Modules
import os

BANNER = r"""
░▒▓███████▓▒░░▒▓███████▓▒░       ░▒▓█▓▒░░▒▓███████▓▒░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░ ░▒▓██████▓▒░░▒▓███████▓▒░░▒▓████████▓▒░ 
░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░      ░▒▓█▓▒░▒▓█▓▒░      ░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░      ░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░ 
░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░      ░▒▓█▓▒░▒▓█▓▒░      ░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░      ░▒▓█▓▒░░▒▓█▓▒░      ░▒▓█▓▒░      ░▒▓█▓▒░ 
░▒▓███████▓▒░░▒▓███████▓▒░       ░▒▓█▓▒░░▒▓██████▓▒░░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒▒▓███▓▒░▒▓████████▓▒░░▒▓██████▓▒░      ░▒▓█▓▒░  
░▒▓█▓▒░      ░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░      ░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░            ░▒▓█▓▒░  
░▒▓█▓▒░      ░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░      ░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░           ░▒▓█▓▒░   
░▒▓█▓▒░      ░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░░▒▓███████▓▒░ ░▒▓██████▓▒░ ░▒▓██████▓▒░░▒▓█▓▒░░▒▓█▓▒░▒▓████████▓▒░    ░▒▓█▓▒░
    """
    
username = "SugaBlaz"
sbcli = ModularTerminal(prompt=f"sbcli: {username}> ")

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

NATIVE_DIR = os.path.join(BASE_DIR, "native")

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

def set_module_dirs():
    attacks.set_dirs(C_FILE_WIFI, LIB_FILE_WIFI, C_FILE_HTTP, LIB_FILE_HTTP)
    installer.set_dirs(C_FILE_WIFI, LIB_FILE_WIFI, C_FILE_HTTP, LIB_FILE_HTTP)

def start():
    # Set Modules Directories
    set_module_dirs()
    
    # Initialize loggings bridge
    attacks.init(sbcli)
    installer.init(sbcli)

    # Register commands
    sbcli.create_command(
        "larp",
        larp.main,
        aliases=["init", "setup", "protocol"]
    )
    
    sbcli.create_command(
        "DoS-WiFi",
        attacks.DOS_WIFI,
        aliases=["wifiDoS", "doswifi", "wifidos"]
    )
    
    sbcli.create_command(
        "DoS-HTTP/S",
        attacks.DOS_HTTP,
        aliases=["httpDoS", "doshttp", "httpdos"]
    )
    
    sbcli.create_command(
        "install_dependencies",
        installer.download_dependencies,
        aliases=["download_dependencies", "downloaddep", "installdep"]
    )
    
    sbcli.run_loop()

if __name__ == "__main__":
    sbcli._log(BANNER)
    start()