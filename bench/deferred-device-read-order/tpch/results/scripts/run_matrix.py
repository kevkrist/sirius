"""Serialize and record a fixed TPC-H ordering experiment; run inside pixi."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

OUT = Path('/tmp/tpch-deferred-order-sf100-20261015')
SEQUENCE = ['siblings_first', 'own_first', 'own_first', 'siblings_first']


def run_one(binary, config, q, target, fresh, retained, flags=(), prefix=()):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        raise RuntimeError(f'Refusing overwrite: {target}')
    command = [*prefix, str(binary), str(OUT / 'workloads' / f'q{q}'), str(target), str(fresh), str(retained), *flags]
    env = os.environ.copy()
    env.update(SIRIUS_CONFIG_FILE=str(config), CUDA_VISIBLE_DEVICES='0')
    start = time.time()
    with open('/tmp/tpch-deferred-order-gpu.lock', 'a') as lease:
        fcntl.flock(lease, fcntl.LOCK_EX)
        for idle_sample in range(3):
            processes = subprocess.check_output(['nvidia-smi', '--query-compute-apps=pid,process_name', '--format=csv,noheader'], text=True).strip()
            utilization = subprocess.check_output(['nvidia-smi', '--id=0', '--query-gpu=utilization.gpu', '--format=csv,noheader,nounits'], text=True).strip()
            if processes or int(utilization) != 0:
                raise RuntimeError(f'GPU not idle: processes={processes}, utilization={utilization}')
            if idle_sample < 2:
                time.sleep(0.5)
        before = subprocess.check_output(['nvidia-smi', '--query-gpu=index,name,memory.used,utilization.gpu,temperature.gpu,clocks.sm,clocks.mem', '--format=csv'], text=True)
        error = None
        try:
            with target.with_suffix('.log').open('w') as log:
                completed = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600)
            returncode = completed.returncode
        except (OSError, subprocess.TimeoutExpired) as failure:
            error = str(failure)
            returncode = 124 if isinstance(failure, subprocess.TimeoutExpired) else 127
        after = subprocess.check_output(['nvidia-smi', '--query-gpu=index,memory.used,utilization.gpu,temperature.gpu,clocks.sm,clocks.mem', '--format=csv'], text=True)
    record = {'command': command, 'config': str(config), 'config_sha256': hashlib.sha256(config.read_bytes()).hexdigest(),
              'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'start_unix': start, 'end_unix': time.time(), 'returncode': returncode, 'error': error,
              'before': before, 'after': after, 'env_overrides': {'SIRIUS_CONFIG_FILE': str(config), 'CUDA_VISIBLE_DEVICES': '0'}}
    target.with_suffix('.run.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'{target.name}: exit={returncode} elapsed={time.time()-start:.1f}s', flush=True)
    if returncode:
        raise RuntimeError(f'Run failed: {target.with_suffix(".log")}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binaries', type=Path, required=True)
    parser.add_argument('--phase', choices=['smoke', 'cpu', 'diagnostic', 'timing'], required=True)
    parser.add_argument('--query', type=int, choices=[1, 6, 14, 19])
    parser.add_argument('--tag', default='')
    args = parser.parse_args()
    queries = [args.query] if args.query else [1, 6, 14, 19]
    configs = [('default', OUT / 'config.yaml'), ('prefetch8', OUT / 'prefetch8.yaml')]
    if args.phase == 'smoke':
        run_one(args.binaries / 'tpch_siblings_first', configs[0][1], args.query or 6, OUT / 'smoke' / f'q{args.query or 6}{args.tag}', 0, 0)
    elif args.phase == 'cpu':
        for q in queries:
            run_one(args.binaries / 'tpch_siblings_first', configs[0][1], q, OUT / 'cpu' / f'q{q}', 0, 0, ['--cpu'])
    elif args.phase == 'diagnostic':
        for name, config in configs:
            for policy in ['siblings_first', 'own_first']:
                for q in queries:
                    run_one(args.binaries / f'tpch_{policy}_diagnostic', config, q,
                            OUT / 'diagnostic' / f'{name}-{policy}-q{q}', 3, 3)
    else:
        for name, config in configs:
            for block, policy in enumerate(SEQUENCE):
                for q in queries:
                    run_one(args.binaries / f'tpch_{policy}', config, q,
                            OUT / 'timing' / f'{name}-b{block}-{policy}-q{q}', 3, 3)


if __name__ == '__main__':
    main()
