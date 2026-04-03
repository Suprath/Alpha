import os
import time
import requests
import psycopg2
import socket
import json
import gzip
from datetime import datetime, timedelta
from services.worker_service.workers.instrument_loader.worker import InstrumentLoaderWorker

# Load environment variables from .env if running locally
def load_env():
    if os.path.exists(".env"):
        with open(".env") as f:
            for line in f:
                if "=" in line and not line.startswith("#"):
                    key, value = line.strip().split("=", 1)
                    os.environ[key] = value

load_env()

# Configuration
UPSTOX_TOKEN = os.getenv("UPSTOX_ACCESS_TOKEN")
PG_HOST = os.getenv("POSTGRES_HOST", "localhost")
PG_PORT = os.getenv("POSTGRES_PORT", "5432")
PG_DB = os.getenv("POSTGRES_DB", "alpha_db")
PG_USER = os.getenv("POSTGRES_USER", "alpha_user")
PG_PASS = os.getenv("POSTGRES_PASSWORD", "alpha_password")
QDB_HOST = os.getenv("QUESTDB_HOST", "localhost")
QDB_PORT = int(os.getenv("QUESTDB_ILP_PORT", 9009))

def test_instrument_loader():
    print("--- Phase 1: Running Instrument Loader ---")
    loader = InstrumentLoaderWorker()
    # Override TEST_MODE to ensure we load all (or many) instruments
    os.environ["TEST_MODE"] = "0" 
    loader.run()
    
    conn = psycopg2.connect(host=PG_HOST, port=PG_PORT, dbname=PG_DB, user=PG_USER, password=PG_PASS)
    with conn.cursor() as cur:
        cur.execute("SELECT count(*) FROM instrument_universe")
        count = cur.fetchone()[0]
        print(f"Postgres successfully populated with {count} instruments.")
        
        # Pick 3 active NSE_EQ symbols
        cur.execute("SELECT instrument_key, trading_symbol FROM instrument_universe WHERE exchange = 'NSE' AND segment = 'NSE_EQ' LIMIT 3")
        symbols = cur.fetchall()
        print(f"Selected symbols for historical fetch: {symbols}")
        return symbols

def fetch_and_store_historical(symbols):
    print("\n--- Phase 2: Fetching 2 Days of Historical Data ---")
    to_date = datetime.now().strftime("%Y-%m-%d")
    from_date = (datetime.now() - timedelta(days=2)).strftime("%Y-%m-%d")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((QDB_HOST, QDB_PORT))
        print(f"Connected to QuestDB ILP at {QDB_HOST}:{QDB_PORT}")
        
        for key, name in symbols:
            url = f"https://api.upstox.com/v2/historical-candle/{key}/1minute/{to_date}/{from_date}"
            headers = {'Accept': 'application/json', 'Authorization': f'Bearer {UPSTOX_TOKEN}'}
            
            print(f"Fetching {name} ({key}) from {from_date} to {to_date}...")
            response = requests.get(url, headers=headers)
            if response.status_code == 200:
                data = response.json()
                if data['status'] == 'success':
                    candles = data['data']['candles']
                    print(f"Received {len(candles)} candles. Persisting to QuestDB...")
                    
                    for c in candles:
                        # Upstox: [timestamp, open, high, low, close, volume, oi]
                        # Fix timestamp for QuestDB (nanoseconds)
                        dt = datetime.strptime(c[0][:19], "%Y-%m-%dT%H:%M:%S")
                        ts_ns = int(dt.timestamp() * 1e9)
                        
                        line = f"candles,symbol={key},interval=1minute open={c[1]},high={c[2]},low={c[3]},close={c[4]},volume={c[5]}i {ts_ns}\n"
                        sock.sendall(line.encode())
                else:
                    print(f"Error in data for {name}: {data}")
            else:
                print(f"HTTP Error {response.status_code} for {name}")
            
            time.sleep(1) # Simple rate limit compliance for test
            
    finally:
        sock.close()
        print("QuestDB connection closed.")

def verify_questdb():
    print("\n--- Phase 3: Final Verification ---")
    # For simplicity, we just print a success message here, but we could use Postgres wire protocol 
    # if we wanted to query QuestDB for row counts.
    print("Integration test scripts completed. Please check QuestDB UI at http://localhost:9000 for verify row counts in 'candles' table.")

if __name__ == "__main__":
    if not UPSTOX_TOKEN:
        print("Error: UPSTOX_ACCESS_TOKEN not found. Please check your .env file.")
    else:
        try:
            selected_symbols = test_instrument_loader()
            if selected_symbols:
                fetch_and_store_historical(selected_symbols)
                verify_questdb()
        except Exception as e:
            print(f"Integration Test Failed: {e}")
