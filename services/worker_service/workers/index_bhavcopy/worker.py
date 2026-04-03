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


class IndexBhavcopyWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[index_bhavcopy] Starting...")
        
        target_date = (params or {}).get("date")
        if not target_date:
            target_date = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")
            
        conn = self._connect_with_retry()
        if not conn:
            return

        try:
            self._ensure_schema(conn)
            downloader = BhavcopyDownloader()
            csv_data = downloader.download_indices(target_date)
            
            if csv_data:
                self._process_csv(conn, csv_data, target_date)
            else:
                print(f"[index_bhavcopy] No data found for {target_date}")
        finally:
            conn.close()
        print(f"[index_bhavcopy] Finished for {target_date}")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[index_bhavcopy] DB retry... {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _ensure_schema(self, conn):
        with conn.cursor() as cur:
            cur.execute("""
                CREATE TABLE IF NOT EXISTS index_bhavcopy (
                    report_date DATE NOT NULL,
                    index_name VARCHAR(128) NOT NULL,
                    open DOUBLE PRECISION,
                    high DOUBLE PRECISION,
                    low DOUBLE PRECISION,
                    close DOUBLE PRECISION,
                    volume BIGINT,
                    turnover DOUBLE PRECISION,
                    pe_ratio DOUBLE PRECISION,
                    pb_ratio DOUBLE PRECISION,
                    div_yield DOUBLE PRECISION,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    PRIMARY KEY (report_date, index_name)
                );
            """)
        conn.commit()

    def _process_csv(self, conn, csv_content, report_date):
        f = io.StringIO(csv_content)
        # Index CSV has varying headers across vendors/reports
        # Common: Index Name, Open, High, Low, Close, Volume, Turnover (Rs. Cr.), P/E, P/B, Div Yield
        reader = csv.DictReader(f)
        
        records = []
        for row in reader:
            try:
                name = row.get("Index Name", row.get("index_name", ""))
                if not name: continue
                
                records.append((
                    report_date,
                    name,
                    float(row.get("Open", 0)),
                    float(row.get("High", 0)),
                    float(row.get("Low", 0)),
                    float(row.get("Close", 0)),
                    int(row.get("Volume", 0)),
                    float(row.get("Turnover", 0)),
                    float(row.get("P/E", 0)),
                    float(row.get("P/B", 0)),
                    float(row.get("Div Yield", 0))
                ))
            except Exception as e:
                continue

        if records:
            with conn.cursor() as cur:
                execute_values(cur, """
                    INSERT INTO index_bhavcopy 
                    (report_date, index_name, open, high, low, close, volume, turnover, pe_ratio, pb_ratio, div_yield)
                    VALUES %s
                    ON CONFLICT (report_date, index_name) DO UPDATE SET
                        open = EXCLUDED.open, high = EXCLUDED.high, low = EXCLUDED.low, close = EXCLUDED.close;
                """, records)
            conn.commit()
            print(f"[index_bhavcopy] Inserted/Updated {len(records)} index rows.")
