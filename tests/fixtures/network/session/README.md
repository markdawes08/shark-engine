# Public client authentication fixtures

Every certificate and private key here is **public, test-only material**, under
the repository's MIT license. These original Shark identities have no external
account, production authority or personal data. Builds and installed packages
must never acquire them as default credentials. Tests explicitly supply the
fixture trust root and enroll certificate fingerprints; there is no test-auth
bypass in the session service.

These fixtures extend M02a's [public TLS fixtures](../README.md) without changing
their bytes. The existing `../ca.pem` signs all leaves except
`wrong-ca-client.pem`, which uses `../wrong-ca.pem`. Keys use P-256 and certificates
use ECDSA/SHA-256. Both valid clients have explicit `CA:FALSE`, `digitalSignature`
key usage and `clientAuth` extended key usage. Client A and B have distinct keys
and certificate fingerprints; certificate subject text never grants account
ownership.

| Certificate | Private key | Intended property |
| --- | --- | --- |
| `client-a.pem` | `client-a-key.pem` | Valid first identity |
| `client-b.pem` | `client-b-key.pem` | Valid distinct identity |
| `expired-client.pem` | `client-a-key.pem` | Expired leaf |
| `wrong-ca-client.pem` | `client-a-key.pem` | Untrusted issuer |
| `wrong-eku-client.pem` | `client-a-key.pem` | `serverAuth` instead of `clientAuth` |
| `no-eku-client.pem` | `client-a-key.pem` | Missing explicit extended key usage |
| `wrong-ku-client.pem` | `client-a-key.pem` | `keyAgreement` instead of `digitalSignature` |
| `ca-client.pem` | `client-a-key.pem` | `CA:TRUE` despite otherwise valid client usage |

All certificates have fixed validity from **2020-01-01 00:00:00 UTC** through
**2040-01-01 00:00:00 UTC**, except `expired-client.pem`, whose validity is
**2000-01-01 00:00:00 UTC** through **2010-01-01 00:00:00 UTC**. The missing-EKU,
keyAgreement-only and CA=true fixtures pass OpenSSL's general `sslclient` purpose
check; this does not establish compliance with TLS 1.3 and Shark's explicit
client profile. They must fail before issuing player authority. Negative leaves
reuse A's key deliberately, but their different DER fingerprints do not inherit
A's account enrollment.

The current SHA-256 fingerprints of the complete DER leaf certificates are:

| Identity | DER SHA-256 |
| --- | --- |
| A | `96baafc1e5c4ef1173b8d8c0ffa4916b39c633cc3b88b100f86fd48b9810fb20` |
| B | `95e15d6eab226755438ab548b7bfd23a3e9362c945ac3c0a8814b26bc441520b` |

`generate.py` is an explicit developer-only recipe. It reads the existing public
issuer certificates/keys, retains existing client private keys and creates
missing client keys with OpenSSL. It writes only this directory and checks that
the parent M02a inputs retain their original hashes. Run from the repository root
on this machine using the existing WSL Ubuntu OpenSSL:

```text
wsl.exe -d Ubuntu --exec python3 /mnt/c/Users/mrkda/Projects/shark/tests/fixtures/network/session/generate.py --openssl /usr/bin/openssl
```

Initial generation used OpenSSL 3.5.5. No new runtime or build dependency is
introduced. Tests never invoke the generator. ECDSA signing uses fresh randomness;
regeneration preserves keys, certificate constraints and dates, but changes DER
bytes and fingerprints. Review changed fixtures and update this fingerprint table
and any recorded evidence explicitly after regeneration.
