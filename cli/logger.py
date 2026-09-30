# logger.py
import json
import os
import sys
import time
from blessed import Terminal

from cli.config_saver import save_config, load_config

term = Terminal()

chosen_color = "green"
color = term.green

VALID_COLORS = {
    "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
    "bright_black", "bright_red", "bright_green", "bright_yellow", 
    "bright_blue", "bright_magenta", "bright_cyan", "bright_white",
    "gray", "grey"
}

def log_load_config():
    """Loads configuration settings from the JSON file if it exists."""
    global chosen_color, color
    
    try:
        data = load_config()
        
        if data:
            loaded_theme = data.get("theme", "green")
            if loaded_theme in VALID_COLORS and hasattr(term, loaded_theme):
                chosen_color = loaded_theme
                color = getattr(term, chosen_color)
    except Exception as e:
        print(f"[-] Error loading config file: {e}", flush=True)
            
def log_save_config():
    data = {"theme": chosen_color}
    
    save_config(data, append=True)

def change_theme(theme: str):
    """Changes the CLI's theme to a given color and persists it to config.json."""
    global chosen_color, color
    theme = theme.lower()

    if theme in VALID_COLORS and hasattr(term, theme):
        chosen_color = theme
        color = getattr(term, chosen_color)
        
        log_save_config()
        
        log(f"[+] Theme successfully changed to '{theme}' and saved.")
    else:
        log("[-] The color selected does not exist or is unsupported.")

def typewriter_log(text: str, active_color, delay: float = 0.04, end: str = "\n"):
    """Applies color per character to prevent ANSI escape sequences from breaking across lines."""
    for char in text:
        sys.stdout.write(active_color(char))
        sys.stdout.flush()
        time.sleep(delay)
    sys.stdout.write(end)
    sys.stdout.flush()

def log(text="", delay=None, end="\n"):
    """Logs colored text with prompt-safe ANSI handling and immediate flushing."""
    global color

    try:
        if chosen_color in VALID_COLORS and hasattr(term, chosen_color):
            active_color = getattr(term, chosen_color)
        else:
            active_color = term.green

        if delay is not None:
            typewriter_log(str(text), active_color, delay, end=end)
        else:
            sys.stdout.write(f"{active_color(str(text))}{end}")
            sys.stdout.flush()
    except Exception:
        if delay is not None:
            for char in str(text):
                sys.stdout.write(char)
                sys.stdout.flush()
                time.sleep(delay)
            sys.stdout.write(end)
            sys.stdout.flush()
        else:
            sys.stdout.write(f"{text}{end}")
            sys.stdout.flush()

log_load_config()