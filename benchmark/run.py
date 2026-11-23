"""Compare native DuckDB and RPT+ using the shell built by make release."""
import argparse
import csv
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUCKDB = ROOT / "duckdb"
CLI = ROOT / "build/release/duckdb"
EXT = ROOT / "build/release/extension/rpt/rpt.duckdb_extension"
# Database name, bundled query directory (relative to DuckDB), full query count
SUITES = {
    "job": ("imdb", "benchmark/imdb_plan_cost/queries", 113),
    "stats-ceb": ("stats-ceb", None, 146),
    "tpch-sf10": ("tpch-sf10", "extension/tpch/dbgen/queries", 22),
    "tpch-sf100": ("tpch-sf100", "extension/tpch/dbgen/queries", 22),
    "tpcds-sf10": ("tpcds-sf10", "extension/tpcds/dsdgen/queries", 99),
    "ceb-imdb": ("imdb", None, 13646),
    "appian": ("appian", "benchmark/appian_benchmarks/queries", 8),
    "ceb-stack": ("ceb-stack", None, 6191),
    "sqlstorm": ("sqlstorm", None, 18251),
}


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def prepare(database, settings):
    if database.is_file():
        return
    name = database.stem
    if name == "imdb":
        sql = "INSTALL httpfs; LOAD httpfs;\n" + (DUCKDB / "benchmark/imdb/init/load.sql").read_text()
    elif name == "appian":
        template = (DUCKDB / "benchmark/appian_benchmarks/appian.benchmark.in").read_text()
        sql = "INSTALL httpfs;\n" + template.split("\nload\n", 1)[1].split("\nrun ", 1)[0]
    else:
        family, scale = name.split("-sf")
        generator = "dbgen" if family == "tpch" else "dsdgen"
        sql = f"INSTALL {family}; LOAD {family}; CALL {generator}(sf={int(scale)});"
    print(f"Preparing {database}", flush=True)
    database.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=database.parent) as tmp:
        staging = Path(tmp) / database.name
        subprocess.run([CLI, "-no-agent", "-no-init", "-batch", "-bail", staging],
                       input=settings + sql, text=True, check=True)
        staging.replace(database)


def measure(database, query, settings, timeout):
    filename = query.name.replace("\\", "\\\\").replace('"', '\\"')
    read = f'.read "{filename}"\n'
    script = settings + ".mode trash\n" + read + ".timer on 9\n" + read
    try:
        proc = subprocess.run([CLI, "-readonly", "-unsigned", "-no-agent", "-no-init",
                               "-batch", "-bail", database], cwd=query.parent, input=script,
                              capture_output=True, text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return "timeout", "", ""
    times = re.findall(r"^Run Time \(s\): real ([\d.]+)", proc.stdout, re.M)
    if proc.returncode or len(times) != 1:
        return "error", "", proc.stderr.strip() or f"expected one timed statement, found {len(times)}"
    return "ok", times[0], ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=ROOT / "build/benchmarks")
    parser.add_argument("--output", type=Path, default=Path("timings.csv"))
    parser.add_argument("--suite", nargs="+", choices=SUITES, default=list(SUITES))
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--memory", default="128GB")
    parser.add_argument("--timeout", type=int, default=150, help="seconds per warmup + timed pair")
    args = parser.parse_args()
    if args.threads < 1 or args.timeout < 1:
        parser.error("threads and timeout must be positive")
    if not CLI.is_file() or not EXT.is_file():
        parser.error("run make release first")
    settings = f"SET threads={args.threads}; SET memory_limit={literal(args.memory)};\n"
    data = args.data.resolve()
    workloads = []
    for suite in dict.fromkeys(args.suite):
        name, bundled, count = SUITES[suite]
        directory = DUCKDB / bundled if bundled else data / "queries" / suite
        queries = sorted(directory.rglob("*.sql"))
        if len(queries) != count:
            parser.error(f"{suite}: expected {count} SQL files in {directory}, found {len(queries)}")
        database = data / (name + ".duckdb")
        if name in {"stats-ceb", "ceb-stack", "sqlstorm"} and not database.is_file():
            parser.error(f"{suite}: missing database {database}")
        workloads.append((suite, database, directory, queries))
    if args.output.exists():
        parser.error(f"{args.output} already exists; choose a fresh --output")
    for database in dict.fromkeys(entry[1] for entry in workloads):
        prepare(database, settings)
    failed = False
    with args.output.open("x", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["suite", "query", "engine", "threads", "memory", "status", "seconds", "error"])
        for suite, database, directory, queries in workloads:
            print(f"{suite}: {len(queries)} queries", flush=True)
            rpt = f"LOAD {literal(EXT)}; SET rpt_enabled=true;\n"
            if suite == "sqlstorm":
                rpt += "SET disabled_optimizers='distinct_aggregate_rewrite';\n"
            modes = [("duckdb", ""), ("rpt", rpt)]
            for i, query in enumerate(queries):
                for mode, init in (modes if i % 2 == 0 else reversed(modes)):
                    status, seconds, error = measure(database, query, settings + init, args.timeout)
                    writer.writerow([suite, query.relative_to(directory), mode, args.threads,
                                     args.memory, status, seconds, error])
                    out.flush()
                    failed |= status != "ok"
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
