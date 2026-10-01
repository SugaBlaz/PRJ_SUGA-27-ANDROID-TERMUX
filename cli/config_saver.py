import os
import json

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_FILE = os.path.join(BASE_DIR, "config.json")

def load_config():
    """Loads configuration settings from the JSON file if it exists."""
    # 1. Check if the file exists and is NOT empty
    if os.path.exists(CONFIG_FILE) and os.path.getsize(CONFIG_FILE) > 0:
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception as e:
            print(f"[-] Error loading config file: {e}", flush=True)
            return {}
            
    # 2. If it doesn't exist, create an empty file (or handle an existing empty file)
    else:
        if not os.path.exists(CONFIG_FILE):
            with open(CONFIG_FILE, "x"): pass
        return {}
        
def save(data):
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=4)

def save_config(data, append = False):
    """Saves CLI settings to the JSON file."""
    try:
        if not append:
            save(data)
        else:
            try: 
                with open(CONFIG_FILE, "x"): pass
            except FileExistsError: pass
                
            dataList: dict = load_config()
            
            if dataList:
                dataList.update(data)
                
                save(dataList)
            else:
                save(data)              
    except Exception as e:
        print(f"[-] Failed to save configuration: {e}", flush=True)