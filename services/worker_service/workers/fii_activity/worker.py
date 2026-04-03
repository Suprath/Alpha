import os
import time
import requests
from datetime import datetime, timedelta
from typing import Optional, Any

from services.worker_service.core.base_worker import BaseWorker
from services.worker_service.core.bhavcopy_downloader import BhavcopyDownloader
from services.worker_service.core.questdb_writer import QuestDBWriter

class FiiActivityWorker(BaseWorker):
    def run(self, params: Optional[dict[str, Any]] = None) -> None:
        print("[fii_activity] Starting...")
        
        target_date = (params or {}).get("date")
        if not target_date:
            target_date = (datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d")
        
        # We need nanosecond timestamp for QuestDB
        ts_ns = int(datetime.strptime(target_date, "%Y-%m-%d").timestamp() * 1e9)
            
        try:
            downloader = BhavcopyDownloader()
            with QuestDBWriter() as qdb:
                self._fetch_cash_activity(downloader, qdb, target_date, ts_ns)
                self._fetch_derivative_activity(downloader, qdb, target_date, ts_ns)
        except Exception as e:
            print(f"[fii_activity] Error: {e}")
            
        print(f"[fii_activity] Finished for {target_date}")

    def _fetch_cash_activity(self, downloader, qdb, date_str, ts_ns):
        print(f"[fii_activity] Fetching FII/DII cash activity...")
        # URL: https://www.nseindia.com/api/fii_dii
        url = f"{downloader.BASE_URL}/api/fii_dii"
        response = downloader.session.get(url, timeout=15)
        
        if response.status_code != 200:
            print(f"[fii_activity] Failed to fetch FII/DII Cash data (Code: {response.status_code})")
            return
            
        data = response.json()
        for item in data:
            # { "category": "FII", "buyValue": 100, "sellValue": 80, "netValue": 20 }
            cat = item.get("category", "")
            if not cat: continue
            
            qdb.write_line(
                table="fii_dii_activity",
                symbols={"category": cat},
                fields={
                    "buy_value": float(item.get("buyValue", 0)),
                    "sell_value": float(item.get("sellValue", 0)),
                    "net_value": float(item.get("netValue", 0))
                },
                timestamp_ns=ts_ns
            )

    def _fetch_derivative_activity(self, downloader, qdb, date_str, ts_ns):
        print(f"[fii_activity] Fetching FII derivatives activity...")
        # URL: https://www.nseindia.com/api/fii-derivatives
        url = f"{downloader.BASE_URL}/api/fii-derivatives"
        response = downloader.session.get(url, timeout=15)
        
        if response.status_code != 200:
            print(f"[fii_activity] Failed to fetch FII Derivatives data (Code: {response.status_code})")
            return
            
        data = response.json()
        # The schema for FII derivatives is usually a complex object
        # We'll map it to our standardized fii_derivatives table
        # Example metrics: Index Futures Long/Short, Stock Futures Long/Short
        
        metrics = {}
        for item in data.get("data", []):
            label = item.get("label", "").lower().replace(" ", "_")
            if "index_futures" in label:
                metrics["index_futures_long"] = int(item.get("buy", 0))
                metrics["index_futures_short"] = int(item.get("sell", 0))
            elif "stock_futures" in label:
                metrics["stock_futures_long"] = int(item.get("buy", 0))
                metrics["stock_futures_short"] = int(item.get("sell", 0))
            elif "index_options" in label:
                metrics["index_options_call_long"] = int(item.get("buy", 0))
                metrics["index_options_put_long"] = int(item.get("sell", 0))

        if metrics:
            qdb.write_line(
                table="fii_derivatives",
                symbols={},
                fields=metrics,
                timestamp_ns=ts_ns
            )
