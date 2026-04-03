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

class CorporateActionsWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[corporate_actions] Starting...")
        
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
        print(f"[corporate_actions] Finished for {target_date}")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[corporate_actions] DB connection error: {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _fetch_and_process(self, conn, downloader, date_str):
        print(f"[corporate_actions] Fetching corporate actions for {date_str}...")
        # URL Example: https://www.nseindia.com/api/corporate-announcements?csv=true
        # Wait, Corporate Actions CSV endpoint for all listed companies
        url = f"{downloader.BASE_URL}/api/corporates-info?index=equities&csv=true"
        response = downloader.session.get(url, timeout=15)
        
        if response.status_code != 200:
            print(f"[corporate_actions] Failed to fetch data (Code: {response.status_code})")
            return
            
        f = io.StringIO(response.text)
        reader = csv.DictReader(f)
        records = []
        
        for row in reader:
            try:
                symbol = row.get("Symbol", row.get("SYMBOL", ""))
                if not symbol: continue
                
                ex_date_raw = row.get("Ex Date", row.get("EX_DATE", ""))
                if not ex_date_raw: continue
                try:
                    ex_date = datetime.strptime(ex_date_raw, "%d-%b-%Y").date()
                except:
                    ex_date = datetime.strptime(ex_date_raw, "%Y-%m-%d").date()

                records.append((
                    symbol,
                    ex_date,
                    row.get("Action Type", row.get("PURPOSE", "DIVIDEND")),
                    row.get("Details", row.get("DESC", "")),
                    date_str # report_date
                ))
            except Exception as e:
                continue

        if records:
            with conn.cursor() as cur:
                execute_values(cur, """
                    INSERT INTO corporate_actions (symbol, ex_date, action_type, split_ratio, report_date)
                    VALUES %s
                    ON CONFLICT (symbol, ex_date, action_type) DO NOTHING;
                """, records)
            conn.commit()
            print(f"[corporate_actions] Inserted {len(records)} rows.")
