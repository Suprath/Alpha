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

class DealsWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[deals_worker] Starting...")
        
        target_date = (params or {}).get("date")
        if not target_date:
            target_date = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")
            
        conn = self._connect_with_retry()
        if not conn:
            return
            
        try:
            downloader = BhavcopyDownloader()
            self._fetch_and_process(conn, downloader, "bulk", target_date)
            self._fetch_and_process(conn, downloader, "block", target_date)
        finally:
            conn.close()
        print(f"[deals_worker] Finished for {target_date}")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[deals_worker] DB connection error: {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _fetch_and_process(self, conn, downloader, mode, date_str):
        # mode: bulk or block
        table_name = f"{mode}_deals"
        print(f"[deals_worker] Fetching {mode} deals for {date_str}...")
        
        # URL Example: https://www.nseindia.com/api/live-analysis-large-deals?csv=true&mode=bulk
        url = f"{downloader.BASE_URL}/api/live-analysis-large-deals?csv=true&mode={mode}"
        response = downloader.session.get(url, timeout=15)
        
        if response.status_code != 200:
            print(f"[deals_worker] Failed to fetch {mode} data (Code: {response.status_code})")
            return
            
        f = io.StringIO(response.text)
        reader = csv.DictReader(f)
        records = []
        
        for row in reader:
            try:
                # Row format depends on NSE's API, but let's assume column names
                symbol = row.get("Symbol", row.get("SYMBOL", ""))
                if not symbol: continue
                
                records.append((
                    date_str,
                    symbol,
                    row.get("Client Name", row.get("CLIENT_NAME", "")),
                    row.get("Buy/Sell", row.get("DEAL_TYPE", "BUY")),
                    int(row.get("Quantity", row.get("QUANTITY", 0))),
                    float(row.get("Trade Price", row.get("PRICE", 0.0))),
                    row.get("Exchange", "NSE")
                ))
            except Exception as e:
                continue

        if records:
            with conn.cursor() as cur:
                execute_values(cur, f"""
                    INSERT INTO {table_name} (report_date, symbol, client_name, deal_type, quantity, price, exchange)
                    VALUES %s
                """, records)
            conn.commit()
            print(f"[deals_worker] Inserted {len(records)} rows into {table_name}.")
