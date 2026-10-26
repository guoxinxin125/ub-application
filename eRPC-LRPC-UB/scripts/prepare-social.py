#!/usr/bin/env python3
"""Generate private UB read-only configs with 1..4 independent client processes.

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


def cpu_list(value):
    if not isinstance(value, str) or not value.strip():
        raise ValueError('CPU list must be a nonempty string')
    result = set()
    for item in value.split(','):
        parts = item.strip().split('-')
        if len(parts) not in (1, 2) or any(not p.isdecimal() for p in parts):
            raise ValueError('invalid CPU list: '+value)
        first, last = int(parts[0]), int(parts[-1])
        if last < first:
            raise ValueError('invalid CPU range: '+item)
        result.update(range(first, last+1))
    return sorted(result)


def cpu_blocks(topology, side, numa, count):
    try:
        profile = topology[side]
        node_cpus = cpu_list(profile['nodes'][str(numa)])
        allowed = set(cpu_list(profile['allowed_cpus']))
    except (KeyError, TypeError) as exc:
        raise ValueError(f'CPU topology missing {side} NUMA{numa} data') from exc
    selected, occupied = [], set()
    for base in range(len(node_cpus)-2):
        block = set(range(base, base+3))
        if not block.isdisjoint(occupied) or not all(node_cpus[i] in allowed for i in block):
            continue
        selected.append(base)
        occupied.update(block)
        if len(selected) == count:
            return selected
    raise ValueError(f'{side} NUMA{numa} has fewer than {count} disjoint three-CPU blocks')


def prepare(original, a, b, samples=10, numa_a=0, numa_b=1,
            timeline_db_host=None, timeline_db_port=None,
            storage_db_host=None, storage_db_port=None, workload='user', clients=1,
            b_core_base=0, cpu_topology=None):
    a, b = host(a), host(b)
    if workload not in ('user', 'home'):
        raise ValueError('workload must be user or home')
    if a == b or not 1 <= samples <= 5000 or min(numa_a, numa_b, b_core_base) < 0 or not 1 <= clients <= 4:
        raise ValueError('require two different hosts, nonnegative NUMA/B core base, 1..5000 samples, 1..4 clients')
    if cpu_topology is not None and b_core_base != 0:
        raise ValueError('--cpu-topology and --b-core-base cannot be combined')
    configs = [copy.deepcopy(original), copy.deepcopy(original)]
    timeline = workload+'_timeline'
    placements = {'client': a, timeline: a, 'post_storage': b,
                  'nginx': b, 'load_balance': b}
    a_blocks = cpu_blocks(cpu_topology, 'A', numa_a, clients+1) if cpu_topology is not None else [3*i for i in range(clients+1)]
    b_blocks = cpu_blocks(cpu_topology, 'B', numa_b, 3) if cpu_topology is not None else [b_core_base+3*i for i in range(3)]
    offsets = {'client': a_blocks[0], timeline: a_blocks[-1],
               'nginx': b_blocks[0], 'load_balance': b_blocks[1], 'post_storage': b_blocks[2]}
    original_client = original['client']
    client_port = int(original_client['server_addr'].rsplit(':', 1)[1])
    registry = []
    for index in range(clients):
        port = client_port if index == 0 else client_port + 10 + index - 1
        if port > 65535: raise ValueError('client port range exceeds 65535')
        registry.append({'id': index + 1, 'rpc_id': original_client['rpc_id'] if index == 0 else 10 + index,
                         'server_addr': a + ':' + str(port), 'bind_core_offset': a_blocks[index]})
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
        c['client'].update(generate_num=samples, user_num=int(workload == 'user'),
                           home_num=int(workload == 'home'), write_num=0)
        if workload == 'home':
            c.setdefault('social_graph_mongodb', {})['connections'] = c['user_timeline_mongodb']['connections']
        c.pop('domain_proxy', None)
        if clients > 1:
            c['domain_proxy'] = {'clients': copy.deepcopy(registry)}
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
    for client in registry[1:]:
        base = client['rpc_id']
        if base in used or base+20 in used or client['server_addr'] in addresses:
            raise ValueError('client registration collides with an existing service')
        used.update((base, base+20)); addresses.add(client['server_addr'])
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
    parser.add_argument('--b-core-base', type=int, default=0,
                        help='B NUMA CPU-list index for nginx; LB and PostStorage use base+3 and base+6')
    parser.add_argument('--cpu-topology', type=Path,
                        help='A/B node CPU lists and allowed affinities; choose disjoint three-CPU blocks automatically')
    parser.add_argument('--samples', type=int, default=10)
    parser.add_argument('--clients', type=int, default=1, help='independent client processes, 1..4')
    parser.add_argument('--workload', choices=('user', 'home'), default='user')
    parser.add_argument('--timeline-db-host')
    parser.add_argument('--timeline-db-port', type=int)
    parser.add_argument('--storage-db-host')
    parser.add_argument('--storage-db-port', type=int)
    args = parser.parse_args()
    try:
        topology = json.loads(args.cpu_topology.read_text(encoding='utf-8')) if args.cpu_topology else None
        sources = ([args.input] * 2 if args.input else
                   [ROOT/'config'/('social-user-read-'+side+'.json') for side in ('a','b')])
        configs = [prepare(json.loads(source.read_text(encoding='utf-8')), args.host_a,
                           args.host_b, args.samples, args.numa_a, args.numa_b,
                           args.timeline_db_host, args.timeline_db_port,
                           args.storage_db_host, args.storage_db_port, args.workload, args.clients,
                           args.b_core_base, topology)[index]
                   for index, source in enumerate(sources)]
        if configs[0].get('domain_proxy') != configs[1].get('domain_proxy'):
            raise ValueError('A/B client registries differ; use matching source templates')
        args.output.mkdir(parents=True, exist_ok=False)
        for side, config in zip(('a', 'b'), configs):
            (args.output/('config-'+side+'.json')).write_text(json.dumps(config, indent=2)+'\n', encoding='utf-8')
    except (ValueError, KeyError, OSError) as exc:
        parser.error(str(exc))
    print(f'UB_SOCIAL_CONFIG_READY scope={args.workload}-read clients={args.clients} lane_per_client=1 concurrency=1 nginx=1 '
          f'A=client,{args.workload}_timeline B=post_storage,lb,nginx,domain-owner database=explicit-overrides-only')
    a_config = configs[0]
    print('UB_SOCIAL_CPU_PLAN A_clients=' + ','.join(str(c['bind_core_offset']) for c in
          a_config.get('domain_proxy', {}).get('clients', [a_config['client']])) +
          f' A_timeline={a_config[args.workload+"_timeline"]["bind_core_offset"]}' +
          f' B_nginx={a_config["nginx"]["bind_core_offset"]}' +
          f' B_load_balance={a_config["load_balance"]["bind_core_offset"]}' +
          f' B_post_storage={a_config["post_storage"]["bind_core_offset"]}')


if __name__ == '__main__': main()
