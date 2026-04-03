import socket
import time
import os
from typing import Dict, Any, Optional


class QuestDBWriter:
    """
    Lightweight Python client to write data to QuestDB using InfluxDB Line Protocol (ILP) over TCP.
    Optimized for institutional and EOD market data ingestion.
    """
    
    def __init__(self, host: str = None, port: int = None):
        self.host = host or os.getenv("QUESTDB_HOST", "questdb")
        self.port = int(port or os.getenv("QUESTDB_ILP_PORT", "9009"))
        self.sock: Optional[socket.socket] = None

    def connect(self) -> bool:
        """Establish a TCP connection to QuestDB's ILP port."""
        if self.sock:
            return True
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(5.0)
            self.sock.connect((self.host, self.port))
            return True
        except Exception as e:
            print(f"[questdb_writer] Connection failed to {self.host}:{self.port} - {e}")
            self.sock = None
            return False

    def close(self):
        """Close the socket connection."""
        if self.sock:
            try:
                self.sock.close()
            except:
                pass
            self.sock = None

    def write_line(self, table: str, symbols: Dict[str, str], fields: Dict[str, Any], timestamp_ns: int = None) -> bool:
        """
        Send a single line in ILP format.
        format: table,symbol1=val1,symbol2=val2 field1=val1,field2=val2 timestamp_ns
        """
        if not self.connect():
            return False
            
        # Construct Symbols string (Tags)
        sym_str = ""
        for k, v in symbols.items():
            # Escape spaces/commas/equals in symbol values if needed
            val = str(v).replace(" ", "\\ ").replace(",", "\\,").replace("=", "\\=")
            sym_str += f",{k}={val}"
            
        # Construct Fields string
        field_str = ""
        for k, v in fields.items():
            if field_str:
                field_str += ","
            
            if isinstance(v, float):
                field_str += f"{k}={v}"
            elif isinstance(v, int):
                # QuestDB ILP expects 'i' suffix for integers
                field_str += f"{k}={v}i"
            elif isinstance(v, bool):
                field_str += f"{k}={'t' if v else 'f'}"
            else:
                # String fields need to be quoted
                val = str(v).replace('"', '\\"')
                field_str += f'{k}="{val}"'
                
        # Timestamp
        ts = timestamp_ns if timestamp_ns else int(time.time() * 1e9)
        
        line = f"{table}{sym_str} {field_str} {ts}\n"
        
        try:
            self.sock.sendall(line.encode('utf-8'))
            return True
        except Exception as e:
            print(f"[questdb_writer] Send failed: {e}")
            self.close()
            return False

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
