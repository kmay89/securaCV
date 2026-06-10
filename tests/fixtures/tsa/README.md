# RFC 3161 test fixtures

Generated once with OpenSSL 3.0 acting as a throwaway local TSA, so the
Rust implementation in `src/tsa.rs` is tested against an independent
implementation rather than against itself. The TSA key/cert here sign
**fixtures only** — they hold no trust and never will.

All artifacts cover `sha256("securacv-fixture")` =
`89e4120edf60957bde82c27f35ef282ba6608dfdc29b454534ae136da1cd9dd2`.

| file | what |
|---|---|
| `query_nononce.tsq` | `openssl ts -query -digest <d> -sha256 -cert -no_nonce` |
| `query_nonce.tsq` | same with a nonce (`0xAC06D335CA6B0758`) |
| `reply.tsr` | granted response to `query_nononce.tsq` (genTime `20260610123324Z`, serial 2) |
| `reply_nonce.tsr` | granted response to `query_nonce.tsq` (serial 3) |
| `tsa.crt` | the fixture TSA's self-signed certificate (P-256, EKU `critical,timeStamping`) |

To regenerate (new genTime/serials — update the assertions in
`tests/tsa_rfc3161.rs`):

```sh
cat > tsa.cnf <<'EOF'
[ req ]
distinguished_name = dn
prompt = no
[ dn ]
CN = SecuraCV Test TSA
O = Fixture Only
[ tsa_cert ]
extendedKeyUsage = critical,timeStamping
keyUsage = critical,digitalSignature
basicConstraints = CA:false
[ tsa ]
default_tsa = tsa_config1
[ tsa_config1 ]
serial = ./serial
crypto_device = builtin
signer_cert = ./tsa.crt
signer_key = ./tsa.key
default_policy = 1.3.6.1.4.1.13762.3
digests = sha256
accuracy = secs:1
ordering = no
tsa_name = no
ess_cert_id_chain = no
signer_digest = sha256
EOF
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
  -keyout tsa.key -out tsa.crt -days 3650 -nodes \
  -config tsa.cnf -extensions tsa_cert
echo 01 > serial
DIGEST=$(printf 'securacv-fixture' | sha256sum | cut -d' ' -f1)
openssl ts -query -digest $DIGEST -sha256 -cert -no_nonce -out query_nononce.tsq
openssl ts -query -digest $DIGEST -sha256 -cert -out query_nonce.tsq
OPENSSL_CONF=tsa.cnf openssl ts -reply -queryfile query_nononce.tsq -out reply.tsr -section tsa_config1
OPENSSL_CONF=tsa.cnf openssl ts -reply -queryfile query_nonce.tsq -out reply_nonce.tsr -section tsa_config1
```
