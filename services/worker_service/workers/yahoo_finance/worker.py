import os
import time
import psycopg2
import yfinance as yf
from psycopg2.extras import execute_values
from datetime import datetime, timedelta
from typing import Optional, Any, List

from services.worker_service.core.base_worker import BaseWorker

DB_HOST = os.getenv("POSTGRES_HOST", "postgres-db")
DB_PORT = os.getenv("POSTGRES_PORT", "5432")
DB_NAME = os.getenv("POSTGRES_DB", "alpha_db")
DB_USER = os.getenv("POSTGRES_USER", "alpha_user")
DB_PASS = os.getenv("POSTGRES_PASSWORD", "alpha_password")

class YahooFinanceWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[yahoo_finance] Starting on-demand fetch...")
        params = params or {}
        symbols = params.get("symbols", [])
        fetch_all = params.get("all", False)
        
        conn = self._connect_with_retry()
        if not conn:
            return
            
        try:
            self._ensure_schema(conn)
            
            if fetch_all:
                symbols = self._get_all_symbols(conn)
            
            if not symbols:
                print("[yahoo_finance] No symbols to fetch. Use {'symbols': ['RELIANCE', 'TCS']} or {'all': true}")
                return

            print(f"[yahoo_finance] Fetching data for {len(symbols)} symbols...")
            for symbol in symbols:
                try:
                    self._process_symbol(conn, symbol)
                    # Small sleep to avoid rate limiting
                    time.sleep(0.5) 
                except Exception as e:
                    print(f"[yahoo_finance] Error processing {symbol}: {e}")
                    
        finally:
            conn.close()
        print("[yahoo_finance] Finished.")

    def _ensure_schema(self, conn):
        with conn.cursor() as cur:
            # 1. Ensure corporate_actions table exists
            cur.execute("""
                CREATE TABLE IF NOT EXISTS corporate_actions (
                    symbol VARCHAR(64) NOT NULL,
                    ex_date DATE NOT NULL,
                    action_type VARCHAR(32) NOT NULL,
                    details TEXT,
                    report_date DATE,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    PRIMARY KEY (symbol, ex_date, action_type)
                );
            """)
            # Fix for evolving from old schema if it exists
            cur.execute("ALTER TABLE corporate_actions ADD COLUMN IF NOT EXISTS details TEXT;")
            cur.execute("ALTER TABLE corporate_actions ADD COLUMN IF NOT EXISTS report_date DATE;")

            # 2. Evolve stock_master with new columns if they don't exist
            # Note: PostgreSQL 9.6+ doesn't have ADD COLUMN IF NOT EXISTS in a simple way 
            # so we use multiple ALTER TABLE steps or a PL/pgSQL block.
            new_columns = [
                ("long_name", "VARCHAR(255)"),
                ("sector", "VARCHAR(128)"),
                ("industry", "VARCHAR(128)"),
                ("market_cap", "BIGINT"),
                ("pe_ratio", "DOUBLE PRECISION"),
                ("beta", "DOUBLE PRECISION"),
                ("dividend_yield", "DOUBLE PRECISION"),
                ("updated_at", "TIMESTAMP DEFAULT CURRENT_TIMESTAMP")
            ]
            for col_name, col_type in new_columns:
                try:
                    cur.execute(f"ALTER TABLE stock_master ADD COLUMN IF NOT EXISTS {col_name} {col_type};")
                except Exception as e:
                    print(f"[yahoo_finance] Warning: Could not add column {col_name}: {e}")
                    conn.rollback()
                    continue
        conn.commit()

    def _get_all_symbols(self, conn) -> List[str]:
        with conn.cursor() as cur:
            cur.execute("SELECT symbol FROM stock_master")
            return [row[0] for row in cur.fetchall()]

    def _process_symbol(self, conn, symbol: str):
        # Yahoo Finance uses .NS for NSE
        yf_symbol = f"{symbol}.NS"
        ticker = yf.Ticker(yf_symbol)
        
        # 1. Update Master Data
        info = ticker.info
        if info:
            with conn.cursor() as cur:
                cur.execute("""
                    UPDATE stock_master SET
                        long_name = %s,
                        sector = %s,
                        industry = %s,
                        market_cap = %s,
                        pe_ratio = %s,
                        beta = %s,
                        dividend_yield = %s,
                        updated_at = CURRENT_TIMESTAMP
                    WHERE symbol = %s
                """, (
                    info.get("longName"),
                    info.get("sector"),
                    info.get("industry"),
                    info.get("marketCap"),
                    info.get("trailingPE"),
                    info.get("beta"),
                    info.get("dividendYield"),
                    symbol
                ))
            conn.commit()
            print(f"[yahoo_finance] Updated master data for {symbol}")

        # 2. Update Corporate Actions
        actions = ticker.actions
        if actions is not None and not actions.empty:
            records = []
            for date, row in actions.iterrows():
                ex_date = date.date()
                if row['Dividends'] > 0:
                    records.append((symbol, ex_date, 'DIVIDEND', f"Dividend: {row['Dividends']}", datetime.now().date()))
                if row['Stock Splits'] > 0:
                    records.append((symbol, ex_date, 'SPLIT', f"Split: {row['Stock Splits']}", datetime.now().date()))
            
            if records:
                with conn.cursor() as cur:
                    execute_values(cur, """
                        INSERT INTO corporate_actions (symbol, ex_date, action_type, details, report_date)
                        VALUES %s
                        ON CONFLICT (symbol, ex_date, action_type) DO UPDATE SET
                            details = EXCLUDED.details,
                            report_date = EXCLUDED.report_date;
                    """, records)
                conn.commit()
                print(f"[yahoo_finance] Updated {len(records)} corporate actions for {symbol}")

    def _connect_with_retry(self):
        retries = 5
        while retries > 0:
            try:
                return psycopg2.connect(host=DB_HOST, port=DB_PORT, dbname=DB_NAME, user=DB_USER, password=DB_PASS)
            except Exception as e:
                print(f"[yahoo_finance] DB connection error: {e}")
                time.sleep(2)
                retries -= 1
        return None
