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

# Generated from shared/proto/ at image build time (see services/data_feed/Dockerfile)
import market_data_v3_pb2 as pb    # noqa: E402  Upstox wire format
import alpha_tick_pb2 as atpb      # noqa: E402  Alpha internal tick schema

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
        # decode_responses=False: stream values are binary (serialized proto)
        self._redis = aioredis.from_url(f"redis://{host}:{port}", decode_responses=False)
        await self._redis.ping()
        logger.info("[LiveFeed] Redis connected at %s:%d", host, port)

    async def _authorize(self) -> str:
        """Call Upstox REST to get an authorized one-time WebSocket URI."""
        headers = {
            "Authorization": f"Bearer {self.access_token}",
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
        import uuid
        msg = json.dumps({
            "guid":   str(uuid.uuid4()),
            "method": "sub",
            "data": {
                "instrumentKeys": self.instrument_keys,
                "mode":           "full",
            },
        })
        # CRITICAL: Upstox V3 requires binary format for requests
        await ws.send(msg.encode('utf-8'))
        logger.info("[LiveFeed] Sent sub (binary): %s", msg)
        logger.info("[LiveFeed] Subscribed to %d instruments (mode=full): %s",
                    len(self.instrument_keys), self.instrument_keys)

    # ── protobuf decode ───────────────────────────────────────────────────────

    def _decode(self, raw: bytes) -> list[atpb.Tick]:
        """Decode an Upstox FeedResponse and return a list of alpha_tick Tick protos."""
        resp = pb.FeedResponse()
        try:
            resp.ParseFromString(raw)
        except Exception as e:
            logger.error("[LiveFeed] Failed to parse protobuf: %s", e)
            return []

        # Debug: log the type if it's not a standard feed
        if resp.type != 0:
            logger.info("[LiveFeed] msg type=%d", resp.type)

        if resp.type == pb.market_info:
            for seg, status in resp.marketInfo.segmentStatus.items():
                if "NSE_EQ" in seg:
                    logger.info("[LiveFeed] Market status %s=%s",
                                seg, pb.MarketStatus.Name(status))
            return []

        if not resp.feeds:
            # Log periodic empty feed messages if they happen
            return []

        ticks = []
        for key, feed in resp.feeds.items():
            token = self.token_map.get(key, 0)
            if token == 0:
                logger.warning("[LiveFeed] Unknown instrument key (not in token_map): %s", key)
                continue

            tick = atpb.Tick(
                token=token,
                instrument_key=key,
                timestamp_ns=_now_ns(),
            )

            if feed.HasField("fullFeed"):
                ff = feed.fullFeed
                if ff.HasField("marketFF"):
                    mf = ff.marketFF
                    tick.last_price    = mf.ltpc.ltp
                    tick.prev_close    = mf.ltpc.cp
                    tick.volume        = int(mf.vtt)
                    tick.open_interest = mf.oi
                    tick.atp           = mf.atp
                    tick.tbq           = mf.tbq
                    tick.tsq           = mf.tsq

                    for q in mf.marketLevel.bidAskQuote[:5]:
                        tick.bids.add(price=q.bidP, quantity=int(q.bidQ))
                        tick.asks.add(price=q.askP, quantity=int(q.askQ))

                    if tick.bids:
                        tick.bid_price = tick.bids[0].price
                        tick.bid_size  = tick.bids[0].quantity
                    if tick.asks:
                        tick.ask_price = tick.asks[0].price
                        tick.ask_size  = tick.asks[0].quantity

                    if mf.HasField("optionGreeks"):
                        g = mf.optionGreeks
                        tick.greeks.delta = g.delta
                        tick.greeks.gamma = g.gamma
                        tick.greeks.theta = g.theta
                        tick.greeks.vega  = g.vega
                        tick.greeks.rho   = g.rho
                    tick.greeks.iv = mf.iv   # always set; 0.0 for non-options

                elif ff.HasField("indexFF"):
                    idx = ff.indexFF
                    tick.last_price = idx.ltpc.ltp
                    tick.prev_close = idx.ltpc.cp

            ticks.append(tick)
        return ticks

    # ── redis publish ─────────────────────────────────────────────────────────

    async def _publish(self, tick: atpb.Tick) -> None:
        """Serialize tick as protobuf binary and publish to the Redis stream."""
        await self._redis.xadd(
            REDIS_STREAM,
            {b"data": tick.SerializeToString()},
            maxlen=REDIS_MAXLEN,
            approximate=True,
        )

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
                                              compression=None,
                                              ping_interval=20,
                                              ping_timeout=20) as ws:
                    backoff = 1
                    subscribed = False
                    logger.info("[LiveFeed] Streaming (no-comp)…")


                    last_heartbeat = time.time()

                    while True:
                        try:
                            # Use wait_for to implement a heartbeat/status check
                            raw = await asyncio.wait_for(ws.recv(), timeout=5.0)
                            msgs_received += 1
                            
                            if msgs_received <= 100 or msgs_received % 1000 == 0:
                                logger.info("[LiveFeed] msg#%d type=%s size=%d bytes",
                                            msgs_received,
                                            "bytes" if isinstance(raw, bytes) else "text",
                                            len(raw))

                            
                            if isinstance(raw, bytes):
                                ticks = self._decode(raw)
                                # Subscribe after receiving the initial market_info handshake (msg type 2)
                                if not subscribed:
                                    await self._subscribe(ws)
                                    subscribed = True
                                for tick in ticks:
                                    await self._publish(tick)
                                    ticks_published += 1
                                    if ticks_published % 500 == 0:
                                        logger.info("[LiveFeed] %d ticks published to Redis",
                                                    ticks_published)
                            else:
                                logger.info("[LiveFeed] Text msg#%d: %s", msgs_received, str(raw)[:120])
                                if not subscribed:
                                    await self._subscribe(ws)
                                    subscribed = True



                        except asyncio.TimeoutError:
                            # Periodic status log
                            now = time.time()
                            if now - last_heartbeat >= 30:
                                logger.info("[LiveFeed] Heartbeat: msgs=%d ticks=%d", 
                                            msgs_received, ticks_published)
                                last_heartbeat = now
                            continue


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

