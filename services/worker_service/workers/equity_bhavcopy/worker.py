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


class EquityBhavcopyWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[equity_bhavcopy] Starting...")
        
        # Determine target date
        target_date = (params or {}).get("date")
        if not target_date:
            # Default to yesterday or last working day
            target_date = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")
            
        conn = self._connect_with_retry()
        if not conn:
            return

        try:
            self._ensure_schema(conn)
            downloader = BhavcopyDownloader()
            csv_data = downloader.download_udiff_cm(target_date)
            
            if csv_data:
                self._process_csv(conn, csv_data, target_date)
            else:
                print(f"[equity_bhavcopy] No data found for {target_date}")
        finally:
            conn.close()
        print(f"[equity_bhavcopy] Finished for {target_date}")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[equity_bhavcopy] DB retry... {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _ensure_schema(self, conn):
        with conn.cursor() as cur:
            cur.execute("""
                CREATE TABLE IF NOT EXISTS equity_bhavcopy (
                    report_date DATE NOT NULL,
                    symbol VARCHAR(64) NOT NULL,
                    isin VARCHAR(32),
                    series VARCHAR(16),
                    open DOUBLE PRECISION,
                    high DOUBLE PRECISION,
                    low DOUBLE PRECISION,
                    close DOUBLE PRECISION,
                    prev_close DOUBLE PRECISION,
                    volume BIGINT,
                    turnover DOUBLE PRECISION,
                    trades BIGINT,
                    delivery_volume BIGINT,
                    delivery_percentage DOUBLE PRECISION,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    PRIMARY KEY (report_date, symbol)
                );
            """)
        conn.commit()

    def _process_csv(self, conn, csv_content, report_date):
        f = io.StringIO(csv_content)
        reader = csv.DictReader(f)
        
        # Normalize headers (remove whitespace/lowercase)
        # UDiFF headers: SYMBOL, SERIES, OPEN, HIGH, LOW, CLOSE, PREVCLOSE, TOTTRDQTY, TOTTRDVAL, TIMESTAMP, TOTALTRADES, ISIN, DELIV_QTY, DELIV_PER
        records = []
        for row in reader:
            try:
                # Basic mapping (adjust keys based on exact UDiFF format if needed)
                symbol = row.get("SYMBOL", row.get("Symbol", ""))
                if not symbol: continue
                
                records.append((
                    report_date,
                    symbol,
                    row.get("ISIN", ""),
                    row.get("SERIES", ""),
                    float(row.get("OPEN", 0)),
                    float(row.get("HIGH", 0)),
                    float(row.get("LOW", 0)),
                    float(row.get("CLOSE", 0)),
                    float(row.get("PREVCLOSE", 0)),
                    int(row.get("TOTTRDQTY", 0)),
                    float(row.get("TOTTRDVAL", 0)),
                    int(row.get("TOTALTRADES", 0)),
                    int(row.get("DELIV_QTY", 0)),
                    float(row.get("DELIV_PER", 0))
                ))
            except Exception as e:
                continue

        if records:
            with conn.cursor() as cur:
                execute_values(cur, """
                    INSERT INTO equity_bhavcopy 
                    (report_date, symbol, isin, series, open, high, low, close, prev_close, volume, turnover, trades, delivery_volume, delivery_percentage)
                    VALUES %s
                    ON CONFLICT (report_date, symbol) DO UPDATE SET
                        open = EXCLUDED.open, high = EXCLUDED.high, low = EXCLUDED.low, close = EXCLUDED.close,
                        volume = EXCLUDED.volume, delivery_volume = EXCLUDED.delivery_volume;
                """, records)
            conn.commit()
            print(f"[equity_bhavcopy] Inserted/Updated {len(records)} rows.")
