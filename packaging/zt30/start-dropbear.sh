#!/bin/sh
# Same qualified key-directory/foreground setup as the RAM development load.
set -eu
app_dir=/customer/camera-app
mkdir -p "$app_dir/auth"
chmod 0700 "$app_dir/auth"
cd "$app_dir/auth"
if [ ! -s dropbear_ed25519_host_key ]; then
    umask 077
    "$app_dir/dropbearkey" -t ed25519 -f dropbear_ed25519_host_key.new
    mv dropbear_ed25519_host_key.new dropbear_ed25519_host_key
fi
[ -f authorized_keys ] || : >authorized_keys
chmod 0600 authorized_keys
export LD_LIBRARY_PATH=/customer/ssh/lib:/lib:/usr/lib
exec "$app_dir/dropbear" -F -E -s -j -k -p 2222 -D . -r dropbear_ed25519_host_key
