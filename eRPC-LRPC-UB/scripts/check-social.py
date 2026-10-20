#!/usr/bin/env python3
"""Validate stopped A and B application logs; never claim kernel/UB health."""
import argparse
from pathlib import Path
import re


def rows(text, marker):
    return [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines()
            if line.startswith(marker+' ')]


def check(logdir, mode, samples, peer_logs=None, chain='legacy'):
    if peer_logs is None:
        raise ValueError('require B --peer-logs; PostStorage is on B, not A')
    paths = {name:logdir for name in ('client','user_timeline')}
    paths['post_storage'] = peer_logs
    if mode == 'rpc':
        paths.update(nginx=peer_logs, load_balance=peer_logs)
    logs = {name:(directory/(name+'.log')).read_text(encoding='utf-8',errors='replace')
            for name,directory in paths.items()}
    for name,text in logs.items():
        if re.search(r'SN_DOMAIN_ERROR|SN_DOMAIN_CONFIG_ERROR|SN_MONGODB_ERROR|UB_DOMAIN_\w*FAIL|terminate called|Assertion.*failed',text):
            raise ValueError('failure in '+name+'.log')
    client = logs['client']
    result, measurement = rows(client,'SN_DOMAIN_E2E'), rows(client,'SN_DOMAIN_MEASUREMENT')
    if len(result)!=1 or len(measurement)!=1:
        raise ValueError('require exactly one completed campaign and measurement report')
    r,m = result[0],measurement[0]
    if (int(r['completed'])!=samples or int(r['expected'])!=samples or
        r['clock']!='steady_ns' or r['scope']!='call-to-consume' or r['fixture']!='0' or
        int(m['total'])!=samples or int(m['measured'])!=int(r['measured']) or
        int(r['measured'])<=0):
        raise ValueError('incomplete/mismatched campaign or wrong measurement scope')
    classes = rows(client,'SN_DOMAIN_CLASS')
    measured = {row['class']:int(row['count']) for row in classes if row['phase']=='measured'}
    if measured.get('all')!=int(r['measured']) or measured.get('empty',0)+measured.get('nonempty',0)!=measured['all']:
        raise ValueError('missing/inconsistent measured result classes')
    ready = []
    if chain not in ('legacy', 'fused') or (mode == 'rpc' and chain != 'legacy'):
        raise ValueError('invalid chain for mode')
    for name in ('client','user_timeline'):
        text = logs[name]
        if mode=='domain':
            found = rows(text,'SN_DOMAIN_READY')
            done = rows(text,'SN_DOMAIN_DONE')
            if (len(found)!=1 or found[0].get('backend')!='ub' or
                found[0].get('chain')!=chain or
                found[0].get('transitions')!=('3' if chain=='fused' else '4') or
                len(done)!=1 or done[0].get('backend')!='ub' or int(done[0]['calls'])!=samples):
                raise ValueError('require UB domain readiness, call count and clean close in '+name)
            ready.append(found[0])
        else:
            if rows(text,'SN_DOMAIN_READY') or not any(row.get('enabled')=='0'
                for row in rows(text,'SN_DOMAIN_CONFIG')):
                raise ValueError('RPC baseline did not use the original RPC path in '+name)
    if ready and (ready[0]['slot']==ready[1]['slot'] or ready[0]['identity']==ready[1]['identity']):
        raise ValueError('caller and reverse executor connections are not isolated')
    return measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--logs',required=True,type=Path)
    parser.add_argument('--peer-logs',required=True,type=Path,help='local directory containing this run\'s B logs')
    parser.add_argument('--mode',required=True,choices=('domain','rpc'))
    parser.add_argument('--samples',required=True,type=int)
    parser.add_argument('--chain',choices=('legacy','fused'),default='legacy',
                        help='expected application domain chain; default legacy')
    args = parser.parse_args()
    try: counts = check(args.logs,args.mode,args.samples,args.peer_logs,args.chain)
    except (OSError,ValueError,KeyError) as exc: parser.error(str(exc))
    print(f'UB_SOCIAL_E2E_PASS mode={args.mode} chain={args.chain if args.mode=="domain" else "rpc"} samples={args.samples} '
          f'measured={counts["all"]} nonempty={counts["nonempty"]} empty={counts["empty"]} '
          'scope=application-logs kernel_health_checked=0')
    if not counts['nonempty']:
        print('WARNING: all measured results were empty; this does not validate nonempty post consumption.')


if __name__=='__main__': main()
