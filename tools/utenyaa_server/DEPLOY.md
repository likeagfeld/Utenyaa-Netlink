# Utenyaa server — saturncoup VM deploy

Target VM: `gary@saturncoup.duckdns.org` (GCP `us-central1-c`).

## First-time setup

```bash
# From dev machine
scp -i ~/.ssh/google_compute_engine \
    tools/utenyaa_server/userver.py \
    tools/utenyaa_server/utenyaa.service \
    gary@saturncoup.duckdns.org:/tmp/

# On the VM
sudo cp /tmp/userver.py /home/gary/userver.py
sudo cp /tmp/utenyaa.service /etc/systemd/system/utenyaa.service
sudo systemctl daemon-reload
sudo systemctl enable --now utenyaa
sudo systemctl is-active utenyaa
```

## Code-only update (hot path)

```bash
scp -i ~/.ssh/google_compute_engine \
    tools/utenyaa_server/userver.py \
    gary@saturncoup.duckdns.org:/tmp/
ssh -i ~/.ssh/google_compute_engine gary@saturncoup.duckdns.org \
    "sudo cp /tmp/userver.py /home/gary/userver.py && sudo systemctl restart utenyaa"
```

## Smoke test

```bash
# On the VM
curl -s -H 'X-Admin-Auth: nginx-verified' http://127.0.0.1:9093/api/state | jq .
```

## Unified admin portal

Add `utenyaa` as a new tab in `/opt/saturn-admin/unified_admin.py`. The
handler just proxies `/admin/utenyaa/*` → `http://127.0.0.1:9093/*`
with the `X-Admin-Auth: nginx-verified` header injected, same pattern
as the existing `disasteroids` and `flock` tabs.

Services to restart after editing the admin module:

```bash
sudo systemctl restart saturn-admin
```

## Ports

| Service | Local bind | Public (nginx path) |
|---|---|---|
| Utenyaa game server | `0.0.0.0:4825` | `:4825` (tcp) |
| Utenyaa admin API   | `127.0.0.1:9093` | `/admin/utenyaa/` |
