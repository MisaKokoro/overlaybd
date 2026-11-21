#!/usr/bin/env python3
"""ARM64/openEuler 隔离测试。仅管理本目录进程，不安装系统服务或管理 Mooncake。"""
import argparse, fcntl, hashlib, ipaddress, json, os, pathlib, platform, re, shutil
import signal, socket, subprocess, sys, time, urllib.request, uuid
B = pathlib.Path(__file__).resolve().parent
OPTIONS = B/'options.json'
STATE = B/'state'

def run(args, **kw):
    return subprocess.run([str(x) for x in args], check=True, **kw)
def output(args, **kw):
    return subprocess.check_output([str(x) for x in args], text=True, **kw).strip()
def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n')
def options():
    if not OPTIONS.exists(): raise RuntimeError('请先执行 init')
    c=json.loads(OPTIONS.read_text())
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,15}',c['bridge']) or c['bridge'] in ['docker0','lo']:
        raise RuntimeError('非法或保留网桥名')
    if c['fallback'] not in ['none','direct']:raise RuntimeError('fallback 只能为 none/direct')
    n=c.get('cache_block_size')
    if type(n) is not int or n<4096 or n>4194304 or n%4096:
        raise RuntimeError('cache_block_size 必须为 4 KiB～4 MiB 的 4096 整数倍；旧测试目录请重新初始化')
    return c
def env(c):
    e=os.environ.copy(); e['PATH']=str(B/'bin')+':'+os.environ.get('PATH','/usr/sbin:/usr/bin:/sbin:/bin')
    # 显式配置的系统 SDK 路径优先；不继承旧实验的 LD_LIBRARY_PATH。
    e['LD_LIBRARY_PATH']=':'.join(x for x in [c['mooncake_lib_dir'],str(B/'lib')] if x)
    e.setdefault('GLOG_minloglevel','2')
    return e

def proc_start(pid):
    try:
        fields=pathlib.Path('/proc',str(pid),'stat').read_text().rsplit(')',1)[1].split()
        return None if fields[0]=='Z' else fields[19]
    except FileNotFoundError: return None

def record(name):
    p=STATE/(name+'.pid.json')
    return json.loads(p.read_text()) if p.exists() else None

def alive(name):
    r=record(name)
    current=proc_start(r['pid']) if r else None
    return bool(r and current is not None and current==r['start'])

def require_stopped():
    if any(alive(n) for n in ['registry','tcmu','snapshotter','containerd','docker']):
        raise RuntimeError('请先 stop，再修改配置或重新初始化')

def docker(args, capture=False, quiet=False):
    c=options(); cmd=[B/'bin/docker','--config',B/'config/client','--host','unix://'+str(STATE/'docker.sock'),*args]
    return output(cmd,env=env(c),stderr=subprocess.DEVNULL if quiet else None) if capture else run(cmd,env=env(c))

def ctr(args):
    return output([B/'bin/ctr','--address',STATE/'containerd.sock','--namespace','obd-portable',*args],env=env(options()))

def generate(c):
    for p in ['config/client','state','logs','data/cache','data/snapshotter','data/containerd','data/docker']:
        (B/p).mkdir(parents=True,exist_ok=True)
    registry='127.0.0.1:'+str(c['registry_port']); api='http://127.0.0.1:'+str(c['api_port'])
    save(B/'config/credentials.json',{'auths':{}})
    save(B/'config/overlaybd.json',{
        'logConfig':{'logLevel':0,'logPath':str(B/'logs/overlaybd.log'),'logSizeMB':128},
        'cacheConfig':{'cacheType':'file','cacheDir':str(B/'data/cache'),'cacheSizeGB':2},
        'credentialConfig':{'mode':'file','path':str(B/'config/credentials.json')},
        'ioEngine':0,'download':{'enable':False},'p2pConfig':{'enable':False},
        'exporterConfig':{'enable':False},'registryFsVersion':'v2',
        'serviceConfig':{'enable':True,'address':api},'userAgent':'overlaybd/portable-moonpool',
        'pool':{'enable':True,'cacheRegistry':False,'master':c['master'],'metadataServer':c['metadata'],
                'localHostname':c['local_ip'],'nameSpace':c['namespace'],'protocol':'tcp',
                'batchBlocks':16,'fallback':c['fallback'],'blockSize':c['cache_block_size'],
                'timeoutMs':c['timeout_ms'],'retryMs':1000,'queueDepth':32}})
    save(B/'config/snapshotter.json',{'root':str(B/'data/snapshotter'),'address':str(STATE/'snapshotter.sock'),
         'verbose':'debug','rwMode':'overlayfs','runtimeType':'docker','autoRemoveDev':True,'tenant':c['device_tenant'],
         'mirrorRegistry':[{'host':registry,'insecure':True}],'poolEnable':True,'poolFsVersion':'v1',
         'poolBlockSize':c['cache_block_size'],'poolTenant':c['namespace'],'poolPrivate':True,'poolPreflightAddress':api})
    save(B/'config/docker.json',{'hosts':['unix://'+str(STATE/'docker.sock')],'data-root':str(B/'data/docker'),
         'exec-root':str(STATE/'de'),'pidfile':str(STATE/'docker.pid'),
         'containerd':str(STATE/'containerd.sock'),'containerd-namespace':'obd-portable',
         'containerd-plugins-namespace':'obd-portable-plugins','features':{'containerd-snapshotter':True},
         'storage-driver':'overlaybd','bridge':c['bridge'],'iptables':False,'ip6tables':False,
         'ip-forward':False,'ip-masq':False,'insecure-registries':[registry],'debug':True})
    q=lambda p:json.dumps(str(p))
    (B/'config/containerd.toml').write_text('version = 2\nroot = '+q(B/'data/containerd')+'\nstate = '+q(STATE/'containerd')+
      '\ndisabled_plugins = ["io.containerd.grpc.v1.cri"]\n[grpc]\n  address = '+q(STATE/'containerd.sock')+
      '\n[debug]\n  level = "debug"\n[proxy_plugins.overlaybd]\n  type = "snapshot"\n  address = '+q(STATE/'snapshotter.sock')+'\n')
    (B/'config/registry.yml').write_text('version: 0.1\nlog:\n  level: debug\nstorage:\n  filesystem:\n    rootdirectory: '+q(B/'registry-data')+'\nhttp:\n  addr: '+registry+'\n')

def doctor(c,verbose=True):
    if platform.machine() not in ['aarch64','arm64']: raise RuntimeError('此二进制包仅支持 ARM64')
    for tool in ['ip','ss','modprobe','mount','mountpoint','ldd','readelf']:
        if not shutil.which(tool): raise RuntimeError('缺少系统工具：'+tool)
    for n in ['docker','dockerd','containerd','ctr','containerd-shim-runc-v2','runc','registry','overlaybd-tcmu','overlaybd-snapshotter','pool-seed']:
        if not os.access(B/'bin'/n,os.X_OK): raise RuntimeError('缺少可执行文件：'+n)
    for n in ['overlaybd-tcmu','pool-seed']:
        p=subprocess.run(['ldd',str(B/'bin'/n)],env=env(c),capture_output=True,text=True)
        if verbose: print(p.stdout,flush=True)
        if p.returncode or 'not found' in p.stdout+p.stderr: raise RuntimeError('动态库不兼容/缺失，请按上面的 ldd 结果安装依赖')
        match=re.search(r'libmooncake_store\.so\s+=>\s+(\S+)',p.stdout)
        if not match: raise RuntimeError('未找到系统 Mooncake C SDK')
        symbols=output(['readelf','--dyn-syms','--wide',match[1]])
        for sym in ['mooncake_store_setup','mooncake_store_put','mooncake_store_batch_get_into']:
            if not any(sym in l and ' UND ' not in l for l in symbols.splitlines()):
                raise RuntimeError('系统 Mooncake 缺少 C 接口：'+sym)
    print('架构及依赖检查通过；仍需确保系统 Mooncake 与构建所用 C ABI 兼容。')

def other_tcmu():
    own=record('tcmu') or {}
    for p in pathlib.Path('/proc').glob('[0-9]*/comm'):
        try:
            if p.read_text().strip()=='overlaybd-tcmu' and int(p.parent.name)!=own.get('pid'):
                raise RuntimeError('机器已有其他 overlaybd-tcmu（PID '+p.parent.name+'），请先安排独占测试，脚本不会停止它')
        except (FileNotFoundError,ProcessLookupError): pass

def bridge(c):
    path=pathlib.Path('/sys/class/net')/c['bridge']; owner='obd-portable:'+str(B)
    if path.exists():
        if (path/'ifalias').read_text().strip()!=owner: raise RuntimeError('网桥已存在且不属于本目录')
        return
    target=ipaddress.ip_interface(c['bridge_cidr'])
    for route in json.loads(output(['ip','-j','-4','route','show','table','all'])):
        if route.get('dst','default')!='default' and target.network.overlaps(ipaddress.ip_network(route['dst'],strict=False)):
            raise RuntimeError('测试网段与已有路由重叠，请 init 时设置 --bridge-cidr')
    run(['ip','link','add',c['bridge'],'type','bridge'])
    run(['ip','link','set',c['bridge'],'alias',owner])
    run(['ip','address','add',str(target),'dev',c['bridge']]); run(['ip','link','set',c['bridge'],'up'])

def spawn(name,args,c):
    if alive(name): return
    with (B/'logs'/(name+'.log')).open('ab') as f:
        p=subprocess.Popen([str(x) for x in args],stdin=subprocess.DEVNULL,stdout=f,stderr=subprocess.STDOUT,
                           cwd=B,env=env(c),start_new_session=True)
    save(STATE/(name+'.pid.json'),{'pid':p.pid,'start':proc_start(p.pid)})

def wait_for(name,check):
    for _ in range(60):
        if not alive(name): raise RuntimeError(name+' 已退出，请查看 logs/'+name+'.log')
        try:
            if check(): return
        except (OSError,subprocess.SubprocessError,urllib.error.URLError): pass
        time.sleep(.5)
    raise RuntimeError(name+' 启动超时，请查看 logs/')

def http_up(url):
    with urllib.request.urlopen(url,timeout=1) as r:return r.status==200

def api_up(c):
    try:
        urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:'+str(c['api_port'])+'/pool/check',data=b'{}'),timeout=1)
    except urllib.error.HTTPError as e:return e.code==400
    return True

def start(c):
    doctor(c,False); other_tcmu()
    if any(alive(n) for n in ['registry','tcmu','snapshotter','containerd','docker']):
        raise RuntimeError('已有本目录进程，使用 status 检查；修改或重启前先 stop')
    for port in [c['registry_port'],c['api_port']]:
        with socket.socket() as s:
            s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            s.bind(('127.0.0.1',port))
    # configfs 和内核模块是主机资源，只加载/挂载，不卸载已有资源。
    run(['modprobe','target_core_user']); run(['modprobe','tcm_loop'])
    if subprocess.call(['mountpoint','-q','/sys/kernel/config'])!=0:run(['mount','-t','configfs','none','/sys/kernel/config'])
    bridge(c)
    try:
        spawn('registry',[B/'bin/registry','serve',B/'config/registry.yml'],c)
        wait_for('registry',lambda:http_up('http://127.0.0.1:'+str(c['registry_port'])+'/v2/'))
        spawn('tcmu',[B/'bin/overlaybd-tcmu',B/'config/overlaybd.json'],c);wait_for('tcmu',lambda:api_up(c))
        spawn('snapshotter',[B/'bin/overlaybd-snapshotter',B/'config/snapshotter.json'],c)
        wait_for('snapshotter',lambda:(STATE/'snapshotter.sock').is_socket())
        spawn('containerd',[B/'bin/containerd','--config',B/'config/containerd.toml'],c)
        wait_for('containerd',lambda:bool(ctr(['version'])))
        plugins=ctr(['plugins','ls'])
        if not any('overlaybd' in l and l.split()[-1]=='ok' for l in plugins.splitlines()):raise RuntimeError('overlaybd snapshotter 未就绪')
        run([B/'bin/dockerd','--validate','--config-file',B/'config/docker.json'],env=env(c))
        spawn('docker',[B/'bin/dockerd','--config-file',B/'config/docker.json'],c)
        wait_for('docker',lambda:bool(docker(['version','--format','{{.Server.Version}}'],True,True)))
        print(docker(['info','--format','Docker {{.ServerVersion}} / {{.Driver}} / {{.DockerRootDir}}'],True))
    except Exception:
        print('启动失败，尝试停止本目录已启动进程；日志保留。',file=sys.stderr)
        stop(c);raise

def stop(c):
    if alive('docker') and docker(['ps','-q'],True):raise RuntimeError('本目录 Docker 仍有运行容器，先手动停止/删除')
    if alive('containerd') and ctr(['tasks','ls','-q']):raise RuntimeError('本目录 containerd 仍有任务，拒绝停止')
    if str(B/'data/snapshotter') in pathlib.Path('/proc/self/mountinfo').read_text():
        raise RuntimeError('本目录 snapshotter 仍有挂载，先通过 Docker 删除测试容器并等待清理')
    for name in ['docker','containerd','snapshotter','tcmu','registry']:
        if alive(name):
            os.kill(record(name)['pid'],signal.SIGTERM)
            for _ in range(100):
                if not alive(name): break
                time.sleep(.1)
            if alive(name):raise RuntimeError(name+' 尚未退出，未强杀；请查看日志后再次 stop')
        (STATE/(name+'.pid.json')).unlink(missing_ok=True)
    p=pathlib.Path('/sys/class/net')/c['bridge']
    if p.exists():
        if (p/'ifalias').read_text().strip()!='obd-portable:'+str(B):raise RuntimeError('网桥归属不匹配，拒绝删除')
        run(['ip','link','delete',c['bridge'],'type','bridge'])
    print('本目录进程已停止，测试网桥已删除。配置、日志、镜像、本地缓存保留；Mooncake 不受影响。')

def wait_tcp_budget(c, manifest=None):
    # 当前 TCP SDK 每个块可能建立短连接；不修改 sysctl，只等待已有 TIME_WAIT 自然释放。
    if manifest is None:manifest=json.loads((B/'fixture/manifest.json').read_text())
    needed=sum((l['size']+c['cache_block_size']-1)//c['cache_block_size'] for l in manifest['layers'])+1024
    lo,hi=map(int,pathlib.Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split())
    capacity=hi-lo+1
    if needed>capacity:raise RuntimeError('临时端口范围不足以完成本镜像的短连接测试；请评估 TCP 连接复用或单独准备网络测试环境')
    started=time.monotonic();last=-15
    while True:
        rows=output(['ss','-Htan','state','time-wait']).splitlines();used=0
        for row in rows:
            fields=row.split()
            if len(fields)>=4:
                try: used+=lo<=int(fields[2].rsplit(':',1)[1])<=hi
                except ValueError:pass
        if capacity-used>=needed:return
        elapsed=time.monotonic()-started
        if elapsed>90:raise RuntimeError('等待90秒后临时端口余量仍不足，请检查其他网络负载；未修改系统参数')
        if elapsed-last>=15:
            print('等待 TCP TIME_WAIT 自然释放：当前约 %d 个连接，测试预留 %d 个临时端口…'%(used,needed),flush=True);last=elapsed
        time.sleep(1)

def seed(c):
    wait_tcp_budget(c)
    manifest=json.loads((B/'fixture/manifest.json').read_text());lines=[]
    for layer in manifest['layers']:
        digest=layer['digest'];p=B/'registry-data/docker/registry/v2/blobs/sha256'/digest[7:9]/digest[7:]/'data'
        if p.stat().st_size!=layer['size'] or hashlib.sha256(p.read_bytes()).hexdigest()!=digest[7:]:raise RuntimeError('测试层校验失败')
        bs=c['cache_block_size']
        lines.append(digest+' '+str(bs)+' '+str(p))
    f=B/'config/seed-layers.txt';f.write_text('\n'.join(lines)+'\n')
    run([B/'bin/pool-seed',c['master'],c['metadata'],c['local_ip'],c['namespace'],c['namespace'],f],env=env(c))
    print('预热完成，检查后续全层读取的 TCP 端口余量。',flush=True)
    wait_tcp_budget(c)

def verify(c,missing=False,args=None):
    external=bool(args and args.image)
    manifest=json.loads(pathlib.Path(args.manifest).read_text()) if external else json.loads((B/'fixture/manifest.json').read_text())
    if not isinstance(manifest.get('layers'),list) or not manifest['layers']:raise RuntimeError('manifest 缺少层列表')
    if external and '@sha256:' not in args.image:raise RuntimeError('外部验收请使用固定 @sha256: 引用')
    if external:
        expected='sha256:'+hashlib.sha256(pathlib.Path(args.manifest).read_bytes()).hexdigest()
        if args.image.rsplit('@',1)[1]!=expected:raise RuntimeError('PullRef 与提供的 manifest digest 不符')
    if not missing:wait_tcp_budget(c,manifest)
    image=args.image if external else '127.0.0.1:'+str(c['registry_port'])+'/obd-baseline:pool'
    log=B/'logs/overlaybd.log';before=log.stat().st_size if log.exists() else 0
    reg=B/'logs/registry.log';reg_before=reg.stat().st_size
    cmd=[B/'bin/docker','--config',B/'config/client','--host','unix://'+str(STATE/'docker.sock'),'pull',image]
    p=subprocess.run([str(x) for x in cmd],env=env(c),capture_output=True,text=True,timeout=650)
    print(p.stdout,end='');print(p.stderr,end='',file=sys.stderr)
    if missing:
        if c['fallback']!='none' or p.returncode==0 or 'moonpool' not in p.stderr or 'HTTP 404' not in p.stderr:
            raise RuntimeError('预期因缺块失败，但实际结果不符（不能用其他错误冒充通过）')
    else:
        if p.returncode:raise RuntimeError('docker pull 失败')
        command=args.check_command if external else 'cat /opt/obd-test/marker.txt && sha256sum /opt/obd-test/cold.bin'
        result=docker(['run','--rm','--network','none',image,'/bin/sh','-c',command],True)
        print(result)
        if external:
            if result.strip()!=args.expect_output.strip():raise RuntimeError('外部镜像验证输出不一致')
        else:
            expected=json.loads((B/'fixture/expected.json').read_text())
            if expected['marker'] not in result or expected['cold_file_sha256'] not in result:raise RuntimeError('容器文件校验失败')
    time.sleep(.5)
    with log.open() as f:f.seek(before);text=f.read()
    with reg.open() as f:f.seek(reg_before);registry=f.read()
    origin=[l for l in registry.splitlines() if 'http.request.useragent=overlaybd/' in l and '/blobs/' in l and ('http.response.status=200' in l or 'http.response.status=206' in l)]
    batches=[int(v) for v in re.findall(r'moonpool batch blocks=(\d+)',text)]
    covers=image.split('/')[0]=='127.0.0.1:'+str(c['registry_port'])
    report={'passed':True,'expected_missing':missing,'fallback':c['fallback'],'pool_hits':text.count('moonpool hit key='),
            'fallbacks':text.count('moonpool fallback key='),'batch_calls':len(batches),'max_batch':max(batches,default=0),
            'registry_log_covers_image':covers,'registry_layer_requests':len(origin) if covers else None,'image':image,'namespace':c['namespace'],'cache_block_size':c['cache_block_size']}
    if c['fallback']=='none' and (origin or report['fallbacks']):raise RuntimeError('禁止回源模式出现回源请求')
    contents=ctr(['content','ls'])
    local_digests={line.split()[0] for line in contents.splitlines()[1:] if line.strip()}
    if any(l['digest'] in local_digests for l in manifest['layers']):raise RuntimeError('containerd 完整下载了层，未走预期按需路径')
    report['layer_blobs_absent']=True
    save(B/'results'/('verify-'+time.strftime('%Y%m%d-%H%M%S')+'.json'),report)
    print(json.dumps(report,indent=2,ensure_ascii=False))
    if not missing and report['pool_hits']==0:print('本轮可能完全命中本地缓存；不能作为 Mooncake 网络读取证据。')

def main():
    p=argparse.ArgumentParser(description=__doc__);s=p.add_subparsers(dest='command',required=True)
    i=s.add_parser('init',help='生成本目录配置（默认禁止回源）')
    i.add_argument('--master',required=True);i.add_argument('--metadata',required=True);i.add_argument('--local-ip',required=True)
    i.add_argument('--mooncake-lib-dir',default='');i.add_argument('--fallback',choices=['none','direct'],default='none')
    i.add_argument('--cache-block-size',type=int,default=262144)
    i.add_argument('--timeout-ms',type=int,default=2000);i.add_argument('--registry-port',type=int,default=16000)
    i.add_argument('--api-port',type=int,default=16056);i.add_argument('--device-tenant',type=int,default=30)
    i.add_argument('--bridge',default='obdp-test0');i.add_argument('--bridge-cidr',default='172.30.253.1/24')
    i.add_argument('--namespace',default='portable-'+uuid.uuid4().hex[:16])
    for n in ['doctor','start','stop','status','seed','fresh-cache']:s.add_parser(n)
    for name in ['verify','verify-missing']:
        v=s.add_parser(name);v.add_argument('--image');v.add_argument('--manifest');v.add_argument('--check-command');v.add_argument('--expect-output')
    s.add_parser('docker').add_argument('args',nargs=argparse.REMAINDER)
    a=p.parse_args()
    if a.command not in ['docker','status','doctor']:
        control=(B/'.control.lock').open('a')
        fcntl.flock(control,fcntl.LOCK_EX | fcntl.LOCK_NB)
    if not re.fullmatch(r'[/A-Za-z0-9_.-]+',str(B)) or len(str(STATE/'de/libnetwork/000000000000.sock'))>=108:
        raise RuntimeError('请解压到简短的绝对路径，路径仅用英文字母、数字、斜杠、点、下划线和连字符')
    if a.command=='init':
        require_stopped()
        if OPTIONS.exists():raise RuntimeError('已初始化；停止后可编辑 options.json，再 start。更换 namespace 请使用新的解压目录')
        c=vars(a).copy();c.pop('command')
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}',c['namespace']) or c['namespace'] in ['.','..']:raise RuntimeError('非法 namespace')
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,15}',c['bridge']) or c['bridge'] in ['docker0','lo']:raise RuntimeError('非法/保留网桥名')
        ipaddress.IPv4Address(c['local_ip']);ipaddress.IPv4Interface(c['bridge_cidr'])
        if not all(1024<=c[k]<=65535 for k in ['registry_port','api_port']) or c['registry_port']==c['api_port']:raise RuntimeError('端口应不同且在1024..65535内')
        if not 1<=c['device_tenant']<=255 or not 1<=c['timeout_ms']<=10000:raise RuntimeError('tenant 或 timeout 超出范围')
        n=c['cache_block_size']
        if n<4096 or n>4194304 or n%4096:raise RuntimeError('非法 cache-block-size')
        save(OPTIONS,c);generate(c);print('配置已写入 options.json；下一步 doctor、start。');return
    c=options()
    if a.command in ['verify','verify-missing']:
        if bool(a.image)!=bool(a.manifest):raise RuntimeError('外部验收须同时提供 --image 和 --manifest')
        if a.command=='verify' and a.image and (not a.check_command or a.expect_output is None):raise RuntimeError('外部镜像须提供 --check-command 和 --expect-output')
    if a.command in ['start','stop','fresh-cache'] and os.geteuid()!=0:raise RuntimeError('该命令需要 root')
    if a.command=='doctor':doctor(c)
    elif a.command=='start':require_stopped();generate(c);start(c)
    elif a.command=='stop':stop(c)
    elif a.command=='status':
        for n in ['registry','tcmu','snapshotter','containerd','docker']:print(n,'running' if alive(n) else 'stopped',record(n) or '')
    elif a.command=='seed':seed(c)
    elif a.command=='verify':verify(c,args=a)
    elif a.command=='verify-missing':verify(c,True,a)
    elif a.command=='docker':docker(a.args)
    elif a.command=='fresh-cache':
        require_stopped();cache=B/'data/cache'
        if cache.exists():cache.rename(B/'data'/('cache.saved-'+uuid.uuid4().hex[:8]))
        cache.mkdir();print('旧本地缓存已归档；下一次读取必须查询 Mooncake。')
if __name__=='__main__':
    try: main()
    except (Exception,KeyboardInterrupt) as e:print('错误：'+str(e),file=sys.stderr);sys.exit(1)
