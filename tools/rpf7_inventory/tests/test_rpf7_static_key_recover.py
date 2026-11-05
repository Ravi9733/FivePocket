from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import rpf7_static_key_recover as MODULE  # noqa: E402


class StaticKeyRecoveryTests(unittest.TestCase):
    def _write_chunk(self, directory: Path, body: str) -> None:
        (directory / "ppc_recomp.0.cpp").write_text(body, encoding="utf-8")

    def test_recovers_big_endian_immediate_stores_without_emitting_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            self._write_chunk(
                source,
                """PPC_FUNC_IMPL(__imp__sub_80000000) {
    // lis r10,-31744
    // li r11,4660
    // addi r9,r10,256
    // sth r11,0(r9)
}
PPC_FUNC_IMPL(__imp__sub_80000010) {
    // lis r7,-31744
    // li r8,-21555
    // addi r6,r7,256
    // sth r8,2(r6)
}
""",
            )
            key, stores = MODULE.recover_constant_stores(
                source, target_address=0x84000100, length=4
            )

        self.assertEqual(key, bytes.fromhex("1234ABCD"))
        self.assertEqual([store.address for store in stores], [0x84000100, 0x84000102])
        self.assertEqual(
            [store.function for store in stores],
            ["sub_80000000", "sub_80000010"],
        )

    def test_unmodelled_instruction_invalidates_constant_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            self._write_chunk(
                source,
                """PPC_FUNC_IMPL(__imp__sub_80000000) {
    // lis r10,-31744
    // li r11,4660
    // add r9,r10,r4
    // sth r11,256(r10)
}
""",
            )
            with self.assertRaisesRegex(
                MODULE.StaticKeyRecoveryError, "cover 0/2"
            ):
                MODULE.recover_constant_stores(
                    source, target_address=0x84000100, length=2
                )

    def test_rejects_conflicting_writes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            self._write_chunk(
                source,
                """PPC_FUNC_IMPL(__imp__sub_80000000) {
    // lis r10,-31744
    // li r11,4660
    // addi r9,r10,256
    // sth r11,0(r9)
}
PPC_FUNC_IMPL(__imp__sub_80000010) {
    // lis r10,-31744
    // li r11,22136
    // addi r9,r10,256
    // sth r11,0(r9)
}
""",
            )
            with self.assertRaisesRegex(
                MODULE.StaticKeyRecoveryError, "conflicting writes"
            ):
                MODULE.recover_constant_stores(
                    source, target_address=0x84000100, length=2
                )


if __name__ == "__main__":
    unittest.main()
