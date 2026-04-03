import os
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

class HolidayCalendarWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[holiday_calendar] Starting...")
        
        conn = self._connect_with_retry()
        if not conn:
            return
            
        try:
            downloader = BhavcopyDownloader()
            url = f"{downloader.BASE_URL}/api/holiday-master?type=trading"
            response = downloader.client.get(url, timeout=15)
            
            if response.status_code == 200:
                self._process_json(conn, response.json())
            else:
                print(f"[holiday_calendar] Failed to fetch holiday data (Code: {response.status_code})")
        finally:
            conn.close()
        print("[holiday_calendar] Finished successfully.")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[holiday_calendar] DB connection error: {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _ensure_schema(self, conn):
        with conn.cursor() as cur:
            cur.execute("""
                CREATE TABLE IF NOT EXISTS holiday_calendar (
                    holiday_date DATE NOT NULL,
                    description TEXT,
                    exchange VARCHAR(16),
                    PRIMARY KEY (holiday_date, exchange)
                );
            """)
        conn.commit()

    def _process_json(self, conn, data):
        # The JSON format is a dict where keys are segments (CM, FO, etc.)
        # and values are lists of holiday objects.
        records = []
        for segment_name, holidays in data.items():
            for holiday in holidays:
                try:
                    # Date format: DD-Mon-YYYY (e.g., 26-Jan-2026)
                    h_date = datetime.strptime(holiday.get("tradingDate"), "%d-%b-%Y").date()
                    records.append((
                        h_date,
                        holiday.get("description", "Trading Holiday"),
                        segment_name
                    ))
                except Exception as e:
                    continue

        if records:
            # Re-ensure schema in case it was created with the old PK
            self._ensure_schema(conn)
            with conn.cursor() as cur:
                execute_values(cur, """
                    INSERT INTO holiday_calendar (holiday_date, description, exchange)
                    VALUES %s
                    ON CONFLICT (holiday_date, exchange) DO UPDATE SET
                        description = EXCLUDED.description;
                """, records)
            conn.commit()
            print(f"[holiday_calendar] Processed {len(records)} holidays.")
