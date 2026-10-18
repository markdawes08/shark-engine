# Public TLS test identities

Every private key in this directory is **public, test-only material**. Never use
these identities or trust roots in a game, deployment or another application.
There is no runtime test-authentication bypass: tests explicitly pass the local
fixture CA and require the certificate's DNS name `shark.test`.

These original Shark test fixtures use P-256 keys, ECDSA/SHA-256 certificates and
fixed validity windows. `ca.pem` and `server.pem` are valid from 2020-01-01 through
2040-01-01 UTC. The leaf has `CA:FALSE`, digital-signature key usage, server-auth
extended key usage and SAN `DNS:shark.test`. `expired-server.pem` uses the same
server key with a 2000-01-01 through 2010-01-01 validity window. `wrong-ca.pem`
is an unrelated root. Both roots have `CA:TRUE,pathlen:0` and certificate-signing
usage. Tests require real server authentication; they do not authenticate players.

The fixtures are provided under the repository's MIT license. They contain no
external account credentials, personal data or third-party content.

`generate.py` records the developer-only generation recipe. OpenSSL is needed only
to regenerate fixtures, never to run tests. On this development PC:

```text
wsl.exe -d Ubuntu --exec python3 /mnt/c/Users/mrkda/Projects/shark/tests/fixtures/network/generate.py
```

The generator retains the committed `ca-key.pem`, `wrong-ca-key.pem` and
`server-key.pem` identities, fixes subjects/extensions/serials/dates, and uses a
temporary signing database. ECDSA signatures use fresh randomness, so regeneration
does not promise identical certificate bytes. Missing keys are generated with
OpenSSL's P-256 implementation. Do not regenerate fixtures during builds or tests.

The integration test launches separate loopback server and client processes with
bounded deadlines. Independent Python standard-library TLS peers additionally
exercise clean close-notify with a complete reply, partial-frame clean EOF,
incompatible wire versions, replayed sequence numbers, unknown kinds and
oversized advertised payloads. These peers run in the test coordinator and are
recorded separately from the engine processes. Their normal TLS 1.3 session
tickets remain enabled to test post-handshake interoperability without enabling
session resumption in Shark. Fault cases delay or close a stream; the deterministic
`FaultLink` utility separately models message/datagram loss, duplication, jitter,
reorder and partition. Dropping arbitrary TLS/TCP bytes is not a packet-loss model.
