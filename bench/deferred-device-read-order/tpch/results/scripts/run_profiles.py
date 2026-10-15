import argparse
from pathlib import Path

import yaml

from run_matrix import OUT, run_one


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binaries', type=Path, required=True)
    parser.add_argument('--phase', choices=['quent', 'nsys'], required=True)
    parser.add_argument('--query', type=int, choices=[1, 6, 14, 19])
    args = parser.parse_args()
    queries = [args.query] if args.query else [1, 6, 14, 19]
    if args.phase == 'quent':
        for policy in ['siblings_first', 'own_first']:
            for q in queries:
                identity = f'prefetch8-{policy}-q{q}'
                config = yaml.safe_load((OUT / 'prefetch8.yaml').read_text())
                config['sirius']['telemetry']['enable_quent'] = True
                config['sirius']['telemetry']['output_directory'] = str(OUT / 'quent' / (identity + '-events'))
                config['sirius']['telemetry']['engine_name'] = identity
                path = OUT / 'profile-configs' / (identity + '.yaml')
                path.parent.mkdir(exist_ok=True)
                path.write_text(yaml.safe_dump(config, sort_keys=False))
                run_one(args.binaries / ('tpch_' + policy), path, q, OUT / 'quent' / identity, 1, 3)
    else:
        # One real CUDA/NVTX trace per query and cache regime for the production
        # ordering. Timings and diagnostic counters provide the paired comparison.
        for q in queries:
            identity = f'prefetch8-siblings_first-q{q}'
            report = OUT / 'nsys' / 'profiles' / identity
            report.parent.mkdir(parents=True, exist_ok=True)
            prefix = ['nsys', 'profile', '--trace=cuda,nvtx', '--sample=none', '--cpuctxsw=none',
                      '--cudabacktrace=none', '--capture-range=cudaProfilerApi',
                      '--capture-range-end=repeat:5', '--export=sqlite', '--output', str(report)]
            run_one(args.binaries / 'tpch_siblings_first', OUT / 'prefetch8.yaml', q,
                    OUT / 'nsys' / identity, 1, 3, ['--profile'], prefix)


if __name__ == '__main__':
    main()
