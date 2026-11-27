# CA certificates for cloud upload

`cloud-roots.pem` holds the root certificates the camera's cloud uploader trusts:
for Google (the Google Trust Services roots and the GlobalSign roots they are
cross-signed by) and for Dropbox (DigiCert). It is a small subset of Mozilla's CA bundle (Debian's
`ca-certificates` package, `/usr/share/ca-certificates/mozilla`), so the
uploader works on cameras that have no CA store of their own.

Another cloud service adds its own roots here. To check what a service needs:

    openssl s_client -connect oauth2.googleapis.com:443 -showcerts </dev/null
    openssl s_client -connect api.dropboxapi.com:443 -showcerts </dev/null

Roots expire or are replaced over the years; refresh the file from a current
Mozilla bundle when a service changes its chain.
