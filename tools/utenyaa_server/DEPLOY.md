# Utenyaa server — deploy notes

`userver.py` is a standard-library Python 3 script. Deploy it wherever
you run your other Saturn NetLink revival servers. The service expects
a port to listen on for SNCP traffic and an optional local admin port.

## systemd unit

`utenyaa.service` is provided as a minimal template. Edit the paths
for your environment and install into `/etc/systemd/system/`. Enable
with `systemctl enable --now utenyaa`.

## Updating

Replace `userver.py` on the host and `systemctl restart utenyaa`.
