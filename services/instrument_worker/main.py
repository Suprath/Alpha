import os
import gzip
import json
import time
import requests
import psycopg2
from psycopg2.extras import execute_values
from datetime import datetime

DB_HOST = os.getenv("POSTGRES_HOST", "postgres-db")
DB_PORT = os.getenv("POSTGRES_PORT", "5432")
DB_NAME = os.getenv("POSTGRES_DB", "alpha_db")
DB_USER = os.getenv("POSTGRES_USER", "alpha_user")
DB_PASS = os.getenv("POSTGRES_PASSWORD", "alpha_password")
TEST_MODE = os.getenv("TEST_MODE", "0") == "1"

URL = "https://assets.upstox.com/market-quote/instruments/exchange/NSE.json.gz"

def ensure_db_schema(conn):
    with conn.cursor() as cur:
        cur.execute("""
            CREATE TABLE IF NOT EXISTS instrument_universe (
                id SERIAL PRIMARY KEY,
                date DATE NOT NULL,
                instrument_key VARCHAR(64) NOT NULL,
                trading_symbol VARCHAR(128),
                name VARCHAR(256),
                exchange VARCHAR(16),
                segment VARCHAR(32),
                lot_size INT,
                tick_size FLOAT,
                instrument_type VARCHAR(16),
                expiry_ms BIGINT,
                strike_price FLOAT,
                UNIQUE(date, instrument_key)
            );
        """)
    conn.commit()

def fetch_and_load(conn):
    print(f"Downloading NSE Instruments from {URL}...")
    headers = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}
    response = requests.get(URL, headers=headers, stream=True)
    response.raise_for_status()

    decompressed_data = gzip.decompress(response.content)
    
    # Wait, sometimes Upstox's JSON response is an Object wrapping a list, like {"data": [{}]}
    # Let's decode and parse
    raw_json = decompressed_data.decode('utf-8')
    # Sometimes there's a problem decoding pure huge JSON, but json.loads deals with strings.
    # In upstox, it's generally a direct array or objects separated by newlines.
    try:
        json_data = json.loads(raw_json)
    except json.JSONDecodeError:
        print("JSON parse failed. Falling back to newline parsing if applicable...")
        json_data = [json.loads(line) for line in raw_json.splitlines() if line.strip()]

    print(f"Parsed {len(json_data)} instruments. Structuring payload...")
    
    today = datetime.now().date()
    
    records = []
    # Test Mode Filters (Targeting NIFTY, RELIANCE, and one Bank Nifty to match criteria)
    target_keys = ["NSE_EQ|INE002A01018", "NSE_INDEX|Nifty 50", "NSE_INDEX|Nifty Bank"]
    
    for item in json_data:
        key = item.get("instrument_key", "")
        if not key:
            continue
            
        if TEST_MODE and key not in target_keys:
            continue
            
        symbol = item.get("trading_symbol")
        name = item.get("name", "")
        exchange = item.get("exchange", "")
        segment = item.get("segment", "")
        lot = item.get("lot_size", 1) or 1
        tick = item.get("tick_size", 0.0) or 0.0
        i_type = item.get("instrument_type", "")
        expiry = item.get("expiry", 0) or 0
        strike_str = item.get("strike_price", 0.0)
        try:
            strike = float(strike_str)
        except:
            strike = 0.0
            
        records.append((today, key, symbol, name, exchange, segment, int(lot), float(tick), i_type, int(expiry), strike))

        if TEST_MODE and len(records) >= 3:
            print(f"TEST_MODE ACTIVE: Matched the 3 test target keys.")
            break

    print(f"Inserting {len(records)} instruments into PostgreSQL...")
    insert_query = """
        INSERT INTO instrument_universe 
        (date, instrument_key, trading_symbol, name, exchange, segment, lot_size, tick_size, instrument_type, expiry_ms, strike_price)
        VALUES %s
        ON CONFLICT (date, instrument_key) DO NOTHING;
    """
    
    with conn.cursor() as cur:
        execute_values(cur, insert_query, records)
    conn.commit()
    
    print("Insertion complete.")

def main():
    print("=== Alpha Instrument Worker ===")
    
    conn = None
    retries = 15
    while retries > 0:
        try:
            conn = psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            break
        except Exception as e:
            print(f"Database not ready. Retrying in 2s... ({e})")
            time.sleep(2)
            retries -= 1
            
    if not conn:
        print("Failed to connect to database. Exiting.")
        return

    try:
        ensure_db_schema(conn)
        fetch_and_load(conn)
    finally:
        conn.close()
        
    print("Worker finished successfully.")

if __name__ == "__main__":
    main()
