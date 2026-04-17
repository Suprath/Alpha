# Alpha Proto Schema Registry

All `.proto` files in this directory are the **single source of truth** for inter-service
message contracts in the Alpha platform. Generated code is **never committed** — each
service generates its own bindings at build/image-build time.

---

## Schema inventory

| File | Package | Owned by | Transport | Producers | Consumers |
|---|---|---|---|---|---|
| `market_data_v3.proto` | `com.upstox.marketdatafeederv3udapi.rpc.proto` | Upstox (external) | Upstox WebSocket wire | Upstox exchange | `data_feed` |
| `alpha_tick.proto` | `alpha.feed` | `data_feed` | Redis Stream `alpha:ticks`, field `data` (binary) | `data_feed` | `ingester`, `signal_engine` |

---

## Field number policy

- **1–15**: hot fields — 1-byte tag encoding. Reserve for highest-frequency fields.
- **16–31**: warm fields — 2-byte tag encoding.
- **≥ 32**: extension space, reserved for future use.
- **Never reuse a field number**, even after deletion. Mark removed fields as `reserved`.

## Backwards-compatibility rules

| Change | Safe? |
|---|---|
| Add a new optional field | ✅ always safe |
| Rename a field | ✅ wire format uses numbers, not names |
| Remove a field (mark `reserved`) | ✅ safe if consumers handle missing fields |
| Change a field number | ❌ breaking — treat as a new field |
| Change a field type (e.g. `int32` → `int64`) | ❌ breaking |
| Change `repeated` ↔ singular | ❌ breaking |

---

## Generating bindings

### Python (data_feed, research API)

Run inside the service Docker build or locally:

```bash
pip install grpcio-tools
python -m grpc_tools.protoc \
    --proto_path=shared/proto \
    --python_out=<output_dir> \
    shared/proto/alpha_tick.proto
```

Generated files: `alpha_tick_pb2.py`

### C++ (ingester, signal_engine)

Handled automatically by CMake. The `add_custom_command` in each service's
`CMakeLists.txt` calls `protoc` at build time and writes `.pb.h` / `.pb.cc` into
the build directory.

```bash
# Manually (from repo root):
protoc \
    --proto_path=shared/proto \
    --cpp_out=<output_dir> \
    shared/proto/alpha_tick.proto
```

Generated files: `alpha_tick.pb.h`, `alpha_tick.pb.cc`

---

## Redis transport for `alpha_tick.proto`

Ticks are published as a single binary field per stream entry:

```
XADD alpha:ticks * data <serialized Tick bytes>
```

The `data` field contains the raw `Tick.SerializeToString()` bytes.
No base64 encoding — hiredis handles binary-safe strings natively.
