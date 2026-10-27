#!/usr/bin/env python3
"""Validate stopped A and B application logs; never claim kernel/UB health."""
import argparse
from pathlib import Path
import re


def rows(text, marker):
    return [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines()
            if line.startswith(marker+' ')]


def check(logdir, mode, samples, peer_logs=None, chain='legacy', workload='user', clients=1, nginx_count=1):
    if peer_logs is None:
        raise ValueError('require B --peer-logs; PostStorage is on B, not A')
    if workload not in ('user', 'home'): raise ValueError('invalid workload')
    if not 1 <= clients <= 4: raise ValueError('require 1..4 client processes')
    if nginx_count not in (1, 3):
        raise ValueError('require one or three nginx instances')
    timeline = workload+'_timeline'
    client_names = ['client'] if clients == 1 else [f'client-{i}' for i in range(1, clients+1)]
    paths = {name:logdir for name in (*client_names,timeline)}
    paths['post_storage'] = peer_logs
    if mode == 'rpc':
        paths['load_balance'] = peer_logs
        for name in (['nginx'] if nginx_count == 1 else ['nginx-1','nginx-2','nginx-3']):
            paths[name] = peer_logs
    logs = {name:(directory/(name+'.log')).read_text(encoding='utf-8',errors='replace')
            for name,directory in paths.items()}
    if nginx_count == 3 and mode == 'domain':
        owner = (peer_logs/'domain_owner.log').read_text(encoding='utf-8',errors='replace')
        if re.search(r'UB_DOMAIN_\w*FAIL|terminate called|Assertion.*failed', owner):
            raise ValueError('failure in B domain_owner.log')
        owners = rows(owner, 'UB_DOMAIN_OWNER_READY')
        published = rows(owner, 'UB_DOMAIN_POOL_PUBLISHED')
        if (len(owners) != 1 or owners[0].get('nginx_count') != '3' or
            owners[0].get('code_mode') != 'remote-social' or
            sum(int(p.get('count',0)) for p in published) < clients+1):
            raise ValueError('require B three-nginx remote-code owner and all connection publications')
    for name,text in logs.items():
        if re.search(r'SN_DOMAIN_ERROR|SN_DOMAIN_CONFIG_ERROR|SN_MONGODB_ERROR|UB_DOMAIN_\w*FAIL|terminate called|Assertion.*failed',text):
            raise ValueError('failure in '+name+'.log')
    measured = {'all':0, 'empty':0, 'nonempty':0}
    reports = []
    for index, name in enumerate(client_names, 1):
        result, measurement = rows(logs[name],'SN_DOMAIN_E2E'), rows(logs[name],'SN_DOMAIN_MEASUREMENT')
        if len(result)!=1 or len(measurement)!=1:
            raise ValueError('require one completed campaign and measurement report in '+name)
        r,m = result[0],measurement[0]
        if (int(r['completed'])!=samples or int(r['expected'])!=samples or
            r['clock']!='steady_ns' or r['scope']!='call-to-consume' or r['fixture']!='0' or
            int(m['total'])!=samples or int(m['measured'])!=int(r['measured']) or
            int(r['measured'])<=0):
            raise ValueError('incomplete/mismatched campaign or wrong measurement scope in '+name)
        if clients>1 and (int(r.get('source',0))!=index or int(m.get('source',0))!=index):
            raise ValueError('client source mismatch in '+name)
        classes = rows(logs[name],'SN_DOMAIN_CLASS')
        counts = {row['class']:int(row['count']) for row in classes if row['phase']=='measured'}
        if counts.get('all')!=int(r['measured']) or counts.get('empty',0)+counts.get('nonempty',0)!=counts['all']:
            raise ValueError('missing/inconsistent measured result classes in '+name)
        for key in measured: measured[key] += counts.get(key,0)
        reports.append(r)
    ready = []
    if chain not in ('legacy', 'fused') or (mode == 'rpc' and chain != 'legacy'):
        raise ValueError('invalid chain for mode')
    for name in (*client_names,timeline):
        text = logs[name]
        if mode=='domain':
            found = rows(text,'SN_DOMAIN_READY')
            done = rows(text,'SN_DOMAIN_DONE')
            if (len(found)!=1 or found[0].get('backend')!='ub' or
                found[0].get('chain')!=chain or
                found[0].get('transitions')!=('3' if chain=='fused' else '4') or
                len(done)!=1 or done[0].get('backend')!='ub' or
                int(done[0]['calls'])!=(samples*clients if name==timeline else samples)):
                raise ValueError('require UB domain readiness, call count and clean close in '+name)
            if clients>1 and name!=timeline and int(found[0].get('source',0))!=int(name.rsplit('-',1)[1]):
                raise ValueError('UB client connection source mismatch in '+name)
            if nginx_count == 3 and int(found[0].get('services',0)) != 4:
                raise ValueError('require one LB and three nginx domains in '+name)
            if nginx_count == 3:
                bound = rows(text, 'UB_DOMAIN_BOUND')
                if (len(bound) != 1 or bound[0].get('code_mode') != 'remote-social' or
                    bound[0].get('code_source') != 'B-published' or bound[0].get('nginx_count') != '3'):
                    raise ValueError('require B-published code binding in '+name)
            if nginx_count == 3 and name != timeline:
                routes = rows(text, 'SN_DOMAIN_ROUTE')
                counts = {int(r['domain']):int(r['calls']) for r in routes}
                if (len(routes) != 3 or set(counts) != {2,3,4} or
                    sum(counts.values()) != samples or
                    (samples >= 3 and not all(counts.values()))):
                    raise ValueError('missing/incomplete three-nginx route counts in '+name)
            ready.append(found[0])
        else:
            if rows(text,'SN_DOMAIN_READY') or not any(row.get('enabled')=='0'
                for row in rows(text,'SN_DOMAIN_CONFIG')):
                raise ValueError('RPC baseline did not use the original RPC path in '+name)
    if ready and (len({r['slot'] for r in ready})!=len(ready) or
                  len({r['identity'] for r in ready})!=len(ready)):
        raise ValueError('caller and reverse executor connections are not isolated')
    if mode == 'rpc' and nginx_count == 3:
        totals = [0, 0]
        for name in ('nginx-1','nginx-2','nginx-3'):
            proxy = rows(logs[name], 'SN_CXL_PROXY_DONE')
            if len(proxy) != 1 or proxy[0].get('role') != 'nginx':
                raise ValueError('missing nginx traffic report in '+name)
            forward, reverse = int(proxy[0].get('forward',-1)), int(proxy[0].get('reverse',-1))
            if forward != reverse or (samples >= 3 and forward <= 0):
                raise ValueError('incomplete nginx traffic in '+name)
            totals[0] += forward; totals[1] += reverse
        if totals != [samples*clients, samples*clients]:
            raise ValueError('incorrect total three-nginx traffic')
    if clients>1 and mode=='rpc':
        for name in (('load_balance','nginx') if nginx_count == 1 else ('load_balance',)):
            proxy = rows(logs[name],'SN_CXL_PROXY_DONE')
            if (len(proxy)!=1 or proxy[0].get('role')!=name or
                int(proxy[0].get('forward',-1))!=samples*clients or
                int(proxy[0].get('reverse',-1))!=samples*clients):
                raise ValueError('incomplete RPC proxy traffic in '+name)
    if clients>1:
        intervals = []
        for r in reports:
            if not all(key in r for key in ('p50_ns','p99_ns','avg_ns')):
                raise ValueError('missing per-client latency summary')
            begin,end = int(r.get('campaign_begin_ns',0)),int(r.get('campaign_end_ns',0))
            if not begin or end<=begin: raise ValueError('missing campaign timing')
            intervals.append((begin,end))
        if max(begin for begin,_ in intervals)>=min(end for _,end in intervals):
            raise ValueError('client campaigns did not overlap')
        measured['_wall_ns'] = max(end for _,end in intervals)-min(begin for begin,_ in intervals)
    measured['_reports'] = reports
    return measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--logs',required=True,type=Path)
    parser.add_argument('--peer-logs',required=True,type=Path,help='local directory containing this run\'s B logs')
    parser.add_argument('--mode',required=True,choices=('domain','rpc'))
    parser.add_argument('--samples',required=True,type=int)
    parser.add_argument('--clients',type=int,default=1,help='independent client processes, 1..4')
    parser.add_argument('--nginx-count',type=int,choices=(1,3),default=1)
    parser.add_argument('--workload',choices=('user','home'),default='user')
    parser.add_argument('--chain',choices=('legacy','fused'),
                        help='expected application domain chain; default fused for domain, legacy for RPC')
    args = parser.parse_args()
    chain = args.chain or ('fused' if args.mode == 'domain' else 'legacy')
    try: counts = check(args.logs,args.mode,args.samples,args.peer_logs,chain,args.workload,args.clients,args.nginx_count)
    except (OSError,ValueError,KeyError) as exc: parser.error(str(exc))
    print(f'UB_SOCIAL_E2E_PASS workload={args.workload} mode={args.mode} chain={chain if args.mode=="domain" else "rpc"} clients={args.clients} nginx={args.nginx_count} samples={args.samples} samples_per_client={args.samples} '
          f'measured={counts["all"]} nonempty={counts["nonempty"]} empty={counts["empty"]} '
          'scope=application-logs kernel_health_checked=0')
    for r in counts['_reports']:
        if all(key in r for key in ('p50_ns','p99_ns','avg_ns')):
            print(f'UB_SOCIAL_CLIENT_LATENCY source={r.get("source","1")} p50_ns={r["p50_ns"]} '
                  f'p99_ns={r["p99_ns"]} avg_ns={r["avg_ns"]} measured={r["measured"]}')
    if args.clients>1:
        mean = sum(float(r['avg_ns'])*int(r['measured']) for r in counts['_reports'])/counts['all']
        throughput = args.clients*args.samples*1e9/counts['_wall_ns']
        print(f'UB_SOCIAL_CONCURRENT_SUMMARY clients={args.clients} weighted_avg_ns={mean:.1f} '
              f'max_client_p99_ns={max(float(r["p99_ns"]) for r in counts["_reports"]):.1f} '
              f'wall_ns={counts["_wall_ns"]} completed_throughput_ops_s={throughput:.1f} '
              'pooled_p50_p99=not_available')
    if not counts['nonempty']:
        print('WARNING: all measured results were empty; this does not validate nonempty post consumption.')


if __name__=='__main__': main()
