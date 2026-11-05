"""Run one source mutant per stage check; copies and logs stay in an explicit private output."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

MUTANTS = [
 ('toolchain-folders', "getattr(args, 'host_tools', None), getattr(args, 'ndk', None))", "None, None)", 'test_the_toolchain_named_from_the_two_folders_is_what_the_translation_takes'),
 ('toolchain-one-folder', "getattr(args, 'host_tools', None), getattr(args, 'ndk', None))", "getattr(args, 'host_tools', None), getattr(args, 'host_tools', None))", 'test_the_toolchain_named_from_the_two_folders_is_what_the_translation_takes'),
 ('toolchain-file', "shader_toolchain.load(getattr(args, 'toolchain', None),", "shader_toolchain.load(None,", 'test_workers_capped_at_four'),
 ('toolchain-fields', "    required = shader_toolchain.BASE\n", "    required = tuple(config)\n", 'test_the_toolchain_named_from_the_two_folders_is_what_the_translation_takes'),
 ('toolchain-cli', "    for name in ('toolchain', 'host-tools', 'ndk'):", "    for name in ('toolchain',):", 'test_the_toolchain_named_from_the_two_folders_is_what_the_translation_takes'),
 ('name-rule', "    return path.relative_to(base).as_posix()", "    return str(path.relative_to(base))", 'test_recorded_names_have_forward_slashes_on_every_host'),
 ('name-effect', "file=record_name(fxc_path, out))", "file=str(fxc_path.relative_to(out)))", 'test_every_recorded_name_is_made_by_the_one_rule'),
 ('name-container', "entry['file'] = record_name(dest, out)", "entry['file'] = str(dest.relative_to(out))", 'test_every_recorded_name_is_made_by_the_one_rule'),
 ('name-package', "file=record_name(dest, out), bytes=size)", "file=str(dest.relative_to(out)), bytes=size)", 'test_every_recorded_name_is_made_by_the_one_rule'),
 ('name-record', "record=record_name(directory / f\"shader-{rec['index']:04d}\", args.output)", "record=str((directory / f\"shader-{rec['index']:04d}\").relative_to(args.output))", 'test_every_recorded_name_is_made_by_the_one_rule'),
 ('duplicate', "if f'{key:016x}' in result or", "if key in result or", 'test_pack_duplicate_keys_refused'),
 ('worker-reason', "print('SHADER_WORKER_REASON', type(error).__name__, str(error), file=sys.stderr, flush=True)", "pass", 'test_worker_preserves_project_cli_failure_reason'),
 ('workers', 'if not 1 <= jobs <= 4:', 'if not 1 <= jobs <= 8:', 'test_workers_capped_at_four'),
 ('producer', "if sha((record_dir / 'container.xenos').read_bytes()) != original['sha256']:", "if False:", 'test_translation_rejects_changed_producer_container'),
 ('connection', "entry['file'] = record_name(dest, out)", "entry['file'] = 'wrong.xenos'", 'test_connected_disc_archive_container_runtime_key_and_pack'),
 ('extent', "if len(data) != size:", "if False:", 'test_bounded_view_refuses_overread_and_short_read'),
 ('identity', "len(blob) != r['bytes'] or sha(blob) != r['sha256'] or r['sha256'] in seen", "r['sha256'] in seen", 'test_source_identity_is_checked'),
 ('key', "r.get('key') != key or r.get('canonical_sha256') != sha(canon)", "r.get('canonical_sha256') != sha(canon)", 'test_runtime_key_is_checked'),
 ('comparison', "actual[key] == expected[key]", "True", 'test_pack_comparison_includes_metadata_and_modules'),
 ('pack-bounds', "offset < end or offset + words*4 > len(data)", "False", 'test_pack_bounds_refuse_truncation'),
 ('trailer', "if cursor != len(data):", "if False:", 'test_pack_trailing_bytes_refused'),
 ('nice', "if count > 8 and host_support.effective_nice() <= 0:", "if count > 10000 and host_support.effective_nice() <= 0:", 'test_bulk_requires_effective_nice'),
 ('nice-posix-only', "if count > 8 and host_support.effective_nice() <= 0:", "if count > 8 and hasattr(os, 'getpriority') and os.getpriority(os.PRIO_PROCESS, 0) <= 0:", 'test_bulk_requires_effective_nice'),
 ('theme', "if content_type in (0x20000, 0x30000):", "if content_type in (0x20000,):", 'test_dashboard_exclusion_does_not_skip_unknown_game_content'),
 ('overwrite', "with path.open('x', encoding='utf-8', newline='\\n') as stream:", "with path.open('w', encoding='utf-8', newline='\\n') as stream:", 'test_output_never_overwrites_evidence'),
 ('unresolved-default', "(getattr(args, 'unresolved_ok', False) and not failures", "(True and not failures", 'test_unresolved_effects_fail_the_translation_unless_coverage_is_left_to_selection'),
 ('unresolved-failures', "'unresolved_ok', False) and not failures and args.limit is None) else 1", "'unresolved_ok', False) and args.limit is None) else 1", 'test_a_failed_translation_fails_the_stage_whatever_the_flag'),
 ('packages-recorded', "                    manifest['packages'].append(dict(origin=origin, file=record_name(dest, out), bytes=size))\n", "", 'test_extract_records_the_title_packages_it_keeps'),
 ('select-missing', "or {r['key'] for r in chosen} != wanted:", "or False:", 'test_select_refuses_a_key_the_translation_does_not_hold'),
 ('select-status', "if any(r['status'] != 'validated' for r in chosen) or", "if", 'test_select_refuses_a_key_whose_translation_was_not_validated'),
 ('select-order', "for k in keys) or keys != sorted(set(keys)):", "for k in keys):", 'test_select_refuses_key_lists_of_another_shape'),
 ('select-schema', "listed.get('schema') != 'shader-keys/1' or ", "", 'test_select_refuses_key_lists_of_another_shape'),
 ('select-shape', "or not all(isinstance(k, str) for k in keys) or keys != sorted(set(keys)):", "or keys != sorted(set(keys)):", 'test_select_refuses_key_lists_of_another_shape'),
 ('select-all', "chosen = [r for r in json.loads(report_bytes)['records'] if r['key'] in wanted]", "chosen = [r for r in json.loads(report_bytes)['records']]", 'test_select_writes_exactly_the_listed_keys_with_the_production_packer'),
 ('select-exact', "if set(read_pack(data)) != wanted:", "if False:", 'test_select_refuses_a_pack_that_lost_a_listed_key'),
 ('select-expected', "same = args.expect_sha256 in (None, digest)", "same = True", 'test_select_reports_a_pack_that_is_not_the_expected_one'),
 ('select-reuse', "    args.output.mkdir(parents=True)\n    for r in chosen:", "    args.output.mkdir(parents=True, exist_ok=True)\n    for r in chosen:", 'test_select_never_writes_into_an_existing_output'),
]


def main():
 p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--tests',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 a.output.mkdir(parents=True,exist_ok=False);original=a.source.read_text();rows=[]
 # control: an unchanged copy passes every test a mutant is judged by (else a mutant would be caught for being a copy)
 same=a.output/'unchanged.py';same.write_text(original)
 env=os.environ.copy();env['SHADER_TEST_MODULE']=str(same.resolve());env['PYTHONDONTWRITEBYTECODE']='1';env['PYTHONPATH']=str(a.source.resolve().parent)
 control=subprocess.run([sys.executable,str(a.tests),*sorted({'Checks.'+m[3] for m in MUTANTS})],env=env,capture_output=True)
 (a.output/'unchanged.log').write_bytes(control.stdout+control.stderr)
 if control.returncode:
  print('CONTROL FAILED: an unchanged copy does not pass',flush=True);return 1
 print('CONTROL an unchanged copy passes',len({m[3] for m in MUTANTS}),'tests',flush=True)
 for name,old,new,test in MUTANTS:
  if original.count(old)!=1:raise ValueError('mutant anchor mismatch: '+name)
  module=a.output/(name+'.py');module.write_text(original.replace(old,new))
  env=os.environ.copy();env['SHADER_TEST_MODULE']=str(module.resolve());env['PYTHONDONTWRITEBYTECODE']='1'
  env['PYTHONPATH']=str(a.source.resolve().parent)   # a copy started as a program still finds the tool's own helpers
  run=subprocess.run([sys.executable,str(a.tests),'Checks.'+test],env=env,capture_output=True)
  (a.output/(name+'.log')).write_bytes(run.stdout+run.stderr)
  caught=run.returncode!=0 and (b'FAILED (' in run.stderr)
  rows.append(dict(name=name,test=test,caught=caught,returncode=run.returncode))
  print('MUTANT',name,'CAUGHT' if caught else 'SURVIVED',flush=True)
 (a.output/'report.json').write_text(json.dumps(rows,indent=2)+'\n')
 return 0 if all(r['caught'] for r in rows) else 1

if __name__=='__main__':raise SystemExit(main())
