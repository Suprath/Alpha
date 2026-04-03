from services.worker_service.core.bhavcopy_downloader import BhavcopyDownloader

def test_download():
    downloader = BhavcopyDownloader()
    # Using an older archived date to avoid "not yet uploaded" issues
    date_str = "2026-03-31"
    
    print(f"\n--- Testing CM Bhavcopy Download for {date_str} ---")
    cm_data = downloader.download_udiff_cm(date_str)
    if cm_data:
        print(f"SUCCESS: Downloaded CM Bhavcopy ({len(cm_data)} bytes)")
        print(f"Snippet:\n{cm_data[:200]}")
    else:
        print("FAILED: CM Bhavcopy download failed. (Check if date is a holiday or session initialization failed)")

    print(f"\n--- Testing Index data Download for {date_str} ---")
    index_data = downloader.download_indices(date_str)
    if index_data:
        print(f"SUCCESS: Downloaded Index data ({len(index_data)} bytes)")
        print(f"Snippet:\n{index_data[:200]}")
    else:
        print("FAILED: Index download failed")

if __name__ == "__main__":
    test_download()
