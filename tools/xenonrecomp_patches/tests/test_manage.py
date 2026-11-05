from pathlib import Path
import sys
import unittest
from unittest import mock


TOOLS_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS_ROOT / "xenonrecomp_patches"))

import manage  # noqa: E402


class PatchManagerTests(unittest.TestCase):
    def test_reads_xenonrecomp_pin(self) -> None:
        revision = manage.pinned_revision()
        self.assertRegex(revision, r"^[0-9a-f]{40}$")

    def test_patch_queue_is_ordered(self) -> None:
        queue = manage.patches()
        self.assertEqual(queue, sorted(queue))
        self.assertTrue(all(path.suffix == ".patch" for path in queue))

    def test_revision_mismatch_is_rejected(self) -> None:
        with mock.patch.object(manage, "pinned_revision", return_value="a" * 40):
            with mock.patch.object(manage, "current_revision", return_value="b" * 40):
                with self.assertRaisesRegex(RuntimeError, "revision mismatch"):
                    manage.ensure_pinned()

    def test_known_upstream_whitespace_is_suppressed_only_when_reversing(self) -> None:
        patch = Path("0003-bound-pe-section-names.patch")
        with mock.patch.object(manage.subprocess, "run") as run:
            manage.git_apply(("--reverse",), patch)
        run.assert_called_once_with(
            ["git", "apply", "--reverse", "--whitespace=nowarn", str(patch)],
            cwd=manage.SUBMODULE,
            check=True,
        )

        with mock.patch.object(manage.subprocess, "run") as run:
            manage.git_apply((), patch)
        run.assert_called_once_with(
            ["git", "apply", str(patch)],
            cwd=manage.SUBMODULE,
            check=True,
        )

    def test_check_queue_uses_one_temporary_index_in_order(self) -> None:
        queue = [manage.ROOT / "0001.patch", manage.ROOT / "0002.patch"]
        with mock.patch.object(manage.subprocess, "run") as run:
            manage.check_queue(queue)

        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands[0], ["git", "read-tree", "HEAD"])
        self.assertEqual(
            commands[1:],
            [
                ["git", "apply", "--cached", str(queue[0])],
                ["git", "apply", "--cached", str(queue[1])],
            ],
        )
        environments = [call.kwargs["env"] for call in run.call_args_list]
        self.assertEqual(
            len({environment["GIT_INDEX_FILE"] for environment in environments}),
            1,
        )


if __name__ == "__main__":
    unittest.main()
