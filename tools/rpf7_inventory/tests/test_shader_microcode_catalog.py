import hashlib
from pathlib import Path
import tempfile
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import shader_microcode_catalog as module
from test_xenos_preflight_batch import PrivateRepository
from test_xenos_translate_batch import tools
import xenos_translate_batch as conversion


def container(stage="vertex", payload=bytes(range(12))):
    data = bytearray(80)
    struct.pack_into(">9I", data, 0, 0x102a1100 | (stage == "vertex"), 68, 12, 0, 36, 0, 36, 0, 0)
    struct.pack_into(">II", data, 36, 0, 12)
    data[68:] = payload
    return bytes(data)


class ShaderMicrocodeCatalogTests(unittest.TestCase):
    def test_validated_association_rejects_mutation_and_symlink_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            chain = tools(repository)
            source = repository.private / "input"
            source.write_bytes(container())
            output = repository.private / "conversion"
            conversion.translate_batch([source], output, repository=repository.root, toolchain=chain,
                                       constant_binding="descriptor32")
            report = module.build_catalog(output, repository=repository.root, validator=chain.spirv_validator)
            self.assertEqual(report["lookup_key_count"], 1)
            self.assertEqual(report["ambiguous_key_count"], 0)
            self.assertFalse(report["runtime_lookup_integrated"])
            pack = module.native_catalog_pack(output, report, repository.root)
            self.assertEqual(pack[:8], b'XSCAT1\0\0')
            self.assertEqual(struct.unpack_from('<I', pack, 8)[0], 1)
            self.assertEqual(struct.unpack_from('<III', pack, 16), (1, 0, 12))
            self.assertEqual(pack[28:40], bytes(range(12)))
            self.assertEqual(pack[40:48], b'XSPVPK1\0')
            self.assertEqual(struct.unpack_from('<I', pack, 12)[0], len(pack) - 40)
            link = repository.private / "linked"
            link.symlink_to(output, target_is_directory=True)
            with self.assertRaises(module.XenosPreflightError):
                module.build_catalog(link, repository=repository.root, validator=chain.spirv_validator)
            payload = output / "shader-0001/container.xenos"
            data = bytearray(payload.read_bytes())
            data[-1] ^= 1
            payload.write_bytes(data)
            with self.assertRaisesRegex(module.ShaderCatalogError, "changed"):
                module.native_catalog_pack(output, report, repository.root)
            with self.assertRaisesRegex(module.ShaderCatalogError, "changed"):
                module.build_catalog(output, repository=repository.root, validator=chain.spirv_validator)
            payload.write_bytes(container())
            chain.spirv_validator.write_text(f"#!{sys.executable}\nraise SystemExit(1)\n")
            with self.assertRaisesRegex(module.ShaderCatalogError, "revalidation"):
                module.build_catalog(output, repository=repository.root, validator=chain.spirv_validator)

    def test_exact_bytes_stage_and_extent(self):
        identity = module.microcode_identity(container(), "vertex")
        self.assertEqual(identity["microcode_sha256"], hashlib.sha256(bytes(range(12))).hexdigest())
        self.assertEqual(identity["microcode_bytes"], 12)
        for data, stage in ((container(), "pixel"), (container()[:-1], "vertex"), (b"", "vertex")):
            with self.assertRaises(module.ShaderCatalogError):
                module.microcode_identity(data, stage)
        for offset, size in ((1, 12), (0, 0), (0, 11), (0xffffffff, 12)):
            data = bytearray(container())
            struct.pack_into(">II", data, 36, offset, size)
            with self.assertRaises(module.ShaderCatalogError):
                module.microcode_identity(bytes(data), "vertex")

    def test_groups_identical_artifacts_but_preserves_different_translations(self):
        entries = []
        for index, stage, shader in ((1, "vertex", "a"), (2, "vertex", "a"),
                                     (3, "vertex", "b"), (4, "pixel", "c")):
            entry = module.microcode_identity(container(stage), stage)
            entry.update(index=index, spirv_sha256=shader)
            entries.append(entry)
        report = module.group_entries(entries, "descriptor32")
        self.assertEqual(report["artifact_count"], 4)
        self.assertEqual(report["lookup_key_count"], 2)
        self.assertEqual(report["ambiguous_key_count"], 1)
        vertex = next(r for r in report["lookups"] if r["stage"] == "vertex")
        self.assertEqual(vertex["resolution"], "ambiguous")
        self.assertEqual(vertex["candidates"][0]["indices"], [1, 2])
        self.assertFalse(report["runtime_lookup_integrated"])


if __name__ == "__main__":
    unittest.main()
