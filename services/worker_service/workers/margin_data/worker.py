import os
import io
import csv
import time
import psycopg2
from psycopg2.extras import execute_values
from datetime import datetime, timedelta
from typing import Optional, Any

from services.worker_service.core.base_worker import BaseWorker
from services.worker_service.core.bhavcopy_downloader import BhavcopyDownloader

DB_HOST = os.getenv("POSTGRES_HOST", "postgres-db")
DB_PORT = os.getenv("POSTGRES_PORT", "5432")
DB_NAME = os.getenv("POSTGRES_DB", "alpha_db")
DB_USER = os.getenv("POSTGRES_USER", "alpha_user")
DB_PASS = os.getenv("POSTGRES_PASSWORD", "alpha_password")

class MarginVaRWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[margin_data] Starting...")
        
        target_date = (params or {}).get("date")
        if not target_date:
            target_date = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")
            
        conn = self._connect_with_retry()
        if not conn:
            return
            
        try:
            downloader = BhavcopyDownloader()
            self._fetch_and_process(conn, downloader, target_date)
        finally:
            conn.close()
        print(f"[margin_data] Finished for {target_date}")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[margin_data] DB connection error: {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _fetch_and_process(self, conn, downloader, date_str):
        print(f"[margin_data] Fetching margin data for {date_str}...")
        # URL Example: https://www.nseindia.com/api/margin-daily-data?csv=true
        url = f"{downloader.BASE_URL}/api/margin-daily-data?csv=true"
        response = downloader.session.get(url, timeout=15)
        
        if response.status_code != 200:
            print(f"[margin_data] Failed to fetch margin data (Code: {response.status_code})")
            return
            
        f = io.StringIO(response.text)
        # Skip the first row if it's the header "Daily Margin Data"
        reader = csv.DictReader(f)
        records = []
        
        for row in reader:
            try:
                symbol = row.get("Symbol", row.get("SYMBOL", ""))
                if not symbol: continue
                
                records.append((
                    date_str,
                    symbol,
                    float(row.get("VAR Margin", row.get("VAR", 0))),
                    float(row.get("Extreme Loss Margin", row.get("ELM", 0))),
                    float(row.get("Applicable Margin", row.get("MTM", 0)))
                ))
            except Exception as e:
                continue

        if records:
            with conn.cursor() as cur:
                execute_values(cur, """
                    INSERT INTO margin_data (report_date, symbol, var_margin, extreme_loss_margin, applicable_margin)
                    VALUES %s
                    ON CONFLICT (report_date, symbol) DO NOTHING;
                """, records)
            conn.commit()
            print(f"[margin_data] Inserted {len(records)} rows.")
