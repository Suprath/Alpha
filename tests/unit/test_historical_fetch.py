import requests
import socket
import time
from datetime import datetime, timedelta

# Configuration
UPSTOX_ACCESS_TOKEN = "YOUR_ACCESS_TOKEN"  # User should replace this
INSTRUMENT_KEY = "NSE_EQ|INE002A01018"  # Reliance
INTERVAL = "1minute"
QUESTDB_HOST = "localhost"
QUESTDB_ILP_PORT = 9009

def fetch_upstox_historical(token, instrument, interval, days=1):
    to_date = datetime.now().strftime("%Y-%m-%d")
    from_date = (datetime.now() - timedelta(days=days)).strftime("%Y-%m-%d")
    
    url = f"https://api.upstox.com/v2/historical-candle/{instrument}/{interval}/{to_date}/{from_date}"
    headers = {
        'Accept': 'application/json',
        'Authorization': f'Bearer {token}'
    }
    
    print(f"Fetching from Upstox: {url}")
    response = requests.get(url, headers=headers)
    if response.status_code == 200:
        return response.json()
    else:
        print(f"Error fetching from Upstox: {response.status_code} {response.text}")
        return None

def send_to_questdb(candles, symbol):
    if not candles:
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((QUESTDB_HOST, QUESTDB_ILP_PORT))
        print(f"Connected to QuestDB at {QUESTDB_HOST}:{QUESTDB_ILP_PORT}")
        
        for c in candles:
            # Upstox format: [timestamp, open, high, low, close, volume, oi]
            # ts_str example: "2023-11-01T09:15:00+05:30"
            ts_str = c[0]
            # Convert to nanoseconds for QuestDB
            dt = datetime.strptime(ts_str[:19], "%Y-%m-%dT%H:%M:%S")
            # Upstox is IST (+5:30), adjust if needed or just use as is for local time
            ts_ns = int(dt.timestamp() * 1e9)
            
            # ILP Format: table_name,tag_key=tag_val field_key=field_val timestamp
            line = f"candles,symbol={symbol} open={c[1]},high={c[2]},low={c[3]},close={c[4]},volume={c[5]}i {ts_ns}\n"
            sock.sendall(line.encode())
            
        print(f"Sent {len(candles)} rows to QuestDB")
    except Exception as e:
        print(f"QuestDB Error: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    print("--- Upstox Historical Fetch Test ---")
    if UPSTOX_ACCESS_TOKEN == "YOUR_ACCESS_TOKEN":
        print("Please set your UPSTOX_ACCESS_TOKEN in the script.")
    else:
        data = fetch_upstox_historical(UPSTOX_ACCESS_TOKEN, INSTRUMENT_KEY, INTERVAL)
        if data and data['status'] == 'success':
            candles = data['data']['candles']
            send_to_questdb(candles, INSTRUMENT_KEY)
        else:
            print("Failed to fetch data.")
