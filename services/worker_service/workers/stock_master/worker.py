import os
import io
import csv
import time
import psycopg2
from psycopg2.extras import execute_values
from datetime import datetime
from typing import Optional, Any

from services.worker_service.core.base_worker import BaseWorker
from services.worker_service.core.bhavcopy_downloader import BhavcopyDownloader

DB_HOST = os.getenv("POSTGRES_HOST", "postgres-db")
DB_PORT = os.getenv("POSTGRES_PORT", "5432")
DB_NAME = os.getenv("POSTGRES_DB", "alpha_db")
DB_USER = os.getenv("POSTGRES_USER", "alpha_user")
DB_PASS = os.getenv("POSTGRES_PASSWORD", "alpha_password")

class StockMasterWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[stock_master] Starting...")
        conn = self._connect_with_retry()
        if not conn:
            return
            
        try:
            downloader = BhavcopyDownloader()
            # Fetch Securities available for trading (Commonly used as master)
            # URL: https://www.nseindia.com/market-data/securities-available-for-trading
            url = f"{downloader.BASE_URL}/api/market-data-filter-equity-sme?csv=true"
            response = downloader.session.get(url, timeout=15)
            
            if response.status_code == 200:
                self._process_csv(conn, response.text)
            else:
                print(f"[stock_master] Failed to fetch master data (Code: {response.status_code})")
        finally:
            conn.close()
        print("[stock_master] Finished successfully.")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[stock_master] DB connection error: {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _process_csv(self, conn, csv_content):
        f = io.StringIO(csv_content)
        reader = csv.DictReader(f)
        
        records = []
        for row in reader:
            try:
                symbol = row.get("SYMBOL", row.get("Symbol", ""))
                if not symbol: continue
                
                records.append((
                    symbol,
                    row.get("ISIN", row.get("ISIN NUMBER", "")),
                    row.get("NAME OF COMPANY", row.get("Security Name", "")),
                    row.get("SERIES", ""),
                    float(row.get("FACE VALUE", 0) or 0),
                    # Other fields like sector/industry often come from a different macro file, 
                    # but we'll store what we have for now.
                ))
            except Exception as e:
                continue

        if records:
            with conn.cursor() as cur:
                execute_values(cur, """
                    INSERT INTO stock_master (symbol, isin, name, series, face_value)
                    VALUES %s
                    ON CONFLICT (symbol) DO UPDATE SET
                        isin = EXCLUDED.isin,
                        name = EXCLUDED.name,
                        series = EXCLUDED.series,
                        face_value = EXCLUDED.face_value;
                """, records)
            conn.commit()
            print(f"[stock_master] Processed {len(records)} stocks.")
