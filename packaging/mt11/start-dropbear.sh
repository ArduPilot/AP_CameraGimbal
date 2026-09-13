#!/bin/sh
set -eu

host_key=/app/dropbear/dropbear_ed25519_host_key
temporary_key=$host_key.new.$$
authorized_keys=/app/dropbear/authorized_keys
runtime_authorized_keys_dir=/dev/dropbear-auth
runtime_authorized_keys=$runtime_authorized_keys_dir/authorized_keys
temporary_authorized_keys=$runtime_authorized_keys.new.$$

cleanup()
{
    rm -f "$temporary_key" "$temporary_authorized_keys"
}
trap cleanup EXIT HUP INT TERM

if [ ! -s "$host_key" ]; then
    umask 077
    /app/bin/dropbearkey -t ed25519 -f "$temporary_key"
    mv "$temporary_key" "$host_key"
fi

# /app persists across upgrades, while /dev is recreated at each boot. Keep
# the live key file on /dev so the web service can update it atomically without
# writing the read-only rootfs.
mkdir -p "$runtime_authorized_keys_dir"
chmod 0700 "$runtime_authorized_keys_dir"
if [ -f "$authorized_keys" ]; then
    cp "$authorized_keys" "$temporary_authorized_keys"
else
    : >"$temporary_authorized_keys"
fi
chmod 0600 "$temporary_authorized_keys"
mv "$temporary_authorized_keys" "$runtime_authorized_keys"

trap - EXIT HUP INT TERM

# Permit shell and rsync-over-SSH access, but not SSH port forwarding. Run in
# the foreground because app_init backgrounds this launcher and captures logs.
exec /app/bin/dropbear -F -E -j -k -p 22 \
    -P /run/dropbear.pid -r "$host_key" -D "$runtime_authorized_keys_dir"
