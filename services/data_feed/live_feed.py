"""
Upstox v3 Live Market Data Feed
─────────────────────────────────────────────────────────────────────────────
1. Calls /v3/feed/market-data-feed/authorize to get a one-time WebSocket URI
2. Connects via WSS, subscribes to configured instruments in "full" mode
3. Decodes incoming protobuf FeedResponse messages
4. Normalises each tick to a flat dict and publishes to Redis Stream alpha:ticks
5. Auto-reconnects with exponential back-off on any failure
"""
import asyncio
import json
import logging
import os
import ssl
import time
from datetime import datetime, timezone
from typing import Optional

import aiohttp
import redis.asyncio as aioredis
import websockets

# Generated from shared/proto/market_data_v3.proto at image build time
import market_data_v3_pb2 as pb  # noqa: E402  (lives in the image root)

logger = logging.getLogger(__name__)

AUTHORIZE_URL = "https://api.upstox.com/v3/feed/market-data-feed/authorize"
REDIS_STREAM   = "alpha:ticks"
REDIS_MAXLEN   = 50_000      # keep last 50 k entries; consumer can lag safely


# ── helpers ──────────────────────────────────────────────────────────────────

def _now_ns() -> int:
    return int(datetime.now(timezone.utc).timestamp() * 1e9)


# ── main class ───────────────────────────────────────────────────────────────

class UpstoxLiveFeed:
    def __init__(
        self,
        access_token: str,
        instrument_keys: list[str],
        token_map: dict[str, int],          # instrument_key → DB id (token)
    ) -> None:
        self.access_token    = access_token
        self.instrument_keys = instrument_keys
        self.token_map       = token_map
        self._redis: Optional[aioredis.Redis] = None

    # ── infrastructure ────────────────────────────────────────────────────────

    async def _connect_redis(self) -> None:
        host = os.getenv("REDIS_HOST", "redis")
        port = int(os.getenv("REDIS_PORT", "6379"))
        self._redis = aioredis.from_url(f"redis://{host}:{port}", decode_responses=True)
        await self._redis.ping()
        logger.info("[LiveFeed] Redis connected at %s:%d", host, port)

    async def _authorize(self) -> str:
        """Call Upstox REST to get an authorized one-time WebSocket URI."""
        headers = {
            "Authorization": f"Bearer {self.access_token}",
            "Api-Version":   "2.0",
            "Accept":        "application/json",
        }
        async with aiohttp.ClientSession() as session:
            async with session.get(AUTHORIZE_URL, headers=headers, timeout=aiohttp.ClientTimeout(total=10)) as resp:
                body = await resp.json()
                if resp.status != 200:
                    raise RuntimeError(f"Auth HTTP {resp.status}: {body}")
                uri = body["data"]["authorized_redirect_uri"]
                logger.info("[LiveFeed] Authorized → %s…", uri[:70])
                return uri

    # ── subscription ─────────────────────────────────────────────────────────

    async def _subscribe(self, ws) -> None:
        msg = json.dumps({
            "guid":   "alpha-live-feed",
            "method": "sub",
            "data": {
                "instrumentKeys": self.instrument_keys,
                "mode":           "full",
            },
        })
        await ws.send(msg)
        logger.info("[LiveFeed] Subscribed to %d instruments: %s",
                    len(self.instrument_keys), self.instrument_keys)

    # ── protobuf decode ───────────────────────────────────────────────────────

    def _decode(self, raw: bytes) -> list[dict]:
        """Decode a FeedResponse and return a list of tick dicts."""
        resp = pb.FeedResponse()
        resp.ParseFromString(raw)

        if resp.type == pb.market_info:
            for seg, status in resp.marketInfo.segmentStatus.items():
                if "NSE_EQ" in seg:
                    logger.info("[LiveFeed] Market status %s=%s",
                                seg, pb.MarketStatus.Name(status))
            return []

        ticks = []
        for key, feed in resp.feeds.items():
            token = self.token_map.get(key, 0)
            if token == 0:
                logger.warning("[LiveFeed] Unknown instrument key (not in token_map): %s", key)
                continue

            tick: dict = {
                "symbol": key,
                "token":  token,
                "ts_ns":  _now_ns(),
            }

            if feed.HasField("fullFeed"):
                ff = feed.fullFeed
                if ff.HasField("marketFF"):
                    mf = ff.marketFF
                    tick.update({
                        "price":     mf.ltpc.ltp,
                        "prev_close": mf.ltpc.cp,
                        "volume":    mf.vtt,
                        "oi":        mf.oi,
                        "atp":       mf.atp,
                        "tbq":       mf.tbq,
                        "tsq":       mf.tsq,
                        "iv":        mf.iv,
                    })
                    if mf.marketLevel.bidAskQuote:
                        q0 = mf.marketLevel.bidAskQuote[0]
                        tick.update({
                            "bid_price": q0.bidP,
                            "bid_size":  q0.bidQ,
                            "ask_price": q0.askP,
                            "ask_size":  q0.askQ,
                        })
                    # Depth levels 1-4
                    for lvl, q in enumerate(mf.marketLevel.bidAskQuote[:5], start=0):
                        tick[f"bid_p{lvl}"] = q.bidP
                        tick[f"bid_q{lvl}"] = q.bidQ
                        tick[f"ask_p{lvl}"] = q.askP
                        tick[f"ask_q{lvl}"] = q.askQ
                    if mf.HasField("optionGreeks"):
                        g = mf.optionGreeks
                        tick.update({"delta": g.delta, "gamma": g.gamma,
                                     "theta": g.theta, "vega":  g.vega, "rho": g.rho})
                elif ff.HasField("indexFF"):
                    idx = ff.indexFF
                    tick.update({
                        "price":      idx.ltpc.ltp,
                        "prev_close": idx.ltpc.cp,
                        "volume":     0,
                        "oi":         0.0,
                    })

            ticks.append(tick)
        return ticks

    # ── redis publish ─────────────────────────────────────────────────────────

    async def _publish(self, tick: dict) -> None:
        fields = {k: str(v) for k, v in tick.items()}
        await self._redis.xadd(REDIS_STREAM, fields, maxlen=REDIS_MAXLEN, approximate=True)

    # ── main loop ─────────────────────────────────────────────────────────────

    async def run(self) -> None:
        await self._connect_redis()

        ssl_ctx         = ssl.create_default_context()
        backoff         = 1
        ticks_published = 0
        msgs_received   = 0

        while True:
            try:
                uri = await self._authorize()
                async with websockets.connect(uri, ssl=ssl_ctx,
                                              ping_interval=20,
                                              ping_timeout=20) as ws:
                    backoff = 1
                    await self._subscribe(ws)
                    logger.info("[LiveFeed] Streaming…")

                    async for raw in ws:
                        msgs_received += 1
                        if msgs_received <= 5 or msgs_received % 100 == 0:
                            logger.info("[LiveFeed] msg#%d type=%s size=%d bytes",
                                        msgs_received,
                                        "bytes" if isinstance(raw, bytes) else "text",
                                        len(raw))
                        if isinstance(raw, bytes):
                            ticks = self._decode(raw)
                            for tick in ticks:
                                await self._publish(tick)
                                ticks_published += 1
                                if ticks_published % 500 == 0:
                                    logger.info("[LiveFeed] %d ticks published to Redis",
                                                ticks_published)
                        else:
                            logger.info("[LiveFeed] Text msg#%d: %s", msgs_received, raw[:120])

            except (websockets.ConnectionClosed,
                    aiohttp.ClientError,
                    ConnectionRefusedError) as exc:
                logger.warning("[LiveFeed] Connection error: %s — reconnecting in %ds", exc, backoff)
            except asyncio.CancelledError:
                logger.info("[LiveFeed] Cancelled — exiting")
                break
            except Exception as exc:
                logger.error("[LiveFeed] Unexpected error: %s", exc, exc_info=True)

            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 60)
