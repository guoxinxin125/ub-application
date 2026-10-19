#!/usr/bin/env python3
"""Read-only CPU/affinity preflight before creating any SDK region."""
import argparse
import json
import os
from pathlib import Path


def cpulist(value):
    result = []
    for item in value.strip().split(','):
        ends = item.split('-')
        first, last = int(ends[0]), int(ends[-1])
        if len(ends)>2 or first<0 or last<first: raise ValueError('invalid Linux CPU list')
        result.extend(range(first,last+1))
    return sorted(set(result))


def services_for(config, side, mode):
    if side not in ('A', 'B') or mode not in ('domain', 'rpc'):
        raise ValueError('invalid side/mode')
    address = lambda name: config[name]['server_addr'].rsplit(':', 1)[0]
    a, b = address('client'), address('nginx')
    if a == b or address('user_timeline') != a or address('load_balance') != b or address('post_storage') != b:
        raise ValueError('require A=Client/Timeline, B=PostStorage/nginx/LB in both modes; regenerate config')
    return ['user_timeline', 'client'] if side == 'A' else (
        ['post_storage'] if mode == 'domain' else ['post_storage', 'nginx', 'load_balance'])


def check(config, services, nodes, allowed):
    common, used = config['common'], set()
    if common['client_num']!=1 or common['server_num']!=1:
        raise ValueError('CPU preflight currently requires one lane')
    for name in services:
        base = config[name]['bind_core_offset']
        if type(base) is not int or base<0: raise ValueError('invalid CPU offset: '+name)
        for role, offset in (('client',0),('server',1),('worker',2)):
            node = common['numa_client_node' if role=='client' else 'numa_server_node']
            cores = nodes[node]
            # Storage has no worker thread; reserve its block conservatively.
            if base+offset>=len(cores) or cores[base+offset] not in allowed:
                raise ValueError(f'{name}/{role}: NUMA{node} index {base+offset} is not allowed; '
                                 f'adjust {name}.bind_core_offset in the private config')
            cpu = cores[base+offset]
            if cpu in used: raise ValueError('overlapping service CPU blocks: '+str(cpu))
            used.add(cpu)
    return sorted(used)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',required=True,type=Path)
    parser.add_argument('--services',nargs='+')
    parser.add_argument('--side', choices=('A','B'))
    parser.add_argument('--mode', choices=('domain','rpc'))
    parser.add_argument('--print-services', action='store_true')
    args = parser.parse_args()
    try:
        config = json.loads(args.config.read_text(encoding='utf-8'))
        services = args.services or services_for(config, args.side, args.mode)
        if args.print_services:
            print(' '.join(services))
            return
        nodes = {node:cpulist(Path(f'/sys/devices/system/node/node{node}/cpulist').read_text())
                 for node in (config['common']['numa_client_node'],config['common']['numa_server_node'])}
        cores = check(config,services,nodes,set(os.sched_getaffinity(0)))
    except (OSError,ValueError,KeyError,AttributeError) as exc: parser.error(str(exc))
    print('UB_SOCIAL_CPU_PREFLIGHT_PASS cpus='+','.join(map(str,cores)))


if __name__=='__main__': main()
