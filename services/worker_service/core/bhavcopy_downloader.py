import httpx
import time
import os
import zipfile
import io
import asyncio
from typing import Optional, Dict
from datetime import datetime
from playwright.sync_api import sync_playwright


class BhavcopyDownloader:
    """
    Handles downloading Bhavcopy files from the NSE website.
    Uses Playwright for the initial Akamai session handshake and httpx for 
    high-performance, rate-limited downloads.
    """
    
    BASE_URL = "https://www.nseindia.com"
    API_URL = f"{BASE_URL}/api"
    
    def __init__(self):
        self.client = httpx.Client(
            headers={
                'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
                'Accept': '*/*',
                'Accept-Language': 'en-US,en;q=0.9',
                'Referer': self.BASE_URL
            },
            timeout=30.0,
            follow_redirects=True
        )
        self.last_request_time = 0
        self._initialize_session()

    def _initialize_session(self):
        """
        Uses Playwright to visit the NSE home page and solve anti-bot challenges.
        Extracts valid session cookies for use in httpx.
        """
        print("[bhavcopy_downloader] Initializing browser session proxy...")
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            context = browser.new_context(user_agent=self.client.headers['User-Agent'])
            page = context.new_page()
            
            try:
                # Step 1: Visit home page to trigger Akamai challenge
                page.goto(self.BASE_URL, wait_until="networkidle")
                
                # Step 2: Visit a market data page to ensure all session cookies are set
                page.goto(f"{self.BASE_URL}/market-data/live-equity-market", wait_until="networkidle")
                time.sleep(2)
                
                # Step 3: Extract cookies and inject into httpx client
                cookies = context.cookies()
                for cookie in cookies:
                    self.client.cookies.set(cookie['name'], cookie['value'], domain=cookie['domain'])
                
                print(f"[bhavcopy_downloader] Session initialized with {len(cookies)} cookies.")
            except Exception as e:
                print(f"[bhavcopy_downloader] Playwright session initialization failed: {e}")
            finally:
                browser.close()

    def _rate_limit(self):
        """Enforces a strict 1 request per 500ms limit."""
        elapsed = time.time() - self.last_request_time
        if elapsed < 0.5:
            time.sleep(0.5 - elapsed)
        self.last_request_time = time.time()

    def get_json(self, url: str) -> Optional[Dict]:
        """Fetch JSON data from NSE API with rate limiting and session reuse."""
        self._rate_limit()
        try:
            response = self.client.get(url)
            if response.status_code == 200:
                return response.json()
            else:
                print(f"[bhavcopy_downloader] API request failed ({response.status_code}): {url}")
                return None
        except Exception as e:
            print(f"[bhavcopy_downloader] Exception during API request: {e}")
            return None

    def download_udiff_cm(self, date_str: str) -> Optional[str]:
        date_formatted = date_str.replace("-", "")
        file_name = f"BhavCopy_NSE_CM_0_0_0_{date_formatted}_F_0000.csv.zip"
        url = f"{self.BASE_URL}/all-reports-equity-bhavcopy-udiff-zip?filename={file_name}&type=archives"
        return self._download_and_extract(url, file_name.replace(".zip", ""))

    def download_udiff_fo(self, date_str: str) -> Optional[str]:
        date_formatted = date_str.replace("-", "")
        file_name = f"BhavCopy_NSE_FO_0_0_0_{date_formatted}_F_0000.csv.zip"
        url = f"{self.BASE_URL}/all-reports-derivatives-bhavcopy-udiff-zip?filename={file_name}&type=archives"
        return self._download_and_extract(url, file_name.replace(".zip", ""))

    def download_indices(self, date_str: str) -> Optional[str]:
        date_formatted = datetime.strptime(date_str, "%Y-%m-%d").strftime("%d%m%y")
        file_name = f"ind_close_all_{date_formatted}.csv"
        url = f"{self.BASE_URL}/archives/indices/index_reports/{file_name}"
        
        self._rate_limit()
        response = self.client.get(url)
        if response.status_code == 200:
            return response.text
        return None

    def _download_and_extract(self, url: str, target_csv: str) -> Optional[str]:
        self._rate_limit()
        try:
            print(f"[bhavcopy_downloader] Downloading from: {url}")
            response = self.client.get(url)
            if response.status_code != 200:
                print(f"[bhavcopy_downloader] Download failed (Code: {response.status_code})")
                return None
            
            with zipfile.ZipFile(io.BytesIO(response.content)) as z:
                if target_csv in z.namelist():
                    with z.open(target_csv) as f:
                        return f.read().decode('utf-8')
                else:
                    csv_files = [n for n in z.namelist() if n.endswith('.csv')]
                    if csv_files:
                        with z.open(csv_files[0]) as f:
                            return f.read().decode('utf-8')
            return None
        except Exception as e:
            print(f"[bhavcopy_downloader] Error processing ZIP: {e}")
            return None
