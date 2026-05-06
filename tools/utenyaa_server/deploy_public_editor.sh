#!/bin/bash
set -e
echo "=== extract editor ==="
sudo tar -xzf /tmp/utenyaa-editor.tar.gz -C /opt/utenyaa-editor
sudo chown -R gary:gary /opt/utenyaa-editor

echo "=== install/refresh systemd units ==="
sudo cp /tmp/utenyaa-editor.service /etc/systemd/system/utenyaa-editor.service
sudo cp /tmp/utenyaa-editor-public.service /etc/systemd/system/utenyaa-editor-public.service
sudo systemctl daemon-reload

echo "=== restart admin instance + start public instance ==="
sudo systemctl restart utenyaa-editor
sudo systemctl enable --now utenyaa-editor-public
sleep 2
sudo systemctl is-active utenyaa-editor utenyaa-editor-public

echo "=== nginx route patch (idempotent) ==="
# Drop a previous version of the editor block, then re-insert with current code
sudo python3 - <<'PY'
import re
p = "/etc/nginx/sites-enabled/saturncoup"
s = open(p).read()
# Remove old block(s) — match from the comment line through the matching end of the second location.
import re
new = re.sub(
    r"    # Utenyaa Web Map Editor.*?    }\n\n(    # PUBLIC Utenyaa Web Map Editor.*?    }\n\n)?",
    "",
    s, count=1, flags=re.S)
if new != s:
    open(p, "w").write(new)
    print("removed old editor block(s)")
else:
    print("no prior editor block to remove")
PY
sudo python3 /tmp/patch_nginx_editor.py
sudo nginx -t 2>&1 | tail -3
sudo systemctl reload nginx

echo "=== smoke test ==="
echo -n "  admin (no auth, expect 401): "
curl -s -k -o /dev/null -w "%{http_code}\n" https://saturncoup.duckdns.org/admin/editor/
echo -n "  public (no auth, expect 200): "
curl -s -k -o /dev/null -w "%{http_code}\n" https://saturncoup.duckdns.org/mapeditor/
echo -n "  public api/admin/status (expect public_mode=true): "
curl -s -k https://saturncoup.duckdns.org/mapeditor/api/admin/status

echo
echo "=== done ==="
