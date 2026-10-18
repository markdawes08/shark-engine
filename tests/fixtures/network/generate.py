"""Developer-only OpenSSL regeneration of public, nonproduction TLS test fixtures.

Existing P-256 private keys are retained. Missing keys are generated explicitly;
tests never run this script or require OpenSSL. ECDSA signatures use randomness,
so regeneration preserves identity/constraints/dates, not certificate bytes.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openssl', default='openssl')
    args = parser.parse_args()
    output = Path(__file__).resolve().parent

    def run(*arguments):
        subprocess.run([args.openssl, *map(str, arguments)], check=True,
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=30)

    for key in ('ca-key.pem', 'wrong-ca-key.pem', 'server-key.pem'):
        path = output / key
        if not path.exists():
            run('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', path)

    with tempfile.TemporaryDirectory(prefix='shark-public-tls-fixtures-') as directory:
        work = Path(directory)
        for issuer, subject in (('ca', 'Shark PUBLIC TEST ONLY CA'),
                                ('wrong-ca', 'Shark PUBLIC TEST ONLY WRONG CA')):
            state = work / issuer
            state.mkdir()
            (state / 'index').write_text('', encoding='ascii')
            (state / 'serial').write_text('1000\n', encoding='ascii')
            configuration = state / 'openssl.cnf'
            configuration.write_text(f'''[ca]
default_ca = fixture
[fixture]
database = {state / 'index'}
serial = {state / 'serial'}
new_certs_dir = {state}
certificate = {output / (issuer + '.pem')}
private_key = {output / (issuer + '-key.pem')}
default_md = sha256
policy = fixture_policy
unique_subject = no
[fixture_policy]
commonName = supplied
[ca_ext]
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[server_ext]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:shark.test
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
''', encoding='ascii')
            request = state / 'ca.csr'
            run('req', '-new', '-sha256', '-key', output / (issuer + '-key.pem'),
                '-subj', '/CN=' + subject, '-out', request)
            run('ca', '-batch', '-selfsign', '-notext', '-config', configuration,
                '-extensions', 'ca_ext', '-startdate', '20200101000000Z',
                '-enddate', '20400101000000Z', '-in', request, '-out', output / (issuer + '.pem'))
            if issuer == 'ca':
                request = state / 'server.csr'
                run('req', '-new', '-sha256', '-key', output / 'server-key.pem',
                    '-subj', '/CN=shark.test', '-out', request)
                for name, start, end in (('server.pem', '20200101000000Z', '20400101000000Z'),
                                         ('expired-server.pem', '20000101000000Z', '20100101000000Z')):
                    run('ca', '-batch', '-notext', '-config', configuration,
                        '-extensions', 'server_ext', '-startdate', start, '-enddate', end,
                        '-in', request, '-out', output / name)


if __name__ == '__main__':
    main()
