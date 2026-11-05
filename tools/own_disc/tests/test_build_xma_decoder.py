#!/usr/bin/env python3
"""build_xma_decoder.sh with stand-ins: a small git repository in the place of the FFmpeg source (its configure and
Makefile write down what `git describe` answers in the build folder), a folder in the place of the NDK. The build
folder is put inside ANOTHER git repository, as the builder's work folder is: the libraries must not take that
repository's revision. The script's pinned revision is replaced by the stand-in's for the test."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(os.environ.get('BUILD_XMA_DECODER', Path(__file__).resolve().parents[2] / 'host_guest_trace' / 'build_xma_decoder.sh'))  # the mutant runner points this at a changed copy
PINNED = '15ece0882e8d5875051ff5b73c5a8326f7cee9f5'
GIT = {'GIT_AUTHOR_NAME': 't', 'GIT_AUTHOR_EMAIL': 't@example.invalid', 'GIT_COMMITTER_NAME': 't', 'GIT_COMMITTER_EMAIL': 't@example.invalid'}


def git(folder, *words):
    env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
    return subprocess.run(['git', '-C', str(folder), *words], check=True, capture_output=True, text=True, env=dict(env, **GIT)).stdout.strip()


class Decoder(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.source = self.root / 'ffmpeg'
        self.source.mkdir()
        (self.source / 'configure').write_text('#!/bin/sh\necho "$@" > configure-words.txt\n')
        (self.source / 'configure').chmod(0o755)
        (self.source / 'Makefile').write_text(
            'libavcodec/libavcodec.a libavutil/libavutil.a:\n'
            '\tmkdir -p $(dir $@)\n'
            '\t(git describe --always 2>/dev/null || echo no-repository) > $@\n')
        git(self.source, 'init', '-q')
        git(self.source, 'add', '.')
        git(self.source, 'commit', '-q', '-m', 'stand-in source')
        self.revision = git(self.source, 'rev-parse', 'HEAD')
        self.ndk = self.root / 'ndk'
        tools = self.ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin'
        tools.mkdir(parents=True)
        (tools / 'aarch64-linux-android26-clang').write_text('#!/bin/sh\n')
        (tools / 'aarch64-linux-android26-clang').chmod(0o755)
        text = SCRIPT.read_text()
        self.assertEqual(text.count(PINNED), 1)                    # the pin itself is not what this test changes
        self.script = self.root / 'build.sh'
        self.script.write_text(text.replace(PINNED, self.revision))
        # the project the build folder lies in: a repository of its own, with a revision of its own
        self.project = self.root / 'project'
        self.project.mkdir()
        (self.project / 'file').write_text('x')
        git(self.project, 'init', '-q')
        git(self.project, 'add', '.')
        git(self.project, 'commit', '-q', '-m', 'the enclosing project')
        self.project_revision = git(self.project, 'rev-parse', '--short', 'HEAD')

    def run_script(self, build):
        env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        return subprocess.run(['sh', str(self.script), str(self.source), str(build), str(self.ndk)], capture_output=True, text=True, env=env)

    def test_inside_another_repository_the_libraries_do_not_take_its_revision(self):
        build = self.project / 'work' / 'xma'
        build.parent.mkdir()
        r = self.run_script(build)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(git(build, 'describe', '--always'), self.project_revision)       # git itself would answer with the project's
        for name in ('libavcodec/libavcodec.a', 'libavutil/libavutil.a'):
            self.assertEqual((build / name).read_text(), 'no-repository\n', name)
        self.assertIn('--enable-decoder=xma1,xma2,xmaframes', (build / 'configure-words.txt').read_text())

    def test_outside_any_repository_the_result_is_the_same(self):
        build = self.root / 'elsewhere' / 'xma'
        build.parent.mkdir()
        r = self.run_script(build)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((build / 'libavcodec/libavcodec.a').read_text(), 'no-repository\n')

    def test_a_source_at_another_revision_is_refused(self):
        (self.source / 'new').write_text('y')
        git(self.source, 'add', '.')
        git(self.source, 'commit', '-q', '-m', 'moved on')
        build = self.root / 'elsewhere' / 'xma'
        build.parent.mkdir()
        r = self.run_script(build)
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((build / 'libavcodec').exists())


if __name__ == '__main__':
    unittest.main()
