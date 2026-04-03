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


class FnoBhavcopyWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[fno_bhavcopy] Starting...")
        
        target_date = (params or {}).get("date")
        if not target_date:
            target_date = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")
            
        conn = self._connect_with_retry()
        if not conn:
            return

        try:
            self._ensure_schema(conn)
            downloader = BhavcopyDownloader()
            csv_data = downloader.download_udiff_fo(target_date)
            
            if csv_data:
                self._process_csv(conn, csv_data, target_date)
            else:
                print(f"[fno_bhavcopy] No data found for {target_date}")
        finally:
            conn.close()
        print(f"[fno_bhavcopy] Finished for {target_date}")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[fno_bhavcopy] DB retry... {e}")
                time.sleep(2)
                retries -= 1
        return None

    def _ensure_schema(self, conn):
        with conn.cursor() as cur:
            cur.execute("""
                CREATE TABLE IF NOT EXISTS fno_bhavcopy (
                    report_date DATE NOT NULL,
                    symbol VARCHAR(64) NOT NULL,
                    expiry_date DATE NOT NULL,
                    instrument_type VARCHAR(32),
                    strike_price DOUBLE PRECISION,
                    option_type VARCHAR(16),
                    open DOUBLE PRECISION,
                    high DOUBLE PRECISION,
                    low DOUBLE PRECISION,
                    close DOUBLE PRECISION,
                    settle_price DOUBLE PRECISION,
                    volume BIGINT,
                    turnover DOUBLE PRECISION,
                    oi BIGINT,
                    change_in_oi BIGINT,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    PRIMARY KEY (report_date, symbol, expiry_date, strike_price, option_type)
                );
            """)
        conn.commit()

    def _process_csv(self, conn, csv_content, report_date):
        f = io.StringIO(csv_content)
        reader = csv.DictReader(f)
        
        # UDiFF FO Headers: INSTRUMENT, SYMBOL, EXPIRY_DT, STRIKE_PR, OPTION_TYP, OPEN, HIGH, LOW, CLOSE, SETTLE_PR, CONTRACTS, VAL_INLAKH, OPEN_INT, CHG_IN_OI, TIMESTAMP
        records = []
        for row in reader:
            try:
                symbol = row.get("SYMBOL", row.get("Symbol", ""))
                if not symbol: continue
                
                # Format expiry_date (typically DD-MMM-YYYY or YYYY-MM-DD)
                expiry_raw = row.get("EXPIRY_DT", row.get("Expiry_Dt", ""))
                try:
                    expiry_dt = datetime.strptime(expiry_raw, "%d-%b-%Y").date()
                except:
                    expiry_dt = datetime.strptime(expiry_raw, "%Y-%m-%d").date()

                records.append((
                    report_date,
                    symbol,
                    expiry_dt,
                    row.get("INSTRUMENT", ""),
                    float(row.get("STRIKE_PR", 0)),
                    row.get("OPTION_TYP", ""),
                    float(row.get("OPEN", 0)),
                    float(row.get("HIGH", 0)),
                    float(row.get("LOW", 0)),
                    float(row.get("CLOSE", 0)),
                    float(row.get("SETTLE_PR", 0)),
                    int(row.get("CONTRACTS", 0)),
                    float(row.get("VAL_INLAKH", 0)),
                    int(row.get("OPEN_INT", 0)),
                    int(row.get("CHG_IN_OI", 0))
                ))
            except Exception as e:
                continue

        if records:
            with conn.cursor() as cur:
                execute_values(cur, """
                    INSERT INTO fno_bhavcopy 
                    (report_date, symbol, expiry_date, instrument_type, strike_price, option_type, open, high, low, close, settle_price, volume, turnover, oi, change_in_oi)
                    VALUES %s
                    ON CONFLICT (report_date, symbol, expiry_date, strike_price, option_type) DO UPDATE SET
                        open = EXCLUDED.open, high = EXCLUDED.high, low = EXCLUDED.low, close = EXCLUDED.close, 
                        oi = EXCLUDED.oi, change_in_oi = EXCLUDED.change_in_oi;
                """, records)
            conn.commit()
            print(f"[fno_bhavcopy] Inserted/Updated {len(records)} F&O rows.")
