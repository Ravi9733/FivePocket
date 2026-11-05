"""The Mac builder's Python 3.9 shim (2026-11-05). A Mac with Xcode has Python 3.9 (Xcode's /usr/bin/python3); the builder's tools
write text with Path.write_text(..., newline='\\n') (UTF-8/LF on every host), which Python gained in 3.10. The Mac launcher puts
this folder on PYTHONPATH, so Python imports it at start in the builder and in every stage it runs, and adds that argument with
3.10's meaning. On 3.10 and later it does nothing. No tool's bytes change (their hashes are pinned in the recipes and profiles)."""
import sys

if sys.version_info < (3, 10):
    import pathlib

    def _write_text(self, data, encoding=None, errors=None, newline=None):
        if not isinstance(data, str):
            raise TypeError('data must be str, not %s' % data.__class__.__name__)
        with self.open(mode='w', encoding=encoding, errors=errors, newline=newline) as f:
            return f.write(data)

    pathlib.Path.write_text = _write_text
