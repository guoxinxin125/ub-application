#!/usr/bin/env python3
"""Generate a private single-lane UB read-only config from an existing UB JSON.

Defaults to the independent A/B experiment template, not a machine-numbered config.
No DB restore, service restart, region creation or modification of input JSON.
Client/Timeline run on A; PostStorage and baseline LB/nginx on B. Domain
publisher is a separate B process. DB endpoints change only on explicit override.
"""
import argparse
import copy
import ipaddress
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def host(value):
    # Explicit IPs only: reject ambiguous placeholders and comma-list endpoints.
    address = ipaddress.ip_address(value)
    if address.version != 4:
        raise ValueError('initial UB application integration requires IPv4 addresses')
    return str(address)


def prepare(original, a, b, samples=10, numa_a=0, numa_b=1,
            timeline_db_host=None, timeline_db_port=None,
            storage_db_host=None, storage_db_port=None):
    a, b = host(a), host(b)
    if a == b or not 1 <= samples <= 5000 or min(numa_a, numa_b) < 0:
        raise ValueError('require two different hosts, nonnegative NUMA, 1..5000 samples')
    configs = [copy.deepcopy(original), copy.deepcopy(original)]
    placements = {'client': a, 'user_timeline': a, 'post_storage': b,
                  'nginx': b, 'load_balance': b}
    offsets = {'client': 0, 'user_timeline': 3, 'post_storage': 6,
               'nginx': 0, 'load_balance': 3}
    for c, numa in zip(configs, (numa_a, numa_b)):
        # Unused services remain in the endpoint audit, so assign explicit B
        # addresses too; their database settings and business logic are untouched.
        for name, item in c.items():
            if isinstance(item, dict) and 'server_addr' in item and 'rpc_id' in item:
                old = item['server_addr']
                if not isinstance(old, str) or old.count(':') != 1 or ',' in old:
                    raise ValueError('require a single IPv4:port service endpoint: '+name)
                port = int(old.rsplit(':', 1)[1])
                if not 1 <= port <= 65535:
                    raise ValueError('invalid service port: '+name)
                item['server_addr'] = placements.get(name, b)+':'+str(port)
                if name in offsets: item['bind_core_offset'] = offsets[name]
        common = c['common']
        common.update(client_num=1, server_num=1, concurrency=1,
                      numa_client_node=numa, numa_server_node=numa,
                      test_loop=1, timeout_second=120)
        c['client'].update(generate_num=samples, user_num=1, home_num=0, write_num=0)
        c.pop('domain_proxy', None)  # Explicitly one client and one nginx.
        for key, db_host, db_port in (
                ('user_timeline_mongodb', timeline_db_host, timeline_db_port),
                ('post_storage_mongodb', storage_db_host, storage_db_port)):
            if db_host is not None:
                if not db_host.strip() or any(ch.isspace() for ch in db_host):
                    raise ValueError('invalid DB hostname: '+key)
                c[key]['addr'] = db_host
            if db_port is not None:
                if not 1 <= db_port <= 65535: raise ValueError('invalid DB port: '+key)
                # utils_mongodb.h reads this JSON field as std::string.
                c[key]['port'] = str(db_port)
    # Prevent the inherited configuration from colliding in a live UB region.
    used, addresses = set(), set()
    for name, item in configs[0].items():
        if not isinstance(item, dict) or 'rpc_id' not in item or 'server_addr' not in item: continue
        base = item['rpc_id']
        if type(base) is not int or base < 0 or base+20 >= 64 or base in used or base+20 in used:
            raise ValueError('overlapping/out-of-range rpc_id: '+name)
        used.update((base, base+20))
        if item['server_addr'] in addresses: raise ValueError('duplicate service address')
        addresses.add(item['server_addr'])
    return configs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path,
                        help='optional common source JSON; defaults to bundled independent A/B templates')
    parser.add_argument('--output', required=True, type=Path, help='new private output directory')
    parser.add_argument('--host-a', required=True)
    parser.add_argument('--host-b', required=True)
    parser.add_argument('--numa-a', type=int, default=0)
    parser.add_argument('--numa-b', type=int, default=1)
    parser.add_argument('--samples', type=int, default=10)
    parser.add_argument('--timeline-db-host')
    parser.add_argument('--timeline-db-port', type=int)
    parser.add_argument('--storage-db-host')
    parser.add_argument('--storage-db-port', type=int)
    args = parser.parse_args()
    try:
        sources = ([args.input] * 2 if args.input else
                   [ROOT/'config'/('social-user-read-'+side+'.json') for side in ('a','b')])
        configs = [prepare(json.loads(source.read_text(encoding='utf-8')), args.host_a,
                           args.host_b, args.samples, args.numa_a, args.numa_b,
                           args.timeline_db_host, args.timeline_db_port,
                           args.storage_db_host, args.storage_db_port)[index]
                   for index, source in enumerate(sources)]
        args.output.mkdir(parents=True, exist_ok=False)
        for side, config in zip(('a', 'b'), configs):
            (args.output/('config-'+side+'.json')).write_text(json.dumps(config, indent=2)+'\n', encoding='utf-8')
    except (ValueError, KeyError, OSError) as exc:
        parser.error(str(exc))
    print('UB_SOCIAL_CONFIG_READY scope=user-read client=1 lane=1 concurrency=1 nginx=1 '
          'A=client,user_timeline B=post_storage,lb,nginx,domain-owner database=explicit-overrides-only')


if __name__ == '__main__': main()
