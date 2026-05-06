#!/usr/bin/env python3
"""Insert /admin/editor/ proxy block into the saturncoup nginx config.
Idempotent — re-running is a no-op once the block is present."""
import sys

CONFIG = "/etc/nginx/sites-enabled/saturncoup"
ANCHOR = "    # Unified Saturn NetLink admin portal"

BLOCK = """    # Utenyaa Web Map Editor (proxied behind admin auth).
    # ^~ modifier is REQUIRED — without it, the `~* \\.(?:js|css)$`
    # regex location elsewhere in this server block takes priority
    # over the prefix match, sending /admin/editor/static/editor.css
    # to the static-file handler (which 404s — the asset lives in
    # /opt/utenyaa-editor, not /opt/coup-server/web). With ^~ the
    # prefix wins outright and the editor's CSS/JS proxies through.
    location = /admin/editor { return 301 /admin/editor/; }
    location ^~ /admin/editor/ {
        auth_basic "Saturn Admin";
        auth_basic_user_file /etc/nginx/.htpasswd_admin;

        proxy_pass http://127.0.0.1:9095/;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_connect_timeout 5s;
        proxy_read_timeout 30s;
        client_max_body_size 256k;
    }

"""

with open(CONFIG, "r") as f:
    s = f.read()

if "location /admin/editor/" in s:
    print("already present")
    sys.exit(0)

new = s.replace(ANCHOR, BLOCK + ANCHOR, 1)
if new == s:
    print("anchor not found, aborting", file=sys.stderr)
    sys.exit(1)

with open(CONFIG, "w") as f:
    f.write(new)
print("inserted")
