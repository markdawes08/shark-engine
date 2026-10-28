"""Keep only unexpected failures from disposable negative-control reports."""

from contextlib import contextmanager
from pathlib import Path
import shutil
import tempfile


@contextmanager
def temporary_control_report(destination: Path):
    """Yield a scratch report; retain its bytes at destination if validation fails."""
    with tempfile.TemporaryDirectory(prefix="shark-negative-control-",
                                     dir=destination.parent) as temporary:
        report = Path(temporary) / destination.name
        try:
            yield report
        except BaseException:
            if report.is_file():
                shutil.copyfile(report, destination)
            raise
