import os
import gzip
import json
import time
import requests
import psycopg2
from psycopg2.extras import execute_values
from datetime import datetime

from services.worker_service.core.base_worker import BaseWorker

DB_HOST = os.getenv("POSTGRES_HOST", "postgres-db")
DB_PORT = os.getenv("POSTGRES_PORT", "5432")
DB_NAME = os.getenv("POSTGRES_DB", "alpha_db")
DB_USER = os.getenv("POSTGRES_USER", "alpha_user")
DB_PASS = os.getenv("POSTGRES_PASSWORD", "alpha_password")
TEST_MODE = os.getenv("TEST_MODE", "0") == "1"

URL = "https://assets.upstox.com/market-quote/instruments/exchange/NSE.json.gz"


class InstrumentLoaderWorker(BaseWorker):
    def run(self) -> None:
        print("[instrument_loader] Starting...")
        conn = self._connect_with_retry()
        if not conn:
            print("[instrument_loader] Failed to connect to database. Aborting.")
            return
        try:
            self._ensure_schema(conn)
            self._fetch_and_load(conn)
        finally:
            conn.close()
        print("[instrument_loader] Finished successfully.")

    def _connect_with_retry(self):
        retries = 15
        while retries > 0:
            try:
                return psycopg2.connect(
                    host=DB_HOST, port=DB_PORT, dbname=DB_NAME,
                    user=DB_USER, password=DB_PASS
                )
            except Exception as e:
                print(f"[instrument_loader] DB not ready, retrying... ({e})")
                time.sleep(2)
                retries -= 1
        return None

    def _ensure_schema(self, conn):
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

    def _fetch_and_load(self, conn):
        print(f"[instrument_loader] Downloading NSE instruments from {URL}...")
        headers = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}
        response = requests.get(URL, headers=headers, stream=True)
        response.raise_for_status()

        raw_json = gzip.decompress(response.content).decode('utf-8')
        try:
            json_data = json.loads(raw_json)
        except json.JSONDecodeError:
            json_data = [json.loads(line) for line in raw_json.splitlines() if line.strip()]

        print(f"[instrument_loader] Parsed {len(json_data)} instruments.")

        today = datetime.now().date()
        target_keys = ["NSE_EQ|INE002A01018", "NSE_INDEX|Nifty 50", "NSE_INDEX|Nifty Bank"]
        records = []

        for item in json_data:
            key = item.get("instrument_key", "")
            if not key:
                continue
            if TEST_MODE and key not in target_keys:
                continue

            try:
                strike = float(item.get("strike_price", 0.0) or 0.0)
            except (ValueError, TypeError):
                strike = 0.0

            records.append((
                today,
                key,
                item.get("trading_symbol"),
                item.get("name", ""),
                item.get("exchange", ""),
                item.get("segment", ""),
                int(item.get("lot_size", 1) or 1),
                float(item.get("tick_size", 0.0) or 0.0),
                item.get("instrument_type", ""),
                int(item.get("expiry", 0) or 0),
                strike,
            ))

            if TEST_MODE and len(records) >= 3:
                print("[instrument_loader] TEST_MODE: matched 3 target instruments.")
                break

        print(f"[instrument_loader] Inserting {len(records)} records into PostgreSQL...")
        with conn.cursor() as cur:
            execute_values(cur, """
                INSERT INTO instrument_universe
                (date, instrument_key, trading_symbol, name, exchange, segment,
                 lot_size, tick_size, instrument_type, expiry_ms, strike_price)
                VALUES %s
                ON CONFLICT (date, instrument_key) DO NOTHING;
            """, records)
        conn.commit()
        print("[instrument_loader] Insertion complete.")
