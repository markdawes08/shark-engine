"""Device-free validation of the explicit native window cube fixture's CLI."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    executable = parser.parse_args().executable.resolve()
    result = subprocess.run([str(executable), '--describe'], capture_output=True, text=True,
                            timeout=10, check=False)
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert result.returncode == 0 and len(rows) == 2, result
    assert rows[0]['kind'] == 'identity' and rows[0]['mode'] == 'describe'
    assert rows[1] == {'kind': 'description', 'device_accessed': False,
                       'cases': 3, 'qualification': 'NOT_RUN'}
    valid = ['--smoke', '--device', '0' * 32, '--bundle', 'missing.sharkshader',
             '--output', 'unused', '--validation', 'required']
    cases = [[], ['--help'], ['--smoke'], ['--describe', '--device', '0' * 32],
             valid[:-1], valid + ['--extra', 'value'],
             [*valid[:2], 'auto', *valid[3:]], [*valid[:-1], 'optional'],
             [*valid[:3], '--output', *valid[4:]]]
    for args in cases:
        result = subprocess.run([str(executable), *args], capture_output=True, text=True,
                                timeout=10, check=False)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        assert result.returncode != 0 and len(rows) == 1, (args, result)
        assert rows[0]['error']['operation'] == 'cube-smoke.arguments', (args, rows)
    print(f'PASS: description and {len(cases)} argument rejections; no device accessed')


if __name__ == '__main__':
    main()
