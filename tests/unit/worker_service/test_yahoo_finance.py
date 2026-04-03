import os
import sys
import psycopg2
from services.worker_service.workers.yahoo_finance.worker import YahooFinanceWorker

def verify_data():
    worker = YahooFinanceWorker()
    conn = worker._connect_with_retry()
    if not conn:
        print("Failed to connect to database.")
        sys.exit(1)
        
    try:
        # 1. Ensure schema and initial test data
        worker._ensure_schema(conn)
        with conn.cursor() as cur:
            # Check if stock_master table exists, if not, wait for it or or create minimal
            cur.execute("SELECT to_regclass('stock_master');")
            if not cur.fetchone()[0]:
                print("stock_master table not found. Creating minimal for test...")
                cur.execute("""
                    CREATE TABLE IF NOT EXISTS stock_master (
                        symbol VARCHAR(64) PRIMARY KEY,
                        isin VARCHAR(32),
                        name VARCHAR(255),
                        long_name VARCHAR(255),
                        series VARCHAR(16),
                        face_value DOUBLE PRECISION,
                        sector VARCHAR(128),
                        industry VARCHAR(128),
                        market_cap BIGINT,
                        pe_ratio DOUBLE PRECISION,
                        beta DOUBLE PRECISION,
                        dividend_yield DOUBLE PRECISION,
                        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                    );
                """)
            
            cur.execute("INSERT INTO stock_master (symbol, name) VALUES ('RELIANCE', 'Reliance Industries') ON CONFLICT (symbol) DO NOTHING;")
        conn.commit()

        # 2. Run the processing for RELIANCE
        print("--- Processing RELIANCE ---")
        worker._process_symbol(conn, 'RELIANCE')

        # 3. Verify data in stock_master
        with conn.cursor() as cur:
            cur.execute("SELECT symbol, long_name, sector, industry, market_cap, pe_ratio FROM stock_master WHERE symbol='RELIANCE';")
            row = cur.fetchone()
            print("\n[V] stock_master verification:")
            print(f"  Symbol: {row[0]}")
            print(f"  Long Name: {row[1]}")
            print(f"  Sector: {row[2]}")
            print(f"  Industry: {row[3]}")
            print(f"  Market Cap: {row[4]}")
            print(f"  PE Ratio: {row[5]}")

        # 4. Verify data in corporate_actions
        with conn.cursor() as cur:
            cur.execute("SELECT ex_date, action_type, details FROM corporate_actions WHERE symbol='RELIANCE' ORDER BY ex_date DESC LIMIT 5;")
            rows = cur.fetchall()
            print("\n[V] corporate_actions verification (Latest 5):")
            for r in rows:
                print(f"  Date: {r[0]}, Type: {r[1]}, Details: {r[2]}")

    finally:
        conn.close()

if __name__ == "__main__":
    verify_data()
