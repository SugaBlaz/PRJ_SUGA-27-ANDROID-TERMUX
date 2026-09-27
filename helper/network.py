import requests

def get_public_ip():
    try:
        # Query the ipify API for a plain text IP response
        response = requests.get('https://api.ipify.org')
        return response.text
    except requests.RequestException as e:
        return f"Error: {e}"