"""Exercise the CLI result path with oversized and empty catalog responses."""
import contextlib
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import catalog_reference
import http_conformance


class CatalogResults(unittest.TestCase):
    def run_catalog(self, body):
        output = io.StringIO()
        with patch.object(sys, "argv", ["http_conformance.py", "http://127.0.0.1/"]), \
                patch.object(http_conformance.http.client, "HTTPConnection") as connection, \
                patch.object(http_conformance, "get", return_value=(200, {}, body)), \
                contextlib.redirect_stdout(output):
            result = http_conformance.main()
        return result, output.getvalue(), connection.return_value

    def test_byte_limit(self):
        result, output, connection = self.run_catalog(b"#" * (catalog_reference.CATALOG_BYTES_MAX + 1))
        self.assertEqual(result, 1)
        self.assertIn("0 passed, 1 failed, 0 skipped", output)
        self.assertIn("limit is", output)
        connection.close.assert_called_once()

    def test_record_limit(self):
        body = "".join("G{:04d},Game,DVD,game.iso\n".format(i)
                       for i in range(catalog_reference.CATALOG_ROWS_MAX + 1)).encode()
        result, output, connection = self.run_catalog(body)
        self.assertEqual(result, 1)
        self.assertIn("0 passed, 1 failed, 0 skipped", output)
        self.assertIn("more than", output)
        connection.close.assert_called_once()

    def test_empty_catalog_still_passes(self):
        result, output, _ = self.run_catalog(b"")
        self.assertEqual(result, 0)
        self.assertIn("1 passed, 0 failed, 0 skipped", output)


if __name__ == "__main__":
    unittest.main()
