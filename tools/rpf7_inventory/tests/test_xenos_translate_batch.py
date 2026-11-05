from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import xenos_translate_batch as module
from test_xenos_preflight_batch import PrivateRepository, fxc, outer_container


def module_bytes(stage: str = "vertex") -> bytes:
    # Framing fixture only: the fake validator accepts it, but this is not an
    # executable shader. Real spirv-val remains mandatory in production.
    words = [0x07230203, 0x10300, 0, 8, 0, (2 << 16) | 17, 1,
             (5 << 16) | 15, 0 if stage == "vertex" else 4, 1, 0x6e69616d, 0]
    return struct.pack(f"<{len(words)}I", *words)


def script(repository: PrivateRepository, name: str, body: str) -> Path:
    path = repository.root / name
    path.write_text(f"#!{sys.executable}\n" + body)
    path.chmod(0o700)
    return path


def tools(repository: PrivateRepository, *, compile_success: bool = True,
          validate_success: bool = True) -> module.Toolchain:
    translator = script(repository, "translator", """import pathlib, sys
pathlib.Path(sys.argv[2]).write_text('synthetic shader text')
""")
    dxc = script(repository, "dxc", f"""import pathlib, struct, sys
if {not compile_success!r}:
    print('private compiler error')
    raise SystemExit(1)
stage = sys.argv[sys.argv.index('-T') + 1]
words = [0x07230203, 0x10300, 0, 8, 0, (2 << 16) | 17, 1,
         (5 << 16) | 15, 0 if stage == 'vs_6_0' else 4, 1, 0x6e69616d, 0]
pathlib.Path(sys.argv[sys.argv.index('-Fo') + 1]).write_bytes(struct.pack('<12I', *words))
""")
    validator = script(repository, "spirv-validator",
                       f"raise SystemExit({0 if validate_success else 1})\n")
    header = repository.root / "common.h"
    header.write_text("// synthetic header\n")
    header.chmod(0o644)
    return module.Toolchain(translator, repository.validator(), header, dxc, validator)


class ShaderTranslationTests(unittest.TestCase):
    def test_cleanup_reaps_exited_leader_before_accepting_missing_group(self) -> None:
        process = mock.Mock(pid=123, **{'poll.return_value': 0})
        with mock.patch.object(module.os, 'killpg',
                side_effect=[PermissionError(1, 'denied'), ProcessLookupError(3, 'gone')]) as kill:
            module._terminate_worker_group(process)
        process.poll.assert_called_once_with()
        self.assertEqual(kill.call_args_list, [mock.call(123, module.signal.SIGKILL)] * 2)

    def test_cleanup_still_kills_descendants_after_reaping_leader(self) -> None:
        process = mock.Mock(pid=123, **{'poll.return_value': 0})
        with mock.patch.object(module.os, 'killpg',
                side_effect=[PermissionError(1, 'denied'), None]) as kill:
            module._terminate_worker_group(process)
        process.poll.assert_called_once_with()
        self.assertEqual(kill.call_count, 2)

    def test_cleanup_does_not_hide_permission_error_for_live_leader(self) -> None:
        process = mock.Mock(pid=123, **{'poll.return_value': None})
        with mock.patch.object(module.os, 'killpg', side_effect=PermissionError(1, 'denied')) as kill:
            with self.assertRaises(PermissionError):
                module._terminate_worker_group(process)
        kill.assert_called_once_with(123, module.signal.SIGKILL)

    def test_cleanup_does_not_hide_permission_error_for_remaining_group(self) -> None:
        # The group stays denied: the check is repeated until the wait is over, then the error stands.
        process = mock.Mock(pid=123, **{'poll.return_value': 0})
        clock = iter([100.0, 100.5, 101.0, 101.5, 102.0, 102.5])                 # the deadline is 102.0
        with mock.patch.object(module.os, 'killpg', side_effect=[PermissionError(1, 'denied')] * 6) as kill, \
                mock.patch.object(module.time, 'monotonic', side_effect=lambda: next(clock)), \
                mock.patch.object(module.time, 'sleep') as sleep:
            with self.assertRaises(PermissionError):
                module._terminate_worker_group(process)
        self.assertEqual(module.ZOMBIE_GROUP_WAIT_SECONDS, 2.0)
        self.assertEqual(kill.call_count, 5)          # the first, then one per reading of the clock up to the deadline
        self.assertEqual(sleep.call_count, 3)
        process.poll.assert_called_once_with()

    def test_cleanup_accepts_a_group_that_is_already_gone(self) -> None:
        process = mock.Mock(pid=123, **{'poll.return_value': 0})
        with mock.patch.object(module.os, 'killpg', side_effect=ProcessLookupError(3, 'gone')) as kill:
            module._terminate_worker_group(process)
        kill.assert_called_once_with(123, module.signal.SIGKILL)
        process.poll.assert_not_called()

    def test_cleanup_waits_for_exited_descendants_that_are_not_reaped_yet(self) -> None:
        # Seen on a loaded machine: the leader is reaped, the group still answers EPERM for a moment, then is gone.
        process = mock.Mock(pid=123, **{'poll.return_value': 0})
        denied = PermissionError(1, 'denied')
        with mock.patch.object(module.os, 'killpg', side_effect=[denied, denied, denied, ProcessLookupError(3, 'gone')]) as kill, \
                mock.patch.object(module.time, 'sleep') as sleep:
            module._terminate_worker_group(process)
        self.assertEqual(kill.call_args_list, [mock.call(123, module.signal.SIGKILL)] * 4)
        self.assertEqual(sleep.call_args_list, [mock.call(0.02)] * 2)
        # and when a later check can signal the group after all, that is a finished cleanup too
        with mock.patch.object(module.os, 'killpg', side_effect=[denied, denied, None]) as kill, \
                mock.patch.object(module.time, 'sleep'):
            module._terminate_worker_group(process)
        self.assertEqual(kill.call_count, 3)

    def test_framing_and_stage_are_checked(self) -> None:
        self.assertEqual(module.spirv_metadata(module_bytes(), "vertex")["capabilities"], [1])
        invalid = [b"", module_bytes()[:-1], bytes(48)]
        bad = bytearray(module_bytes())
        struct.pack_into("<I", bad, 20, 17)  # zero word count
        invalid.append(bytes(bad))
        bad = bytearray(module_bytes())
        struct.pack_into("<I", bad, 20, (100 << 16) | 17)
        invalid.append(bytes(bad))
        for data in invalid:
            with self.assertRaises(module.ShaderTranslationError):
                module.spirv_metadata(data, "vertex")
        with self.assertRaisesRegex(module.ShaderTranslationError, "stage"):
            module.spirv_metadata(module_bytes(), "pixel")

    def test_complete_pipeline_remains_private_and_does_not_mutate_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            source = repository.private / "private-name.fxc"
            original = fxc(outer_container(), outer_container(flags_byte=1))
            source.write_bytes(original)
            source.chmod(0o640)
            chain = tools(repository)
            output = repository.private / "conversion"
            report = module.translate_batch([source], output,
                                             toolchain=chain, repository=repository.root)
            self.assertEqual(report["status"], "validated")
            self.assertEqual(report["validated_container_count"], 2)
            self.assertFalse(report["gpu_execution"])
            self.assertFalse(report["semantic_equivalence_verified"])
            self.assertNotIn("private-name", json.dumps(report))
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(source.stat().st_mode & 0o777, 0o640)
            self.assertEqual(chain.common_header.stat().st_mode & 0o777, 0o644)
            packet = (output / "validated-modules.bin").read_bytes()
            self.assertEqual(packet[:8], b"XSPVPK1\0")
            self.assertEqual(struct.unpack_from("<II", packet, 8), (1, 2))
            for path in output.rglob("*"):
                self.assertEqual(path.stat().st_mode & 0o777, 0o700 if path.is_dir() else 0o600)
            with self.assertRaises(module.XenosPreflightError):
                module.translate_batch([source], output, toolchain=chain, repository=repository.root)

    def test_compilation_and_validation_failures_do_not_claim_success(self) -> None:
        for compile_success, validate_success, expected in (
            (False, True, "compilation-failed"), (True, False, "validation-failed"),
        ):
            with tempfile.TemporaryDirectory() as temporary:
                repository = PrivateRepository(Path(temporary))
                source = repository.private / "input.xenos"
                source.write_bytes(outer_container())
                chain = tools(repository, compile_success=compile_success,
                              validate_success=validate_success)
                report = module.translate_batch([source], repository.private / "output",
                                                 toolchain=chain, repository=repository.root)
                self.assertEqual(report["status"], "partial")
                self.assertEqual(report["validated_container_count"], 0)
                self.assertEqual(report["records"][0]["status"], expected)
                self.assertFalse((repository.private / "output/validated-modules.bin").exists())

    def test_descriptor_variant_checks_flag_profile_and_64bit_rejection(self) -> None:
        for wide in (False, True):
            with tempfile.TemporaryDirectory() as temporary:
                repository = PrivateRepository(Path(temporary))
                chain = tools(repository)
                code = chain.dxc.read_text().replace("stage = sys.argv", 
                    "assert 'XENOS_RECOMP_DESCRIPTOR_CONSTANTS=1' in sys.argv\nstage = sys.argv")
                if wide:
                    code = code.replace("(2 << 16) | 17, 1,", "(2 << 16) | 17, 11,")
                chain.dxc.write_text(code)
                source = repository.private / "input"
                source.write_bytes(outer_container())
                output = repository.private / "output"
                report = module.translate_batch([source], output, toolchain=chain,
                    repository=repository.root, constant_binding="descriptor32")
                self.assertEqual(report["constant_binding"], "descriptor32")
                self.assertEqual(report["validated_container_count"], 0 if wide else 1)
                if wide:
                    self.assertEqual(report["records"][0]["status"], "invalid-output")
                    self.assertIn("64-bit", report["records"][0]["failure_reason"])
                else:
                    self.assertEqual(struct.unpack_from("<II",
                        (output / "validated-modules.bin").read_bytes(), 8), (2, 1))

    def test_staging_rejects_ambiguous_empty_duplicate_and_outside_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            chain = tools(repository)
            source = repository.private / "input"
            output = repository.private / "output"
            for data in (b"", fxc(), outer_container() + b"trailing"):
                source.write_bytes(data)
                with self.assertRaises((module.ShaderTranslationError, module.FxcInspectError)):
                    module.translate_batch([source], output, toolchain=chain, repository=repository.root)
                self.assertFalse(output.exists())
            source.write_bytes(outer_container())
            for sources in ([source, source], [repository.root / "outside"]):
                with self.assertRaises(module.XenosPreflightError):
                    module.translate_batch(sources, output, toolchain=chain, repository=repository.root)
                self.assertFalse(output.exists())
            source.write_bytes(fxc(outer_container(), outer_container()))
            with self.assertRaises(module.FxcInspectError):
                module.translate_batch([source], output, toolchain=chain,
                                       repository=repository.root, max_containers=1)

    def test_preflight_rejection_never_runs_translator(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            chain = tools(repository)
            chain.preflight.write_text(f"#!{sys.executable}\nprint('XENOS_PREFLIGHT_V1 1 0 1')\nraise SystemExit(1)\n")
            source = repository.private / "input"
            source.write_bytes(outer_container())
            output = repository.private / "output"
            report = module.translate_batch([source], output, toolchain=chain, repository=repository.root)
            self.assertEqual(report["records"][0]["status"], "preflight-failed")
            self.assertFalse((output / "shader-0001/translated.hlsl").exists())

    def test_worker_limits_wall_and_output_and_preserves_redacted_status(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for body, expected in (
                ("import time; time.sleep(5)", "timeout"),
                ("import os; os.write(1, b'x' * 100000)", "output-limit"),
                ("import os,signal; os.kill(os.getpid(), signal.SIGTERM)", "signal"),
            ):
                result = module.run_worker([sys.executable, "-c", body],
                    cwd=Path(temporary), environment=os.environ.copy(), wall_seconds=0.3)
                self.assertEqual(result.status, expected)
                self.assertLessEqual(len(result.output), module.MAX_LOG_BYTES)
            result = module.run_worker([sys.executable, "-c",
                "import resource; print(resource.getrlimit(resource.RLIMIT_CORE)[0]); "
                "print(resource.getrlimit(resource.RLIMIT_CPU)[0])"],
                cwd=Path(temporary), environment=os.environ.copy())
            self.assertEqual(result.output, b"0\n10\n")

    def test_host_without_groups_bounds_wall_time_and_output(self) -> None:
        # (Windows) no process group, no resource limits, no selector on a pipe: the same statuses, the same output bound
        with tempfile.TemporaryDirectory() as temporary:
            for body, expected, wall in (
                ("import time; time.sleep(30)", "timeout", 0.5),
                # (a long wall limit: the worker must be ended when it has written too much, not when its time is up)
                ("import os, time; os.write(1, b'x' * 100000); time.sleep(30)", "output-limit", 8),
                ("import os,signal; os.kill(os.getpid(), signal.SIGTERM)", "signal", 8),
            ):
                started = time.monotonic()
                result = module.run_worker_without_groups([sys.executable, "-c", body],
                    cwd=Path(temporary), environment=os.environ.copy(), wall_seconds=wall)
                self.assertEqual(result.status, expected)
                self.assertLessEqual(len(result.output), module.MAX_LOG_BYTES)
                self.assertLess(time.monotonic() - started, 4, "the worker was ended, not waited for")
            result = module.run_worker_without_groups([sys.executable, "-c",
                "import sys; sys.stdout.write('made'); sys.stdout.flush(); sys.stderr.write(' and said'); sys.exit(3)"],
                cwd=Path(temporary), environment=os.environ.copy())
            self.assertEqual((result.status, result.returncode, result.output), ("exited", 3, b"made and said"))
            marker = Path(temporary) / "input.bin"; marker.write_bytes(b"\x00\r\nfrom the input file\x1a")
            with marker.open("rb") as stream:
                result = module.run_worker_without_groups([sys.executable, "-c",
                    "import sys; sys.stdout.buffer.write(sys.stdin.buffer.read())"],
                    cwd=Path(temporary), environment=os.environ.copy(), input_file=stream)
            self.assertEqual((result.status, result.returncode, result.output), ("exited", 0, marker.read_bytes()))
            for wall in (0, -1, 61):
                with self.assertRaises(module.ShaderTranslationError):
                    module.run_worker_without_groups([sys.executable, "-c", "pass"], cwd=Path(temporary),
                                                     environment=os.environ.copy(), wall_seconds=wall)

    def test_host_decides_which_worker_runs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            arguments = dict(cwd=Path(temporary), environment={"A": "1"}, input_file=None, wall_seconds=7)
            sentinel = module.WorkerResult("exited", 0, b"other host")
            with mock.patch.object(module, "run_worker_without_groups", return_value=sentinel) as other, \
                 mock.patch.object(module.subprocess, "Popen", side_effect=AssertionError("no process group on that host")):
                with mock.patch.object(module.os, "name", "nt"):
                    self.assertIs(module.run_worker(["tool"], **arguments), sentinel)
                other.assert_called_once_with(["tool"], **arguments)
            # this host keeps its process group and its limits
            with mock.patch.object(module, "run_worker_without_groups", side_effect=AssertionError("groups exist here")):
                result = module.run_worker([sys.executable, "-c", "print(1)"], cwd=Path(temporary), environment=os.environ.copy())
            self.assertEqual((result.status, result.output), ("exited", b"1\n"))

    def test_host_name_does_not_refuse_the_translation(self) -> None:
        # the worker for a host without process groups exists, so no host is refused by its name: there the call gets
        # as far as its own first look at the inputs; the bound on the count stays on every host
        with tempfile.TemporaryDirectory() as temporary:
            program = Path(sys.executable)
            toolchain = module.Toolchain(program, program, Path(temporary) / "common.h", program, program)
            arguments = dict(toolchain=toolchain, repository=Path(temporary))
            output = Path(temporary) / "out"                      # (made before the name changes: paths follow os.name)
            with mock.patch.object(module.os, "name", "nt"):
                with self.assertRaisesRegex(ValueError, "at least one private input is required"):
                    module.translate_batch([], output, **arguments)
            for count in (0, 1025):
                with self.assertRaisesRegex(module.ShaderTranslationError, "1..1024 containers"):
                    module.translate_batch([], output, max_containers=count, **arguments)

    def test_host_rule_for_finding_the_compiler_library(self) -> None:
        # this host: the loader's own variable, the PATH untouched
        variable = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
        environment = {"PATH": "/first:/second", "OTHER": "1"}
        module._name_library_directory(environment, "/lib")
        self.assertEqual(environment, {"PATH": "/first:/second", "OTHER": "1", variable: "/lib"})
        # a host without such a variable (Windows): the folder in front of the PATH, nothing else added
        with mock.patch.object(module.os, "name", "nt"):
            for before, after in (({"PATH": r"C:\a;C:\b", "OTHER": "1"}, {"PATH": r"D:\lib;C:\a;C:\b", "OTHER": "1"}),
                                  ({"OTHER": "1"}, {"OTHER": "1", "PATH": r"D:\lib"}), ({"PATH": ""}, {"PATH": r"D:\lib"})):
                module._name_library_directory(before, r"D:\lib")
                self.assertEqual(before, after)
        # and the translation hands that environment to the programs it starts
        seen = {}
        def record(environment, library):
            seen["library"] = library
            raise module.ShaderTranslationError("stop here")
        with tempfile.TemporaryDirectory() as temporary:
            library = Path(temporary) / "lib"; library.mkdir()
            program = Path(sys.executable).resolve()
            header = Path(temporary) / "common.h"; header.write_text("// authored")
            toolchain = module.Toolchain(program, program, header, program, program, library)
            with mock.patch.object(module, "_prepare_inputs", return_value=[]), mock.patch.object(module, "_name_library_directory", record):
                with self.assertRaisesRegex(module.ShaderTranslationError, "stop here"):
                    module.translate_batch([Path(temporary) / "x"], Path(temporary) / "out", toolchain=toolchain, repository=Path(temporary))
            self.assertEqual(seen["library"], str(library.resolve()))

    def test_host_flags_for_reading_an_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "out.spv"; path.write_bytes(b"\x03\x02\x23\x07\r\n\x1a tail")
            real_open, seen = os.open, []
            def spy(target, flags, *more, **named):
                seen.append(flags)
                return real_open(target, flags, *more, **named)
            with mock.patch.object(module.os, "open", spy):
                self.assertEqual(module._artifact(path), path.read_bytes())
            self.assertEqual(seen, [os.O_RDONLY | getattr(os, "O_BINARY", 0) | os.O_NOFOLLOW])
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            # a host with neither O_NOFOLLOW nor fchmod reads the same bytes; a link is refused before it is opened
            fchmod, nofollow = os.fchmod, os.O_NOFOLLOW
            try:
                del module.os.fchmod, module.os.O_NOFOLLOW
                self.assertEqual(module._artifact(path), path.read_bytes())
                link = Path(temporary) / "link.spv"; link.symlink_to(path)
                with self.assertRaises(module.ShaderTranslationError):
                    module._artifact(link)
            finally:
                module.os.fchmod, module.os.O_NOFOLLOW = fchmod, nofollow


if __name__ == "__main__":
    unittest.main()
