"""Explicit developer-only generation of public M02b client-certificate fixtures.

Run with Python and OpenSSL in the existing WSL Ubuntu environment. This script
only writes its own session directory; the existing M02a issuers and server
fixtures are read-only inputs. Tests and builds never regenerate certificates.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openssl', default='openssl')
    args = parser.parse_args()
    output = Path(__file__).resolve().parent
    issuer_root = output.parent
    original = {path: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in issuer_root.glob('*') if path.is_file()}
    required = ('ca.pem', 'ca-key.pem', 'wrong-ca.pem', 'wrong-ca-key.pem')
    if any(not (issuer_root / name).is_file() for name in required):
        parser.error('Existing public M02a CA certificates and keys are required')

    def run(*arguments):
        subprocess.run([args.openssl, *map(str, arguments)], check=True,
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=30)

    for name in ('client-a-key.pem', 'client-b-key.pem'):
        path = output / name
        if not path.exists():
            run('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', path)

    fixtures = (
        ('client-a.pem', 'client-a', 'ca', 'client', '20200101000000Z', '20400101000000Z'),
        ('client-b.pem', 'client-b', 'ca', 'client', '20200101000000Z', '20400101000000Z'),
        ('expired-client.pem', 'client-a', 'ca', 'client', '20000101000000Z', '20100101000000Z'),
        ('wrong-ca-client.pem', 'client-a', 'wrong-ca', 'client', '20200101000000Z', '20400101000000Z'),
        ('wrong-eku-client.pem', 'client-a', 'ca', 'server', '20200101000000Z', '20400101000000Z'),
        ('no-eku-client.pem', 'client-a', 'ca', 'no_eku', '20200101000000Z', '20400101000000Z'),
        ('wrong-ku-client.pem', 'client-a', 'ca', 'wrong_ku', '20200101000000Z', '20400101000000Z'),
        ('ca-client.pem', 'client-a', 'ca', 'ca_client', '20200101000000Z', '20400101000000Z'),
    )
    with tempfile.TemporaryDirectory(prefix='shark-public-session-fixtures-') as directory:
        work = Path(directory)
        configurations = {}
        for issuer in ('ca', 'wrong-ca'):
            state = work / issuer
            state.mkdir()
            (state / 'index').write_text('', encoding='ascii')
            (state / 'serial').write_text('2000\n', encoding='ascii')
            configuration = state / 'openssl.cnf'
            configuration.write_text(f'''[ca]
default_ca = fixture
[fixture]
database = {state / 'index'}
serial = {state / 'serial'}
new_certs_dir = {state}
certificate = {issuer_root / (issuer + '.pem')}
private_key = {issuer_root / (issuer + '-key.pem')}
default_md = sha256
policy = fixture_policy
unique_subject = no
[fixture_policy]
commonName = supplied
[client]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[server]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[no_eku]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[wrong_ku]
basicConstraints = critical,CA:FALSE
keyUsage = critical,keyAgreement
extendedKeyUsage = clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[ca_client]
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,digitalSignature,keyCertSign
extendedKeyUsage = clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
''', encoding='ascii')
            configurations[issuer] = configuration
        requests = {}
        for identity in ('client-a', 'client-b'):
            request = work / (identity + '.csr')
            run('req', '-new', '-sha256', '-key', output / (identity + '-key.pem'),
                '-subj', '/CN=Shark PUBLIC TEST ONLY ' + identity, '-out', request)
            requests[identity] = request
        for name, identity, issuer, extensions, start, end in fixtures:
            run('ca', '-batch', '-notext', '-config', configurations[issuer],
                '-extensions', extensions, '-startdate', start, '-enddate', end,
                '-in', requests[identity], '-out', output / name)

    changed = [str(path) for path, digest in original.items()
               if hashlib.sha256(path.read_bytes()).hexdigest() != digest]
    if changed:
        raise RuntimeError('Existing M02a fixture inputs changed: ' + ', '.join(changed))
    print('Generated eight public client certificates; existing M02a inputs unchanged')


if __name__ == '__main__':
    main()
