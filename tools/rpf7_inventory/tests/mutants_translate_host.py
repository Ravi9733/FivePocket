#!/usr/bin/env python3
"""Mutant check for the host-neutral parts of xenos_translate_batch.py (the worker for a host without process groups,
the choice between the two workers, the flags for reading an output): each mutant is the tool with one rule changed;
the host tests of test_xenos_translate_batch.py must fail on it. First a control. The tool is changed in place for each
mutant and restored afterwards (the tests import it by name); exits 1 if the control fails, a mutant survives or no
longer applies."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'xenos_translate_batch.py'
MUTANTS = [
    ('Windows gets a loader variable instead of the PATH', "    if os.name == \"nt\":\n        environment[\"PATH\"]", "    if False:\n        environment[\"PATH\"]"),
    ('every host gets the PATH instead of its loader variable', "    if os.name == \"nt\":\n        environment[\"PATH\"]", "    if True:\n        environment[\"PATH\"]"),
    ('on Windows the folder comes after the PATH', "        environment[\"PATH\"] = library + \";\" + environment[\"PATH\"] if environment.get(\"PATH\") else library\n", "        environment[\"PATH\"] = environment[\"PATH\"] + \";\" + library if environment.get(\"PATH\") else library\n"),
    ('on Windows the PATH is replaced', "        environment[\"PATH\"] = library + \";\" + environment[\"PATH\"] if environment.get(\"PATH\") else library\n", "        environment[\"PATH\"] = library\n"),
    ('an empty PATH leaves a stray separator', "        environment[\"PATH\"] = library + \";\" + environment[\"PATH\"] if environment.get(\"PATH\") else library\n", "        environment[\"PATH\"] = library + \";\" + environment.get(\"PATH\", \"\")\n"),
    ('this host gets the other loader variable', "        environment[\"DYLD_LIBRARY_PATH\" if platform.system() == \"Darwin\" else \"LD_LIBRARY_PATH\"] = library\n", "        environment[\"LD_LIBRARY_PATH\" if platform.system() == \"Darwin\" else \"DYLD_LIBRARY_PATH\"] = library\n"),
    ('the translation does not name the library folder', "        _name_library_directory(environment, str(library))\n", "        pass\n"),
    ('a host without process groups is refused by its name', "    if not 1 <= max_containers <= 1024:\n", "    if os.name != \"posix\" or not 1 <= max_containers <= 1024:\n"),
    ('the count of containers has no bound', "    if not 1 <= max_containers <= 1024:\n", "    if False:\n"),
    ('a worker over its time is waited for', "    except subprocess.TimeoutExpired:\n        status = \"timeout\"\n    finally:\n        if process.poll() is None:\n            process.kill()\n", "    except subprocess.TimeoutExpired:\n        status = \"timeout\"\n    finally:\n"),
    ('a timeout is reported as an ordinary end', "    except subprocess.TimeoutExpired:\n        status = \"timeout\"\n    finally:\n        if process.poll() is None:", "    except subprocess.TimeoutExpired:\n        status = \"exited\"\n    finally:\n        if process.poll() is None:"),
    ('a worker that writes without end is not stopped', "                over.set()\n                process.kill()\n                return\n", "                over.set()\n                return\n"),
    ('too much output is not reported', "    if over.is_set():\n        status = \"output-limit\"\n    elif", "    if"),
    ('the output kept is not bounded', "            output.extend(chunk[:MAX_LOG_BYTES + 1 - len(output)])\n            if len(output) > MAX_LOG_BYTES:", "            output.extend(chunk)\n            if False:"),
    ('a worker ended by a signal counts as exited', "    elif status == \"exited\" and process.returncode < 0:\n        status = \"signal\"\n    return WorkerResult(status, process.returncode, bytes(output[:MAX_LOG_BYTES]))\n\n\ndef run_worker(", "    return WorkerResult(status, process.returncode, bytes(output[:MAX_LOG_BYTES]))\n\n\ndef run_worker("),
    ('the error stream is dropped', "        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,\n    )\n    assert process.stdout is not None\n    output = bytearray()\n    over = threading.Event()", "        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,\n    )\n    assert process.stdout is not None\n    output = bytearray()\n    over = threading.Event()"),
    ('the input file is not handed on', "        stdin=input_file if input_file is not None else subprocess.DEVNULL,\n        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,\n    )\n    assert process.stdout is not None\n    output = bytearray()\n    over", "        stdin=subprocess.DEVNULL,\n        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,\n    )\n    assert process.stdout is not None\n    output = bytearray()\n    over"),
    ('any wall limit is accepted on the other host', "    limit stands for the first, and every output file is still refused over MAX_OUTPUT_BYTES when it is read back.\"\"\"\n    if not 0 < wall_seconds <= 60:\n        raise ShaderTranslationError(\"worker wall limit is outside the bounded range\")\n", "    limit stands for the first, and every output file is still refused over MAX_OUTPUT_BYTES when it is read back.\"\"\"\n"),
    ('the other host runs the grouped worker', "    if os.name == \"nt\":\n        return run_worker_without_groups(", "    if False:\n        return run_worker_without_groups("),
    ('this host runs the worker without groups', "    if os.name == \"nt\":\n        return run_worker_without_groups(", "    if True:\n        return run_worker_without_groups("),
    ('the other worker gets another time limit', "                                         input_file=input_file, wall_seconds=wall_seconds)\n", "                                         input_file=input_file)\n"),
    ('an output is opened without the flag this host has', "getattr(os, \"O_BINARY\", 0) | getattr(os, \"O_NOFOLLOW\", 0))", "getattr(os, \"O_BINARY\", 0))"),
    ('an output is opened with a fixed Unix flag', "getattr(os, \"O_BINARY\", 0) | getattr(os, \"O_NOFOLLOW\", 0))", "getattr(os, \"O_BINARY\", 0) | os.O_NOFOLLOW)"),
    ('permissions are set by a call the host may lack', "        if normalize_permissions and hasattr(os, \"fchmod\"):\n", "        if normalize_permissions:\n"),
    ('permissions are never set', "        if normalize_permissions and hasattr(os, \"fchmod\"):\n", "        if False:\n"),
]


def run():
    return subprocess.run([sys.executable, '-B', '-m', 'unittest', '-k', 'host', 'test_xenos_translate_batch'], cwd=HERE, capture_output=True, text=True)


def main() -> int:
    source = TOOL.read_text(encoding='utf-8')
    killed = 0
    try:
        if run().returncode != 0:
            print('CONTROL FAILED: the host tests do not pass on the tool as it is')
            return 1
        print('control: the host tests pass on the tool as it is', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            TOOL.write_text(source.replace(old, new), encoding='utf-8', newline='\n')
            r = run()
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    finally:
        TOOL.write_text(source, encoding='utf-8', newline='\n')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
