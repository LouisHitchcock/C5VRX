#!/usr/bin/env python3
"""Regression checks for immutable alpha releases after git history rewrites."""
from pathlib import Path
import unittest
from unittest.mock import patch

from c5vrx4_version import published_version, resolve_version
from firmware_input_hash import firmware_input


TAG = 'c5vrx4-v4.0.0-alpha.3'
NEXT = 'c5vrx4-v4.0.0-alpha.4'


class PublishedVersionTests(unittest.TestCase):
    def resolve(self, tags=(TAG,), same=(TAG,), published=(TAG,),
                commit='old-commit', inputs='old-inputs'):
        def download(command, check):
            self.assertTrue(check)
            previous = Path(command[command.index('--dir') + 1])
            (previous / 'COMMIT').write_text(commit + '\n')
            (previous / 'FIRMWARE_INPUT_SHA').write_text(inputs + '\n')

        with patch('c5vrx4_version.subprocess.check_output',
                   return_value='\n'.join(published)), \
             patch('c5vrx4_version.subprocess.run', side_effect=download):
            return published_version(tags, same, 'new-commit', 'new-inputs',
                                     'Twotoz/C5VRX')['tag']

    def test_rewritten_tag_with_different_inputs_gets_new_version(self):
        self.assertEqual(self.resolve(), NEXT)

    def test_rewritten_commit_with_identical_inputs_reuses_release(self):
        self.assertEqual(self.resolve(inputs='new-inputs'), TAG)

    def test_exact_commit_rerun_reuses_release(self):
        self.assertEqual(self.resolve(commit='new-commit'), TAG)

    def test_release_without_local_tag_still_reserves_version(self):
        self.assertEqual(self.resolve(tags=(), same=()), NEXT)

    def test_unpublished_tag_can_be_published(self):
        self.assertEqual(self.resolve(published=()), TAG)

    def test_fresh_repository_starts_at_alpha_one(self):
        self.assertEqual(self.resolve(tags=(), same=(), published=()),
                         'c5vrx4-v4.0.0-alpha.1')

    def test_unrelated_tags_do_not_affect_alpha_sequence(self):
        self.assertEqual(resolve_version(['v3.27.0', 'c5vrx4-pr-164', TAG])['tag'], NEXT)


class FirmwareInputTests(unittest.TestCase):
    def test_v4_relocation_covers_each_build_input(self):
        for path in ('v4/main/video.c', 'v4/firmware/pipeline.c',
                     'v4/firmware/include/c5vrx4.h',
                     'v4/firmware/programs/c5vrx4_phase8_static.bsasm',
                     'v4/tools/verify.py', 'v4/component.cmake',
                     'v4/sdkconfig.defaults', '.github/workflows/c5vrx4.yml'):
            with self.subTest(path=path):
                self.assertTrue(firmware_input(path, '4'))

    def test_generations_do_not_hash_each_others_sources(self):
        self.assertTrue(firmware_input('main/video.c', '3'))
        self.assertFalse(firmware_input('main/video.c', '4'))
        self.assertFalse(firmware_input('v4/main/video.c', '3'))
        self.assertFalse(firmware_input('experiments/c5vrx-4/main/video.c', '4'))

    def test_documentation_and_website_do_not_change_firmware_hash(self):
        for path in ('v4/README.md', 'v4/docs/DESIGN.md', 'web/app.js'):
            for generation in ('3', '4'):
                with self.subTest(path=path, generation=generation):
                    self.assertFalse(firmware_input(path, generation))


if __name__ == '__main__':
    unittest.main()
