import os
import io
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

class IndexConstituentsWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[index_constituents] Starting...")
        conn = self._connect_with_retry()
        if not conn:
            return
            
        try:
            downloader = BhavcopyDownloader()
            indices = ["NIFTY 50", "NIFTY BANK", "NIFTY IT", "NIFTY FMCG", "NIFTY PHARMA"]
            for idx in indices:
                self._fetch_index_data(conn, downloader, idx)
        finally:
            conn.close()
        print("[index_constituents] Finished successfully.")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[index_constituents] DB connection error: {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _fetch_index_data(self, conn, downloader, index_name):
        print(f"[index_constituents] Fetching data for {index_name}...")
        # URL Example: https://www.nseindia.com/api/equity-stockIndices?index=NIFTY%2050
        url = f"{downloader.BASE_URL}/api/equity-stockIndices?index={index_name.replace(' ', '%20')}"
        response = downloader.session.get(url, timeout=15)
        
        if response.status_code != 200:
            print(f"[index_constituents] Failed to fetch {index_name} data (Code: {response.status_code})")
            return
            
        data = response.json()
        records = []
        today = datetime.now().date()
        
        for item in data.get("data", []):
            symbol = item.get("symbol")
            if not symbol or symbol == "-": continue
            
            records.append((
                index_name,
                symbol,
                today, # entry_date
                float(item.get("weightage", 0) or 0)
            ))

        if records:
            with conn.cursor() as cur:
                # Mark old constituents as inactive for this index
                cur.execute("UPDATE index_constituents SET is_active = FALSE WHERE index_name = %s", (index_name,))
                
                # Insert new ones
                execute_values(cur, """
                    INSERT INTO index_constituents (index_name, symbol, entry_date, weightage, is_active)
                    VALUES %s
                    ON CONFLICT (index_name, symbol, entry_date) DO UPDATE SET
                        is_active = TRUE,
                        weightage = EXCLUDED.weightage;
                """, records)
            conn.commit()
            print(f"[index_constituents] Updated {len(records)} stocks for {index_name}.")
