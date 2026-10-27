#!/usr/bin/env python3
"""Read-only CPU/affinity preflight before creating any SDK region."""
import argparse
import errno
import json
import os
import socket
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
    registered = config.get('domain_proxy', {}).get('clients', [])
    if registered:
        if not 1 <= len(registered) <= 4 or [c['id'] for c in registered] != list(range(1,len(registered)+1)):
            raise ValueError('require 1..4 registered Client IDs ordered 1..N')
        if registered[0]['server_addr'] != config['client']['server_addr'] or registered[0]['rpc_id'] != config['client']['rpc_id']:
            raise ValueError('first registered Client must match client endpoint')
        if any(c['server_addr'].rsplit(':',1)[0] != a for c in registered):
            raise ValueError('all registered Clients must run on A')
    user, home = config['client']['user_num'], config['client']['home_num']
    if (user, home) not in ((1, 0), (0, 1)) or config['client']['write_num'] != 0:
        raise ValueError('require exactly one read workload')
    timeline = 'home_timeline' if home else 'user_timeline'
    if a == b or address(timeline) != a or address('load_balance') != b or address('post_storage') != b:
        raise ValueError('require A=Client/Timeline, B=PostStorage/nginx/LB in both modes; regenerate config')
    return [timeline, 'client'] if side == 'A' else (
        ['post_storage'] if mode == 'domain' else ['post_storage', 'nginx', 'load_balance'])


def check(config, services, nodes, allowed):
    common, used = config['common'], set()
    if common['client_num']!=1 or common['server_num']!=1:
        raise ValueError('multi-process experiment requires one lane per process')
    placements = [(name, config[name]['bind_core_offset']) for name in services]
    if 'nginx' in services:
        nginx = config.get('domain_proxy', {}).get('nginx_instances', [])
        if nginx and len(nginx) not in (1, 3):
            raise ValueError('require one or three nginx instances')
        if len(nginx) == 3:
            if nginx[0]['server_addr'] != config['nginx']['server_addr'] or nginx[0]['rpc_id'] != config['nginx']['rpc_id']:
                raise ValueError('first nginx must match base endpoint')
            placements = [(name, base) for name, base in placements if name != 'nginx']
            placements.extend((f'nginx-{i+1}', entry['bind_core_offset']) for i, entry in enumerate(nginx))
    if 'client' in services:
        clients = config.get('domain_proxy', {}).get('clients', [])
        if clients:
            if not 1 <= len(clients) <= 4:
                raise ValueError('require 1..4 registered clients')
            if [c['id'] for c in clients] != list(range(1,len(clients)+1)):
                raise ValueError('client IDs must be ordered 1..N')
            placements = [(name, base) for name, base in placements if name != 'client']
            placements.extend((f'client-{c["id"]}', c['bind_core_offset']) for c in clients)
    for name, base in placements:
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


def endpoints_for(config, services):
    """Expand the services this side starts into (name, ip, port) triples.

    The application processes bind the SM UDP port in Nexus before anything
    else, so a stale or duplicate binder is only ever visible as an unattributed
    "bind: Address already in use" terminate. Probing before startup keeps that
    failure attributable.
    """
    entries = []
    for name in services:
        if name == 'client':
            registered = config.get('domain_proxy', {}).get('clients') or [config['client']]
            entries.extend((f'client-{c["id"]}', c['server_addr']) for c in registered)
        elif name == 'nginx':
            instances = config.get('domain_proxy', {}).get('nginx_instances') or [config['nginx']]
            entries.extend((f'nginx-{index+1}', item['server_addr']) for index, item in enumerate(instances))
        else:
            entries.append((name, config[name]['server_addr']))
    result = []
    for name, address in entries:
        host, port = address.rsplit(':', 1)
        result.append((name, host, int(port)))
    return result


def port_holders(port):
    """Name local processes that own a UDP port, using only /proc (Linux).

    The failure this preflight exists for is a bind clash at process start, and
    the owner is often an orphaned worker from the previous campaign. Waiting
    for the exact moment with ss is impractical, so resolve the owner here.
    """
    inodes = set()
    for table in ('/proc/net/udp', '/proc/net/udp6'):
        try:
            lines = Path(table).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) < 10:
                continue
            try:
                local_port = int(fields[1].rsplit(':', 1)[1], 16)
            except (IndexError, ValueError):
                continue
            if local_port == port:
                inodes.add('socket:[' + fields[9] + ']')
    if not inodes:
        return []
    holders = []
    try:
        entries = list(Path('/proc').iterdir())
    except OSError:
        return holders
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            for fd in (entry / 'fd').iterdir():
                try:
                    if os.readlink(fd) not in inodes:
                        continue
                except OSError:
                    continue
                cmd = (entry / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace').strip()
                holders.append(f'pid={entry.name} cmd={cmd or "?"}')
                break
        except OSError:
            continue
    return holders


def ephemeral_range_overlap():
    """Report a kernel ephemeral port range that covers the eRPC SM ports.

    Any unbound UDP socket on the host may then be assigned a port another eRPC
    process needs for its Nexus, which yields the same unattributed EADDRINUSE.
    """
    try:
        low, high = (int(v) for v in Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split())
    except (OSError, ValueError):
        return None
    if low <= 31881 and high >= 31850:
        return f'SN_EPHEMERAL_RANGE_OVERLAP low={low} high={high} reserved=31850-31881'
    return None


def check_ports(entries):
    """Detect a live binder of any configured SM UDP port on this host.

    Nexus binds each SM socket to 0.0.0.0, so a holder on any local address of
    the port would make the service fail with an unattributed EADDRINUSE. The
    service address is probed first so an endpoint of the peer host is skipped.
    """
    checked = []
    for name, host, port in entries:
        error = None
        for address in ((host, port), ('', port)):
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                probe.bind(address)
            except OSError as exc:
                error = exc.errno
            finally:
                probe.close()
            if error is not None:
                break
        if error == errno.EADDRNOTAVAIL:
            continue  # Endpoint belongs to the peer host, not this one
        if error == errno.EADDRINUSE:
            try:
                holders = port_holders(port)
            except OSError:
                holders = []
            detail = ' holders=' + ' | '.join(holders) if holders else ''
            raise ValueError(
                f'SN_PORT_IN_USE service={name} addr={host}:{port}{detail}; '
                f'inspect "ss -lunp | grep :{port}" and stop the holder')
        if error is not None:
            raise OSError(error, os.strerror(error))
        checked.append(f'{name}={host}:{port}')
    return checked


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
        ports = check_ports(endpoints_for(config, services))
    except (OSError,ValueError,KeyError,AttributeError) as exc: parser.error(str(exc))
    overlap = ephemeral_range_overlap()
    if overlap: print(overlap)
    print('UB_SOCIAL_CPU_PREFLIGHT_PASS cpus='+','.join(map(str,cores)))
    print('UB_SOCIAL_PORT_PREFLIGHT_PASS ports='+','.join(ports))


if __name__=='__main__': main()
