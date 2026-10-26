#!/usr/bin/env python3
"""Compare two completed multi-process SocialNetwork read campaigns.

Per-client percentiles are reported separately. Only the weighted mean is
pooled; percentiles cannot be merged from percentile summaries alone.
"""
import argparse
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('check_social', Path(__file__).with_name('check-social.py'))
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def weighted_mean(result):
    return sum(float(row['avg_ns'])*int(row['measured']) for row in result['_reports'])/result['all']


def change(before, after):
    return (before-after)*100/before


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('rpc_a','rpc_b','domain_a','domain_b'):
        parser.add_argument('--'+name.replace('_','-'), required=True, type=Path)
    parser.add_argument('--workload', choices=('user','home'), required=True)
    parser.add_argument('--clients', type=int, required=True)
    parser.add_argument('--samples', type=int, required=True, help='requests per client, including warmup')
    parser.add_argument('--chain', choices=('fused','legacy'), default='fused')
    args = parser.parse_args()
    try:
        rpc = checker.check(args.rpc_a, 'rpc', args.samples, args.rpc_b, 'legacy', args.workload, args.clients)
        domain = checker.check(args.domain_a, 'domain', args.samples, args.domain_b,
                               args.chain, args.workload, args.clients)
        if (rpc['all'],rpc['empty'],rpc['nonempty']) != (domain['all'],domain['empty'],domain['nonempty']):
            raise ValueError('result classes differ between RPC and domain; not a matched workload')
        if any('warmup' not in row or any(key not in row for key in ('p50_ns','p99_ns','avg_ns'))
               for row in rpc['_reports']+domain['_reports']):
            raise ValueError('missing latency or warmup fields')
        if len({row['warmup'] for row in rpc['_reports']+domain['_reports']}) != 1:
            raise ValueError('warmup differs between campaigns')
        for index,(r,d) in enumerate(zip(rpc['_reports'],domain['_reports']),1):
            print(f'UB_SOCIAL_CLIENT_COMPARE source={index} workload={args.workload} '
                  f'rpc_p50_ns={r["p50_ns"]} domain_p50_ns={d["p50_ns"]} '
                  f'rpc_p99_ns={r["p99_ns"]} domain_p99_ns={d["p99_ns"]} '
                  f'rpc_avg_ns={r["avg_ns"]} domain_avg_ns={d["avg_ns"]} '
                  f'avg_reduction_pct={change(float(r["avg_ns"]),float(d["avg_ns"])):.2f}')
        r_mean,d_mean = weighted_mean(rpc),weighted_mean(domain)
        print(f'UB_SOCIAL_COMPARE_PASS workload={args.workload} clients={args.clients} '
              f'measured={rpc["all"]} warmup={rpc["_reports"][0]["warmup"]} '
              f'rpc_weighted_avg_ns={r_mean:.1f} domain_weighted_avg_ns={d_mean:.1f} '
              f'avg_reduction_pct={change(r_mean,d_mean):.2f} '
              'pooled_p50_p99=not_available scope=call-to-consume')
        if args.clients>1:
            r_rate=args.clients*args.samples*1e9/rpc['_wall_ns']
            d_rate=args.clients*args.samples*1e9/domain['_wall_ns']
            print(f'UB_SOCIAL_THROUGHPUT rpc_ops_s={r_rate:.1f} domain_ops_s={d_rate:.1f} '
                  f'change_pct={(d_rate-r_rate)*100/r_rate:.2f} scope=campaign-wall')
    except (OSError, ValueError, KeyError, ZeroDivisionError) as exc:
        parser.error(str(exc))


if __name__ == '__main__': main()
