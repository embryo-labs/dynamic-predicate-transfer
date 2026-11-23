#!/usr/bin/env python3
"""Check complete replay after RPT abandons a partially materialized input.

Use the DuckDB CLI and RPT+ extension from the same build.
Unlike an output-only test, this also requires evidence that abandonment fired.
"""
import argparse
import json
from pathlib import Path
import subprocess

SETUP = """
CREATE TABLE dimension AS SELECT i AS k, i%20 AS flag FROM range(400000) t(i);
CREATE TABLE fact AS SELECT i AS id, i%400000 AS k, repeat('x',60)||i::VARCHAR AS payload
FROM range(1600000) t(i);
CREATE TABLE tiny AS SELECT i AS flag FROM range(20) t(i);
"""
QUERIES = """
SELECT count(*),sum(f.id::HUGEINT),min(f.payload),max(f.payload)
FROM fact f JOIN dimension d USING(k) WHERE d.flag<>0;
SELECT count(*),sum(f.id::HUGEINT)
FROM fact f JOIN dimension d USING(k) JOIN tiny t USING(flag) WHERE t.flag<>0;
SELECT count(*),sum(f.id::HUGEINT) FROM fact f JOIN dimension d USING(k) WHERE d.flag=1;
SELECT count(*) FROM fact f JOIN dimension d USING(k) WHERE d.flag<0;
SELECT count(*),sum(f.id::HUGEINT) FROM fact f LEFT JOIN dimension d ON f.k=d.k AND d.flag<>0;
PREPARE replay AS SELECT count(*),sum(f.id::HUGEINT) FROM fact f JOIN dimension d USING(k) WHERE d.flag>$1;
EXECUTE replay(0);
EXECUTE replay(18);
EXECUTE replay(0);
INSERT INTO dimension VALUES(0,1);
EXECUTE replay(0);
"""

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duckdb', type=Path, required=True)
    parser.add_argument('--extension', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    quote = lambda p: str(p.resolve()).replace("'", "''")
    results = {}
    for threads in [1, 4]:
        reference = None
        for mode in ['native', 'disabled', 'enabled']:
            key = f'{threads}-{mode}'
            profile = args.output / (key + '-profile.json')
            options = f"SET threads={threads}; SET memory_limit='1GB';"
            if mode != 'native':
                options += f"LOAD '{quote(args.extension)}'; SET rpt_dynamic_abandon={'true' if mode == 'enabled' else 'false'};"
            options += SETUP
            options += f"PRAGMA enable_profiling='json'; SET profiling_output='{quote(profile)}';"
            proc = subprocess.run([str(args.duckdb), '-unsigned', '-no-agent', '-batch', '-bail', '-csv', '-noheader'],
                                  input=options + QUERIES, text=True, capture_output=True, timeout=120)
            (args.output / (key + '.stdout')).write_text(proc.stdout)
            (args.output / (key + '.stderr')).write_text(proc.stderr)
            assert proc.returncode == 0, proc.stderr[-4000:]
            if reference is None:
                reference = proc.stdout
            assert proc.stdout == reference, f'Result mismatch: {key}'
            events = [line for line in proc.stderr.splitlines() if line.startswith('RPT_DYNAMIC_ABANDON ')]
            assert bool(events) == (mode == 'enabled'), f'Unexpected abandon events: {key}: {events}'
            results[key] = dict(result_match=True, abandon_events=events)
            print(key, 'PASS', len(events), 'abandon events', flush=True)
    (args.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')

if __name__ == '__main__':
    main()
