#!/usr/bin/env python3
"""在现有验证机上导出 ARM64 二进制包；不包含 Mooncake 库、连接凭据或运行时缓存。"""
import argparse, hashlib, json, os, pathlib, platform, shutil, subprocess, tarfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--workspace',required=True,type=pathlib.Path)
p.add_argument('--output',required=True,type=pathlib.Path)
a=p.parse_args();w=a.workspace.resolve();b=a.output.resolve();src=pathlib.Path(__file__).resolve().parent
if platform.machine()!='aarch64':raise SystemExit('请在 ARM64 构建机运行')
if b.exists() or pathlib.Path(str(b)+'.tar.gz').exists():raise SystemExit('输出路径已存在，请换一个新目录')
(b/'bin').mkdir(parents=True);(b/'lib').mkdir();(b/'fixture').mkdir()
for name in ['test.sh','test.py','README.md','pool-seed.cpp','package.py','test_controls.py']:
 shutil.copy2(src/name,b/name)
shutil.copy2(w/'overlaybd/docs/moonpool-v1-缓存预热与读取对接协议.md',b/'MOONPOOL-PROTOCOL.md')
for name in ['containerd','ctr','containerd-shim-runc-v2']:
 shutil.copy2(w/'runtime/bin'/name,b/'bin'/name)
for name in ['docker','dockerd','docker-init','docker-proxy','runc']:
 shutil.copy2(w/'runtime/docker-27.5.1'/name,b/'bin'/name)
for origin,name in [('runtime/registry/registry','registry'),('overlaybd/build/output/overlaybd-tcmu','overlaybd-tcmu'),('snapshotter/bin/overlaybd-snapshotter','overlaybd-snapshotter')]:
 shutil.copy2(w/origin,b/'bin'/name)
shutil.copy2(w/'deps/overlaybd/e2fsprogs/build/libext2fs/lib/libext2fs.so.2',b/'lib/libext2fs.so.2')
subprocess.run(['g++','-O2','-Wno-deprecated-declarations',str(src/'pool-seed.cpp'),'-I'+str(w/'mooncake/mooncake-store/include'),
 str(w/'build-mooncake/mooncake-store/src/shared/libmooncake_store.so'),'-lcrypto','-Wl,--allow-shlib-undefined','-o',str(b/'bin/pool-seed')],check=True)
for name in ['overlaybd-tcmu','pool-seed']:
 subprocess.run(['patchelf','--set-rpath','$ORIGIN/../lib',str(b/'bin'/name)],check=True)
 subprocess.run(['strip','--strip-debug',str(b/'bin'/name)],check=True)
# 只导出无 zfile-config 的原生标签及其 blobs，不复制原 Registry 的其他仓库和凭据。
registry_root=w/'data/registry/docker/registry/v2'
native=(registry_root/'repositories/obd-baseline/_manifests/tags/native/current/link').read_text().strip()
raw=(registry_root/'blobs/sha256'/native[7:9]/native[7:]/'data').read_bytes();manifest=json.loads(raw)
assert all('containerd.io/snapshot/overlaybd/zfile-config' not in l.get('annotations',{}) for l in manifest['layers']), 'fixture must have no zfile-config annotation'
md='sha256:'+hashlib.sha256(raw).hexdigest();root=b/'registry-data/docker/registry/v2'
def blob(digest,data):
 assert hashlib.sha256(data).hexdigest()==digest[7:]
 dest=root/'blobs/sha256'/digest[7:9]/digest[7:]/'data';dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
def link(relative,digest):
 dest=root/'repositories/obd-baseline'/relative;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(digest)
for item in [manifest['config'],*manifest['layers']]:
 d=item['digest'];original=w/'data/registry/docker/registry/v2/blobs/sha256'/d[7:9]/d[7:]/'data';data=original.read_bytes()
 assert len(data)==item['size'];blob(d,data);link('_layers/sha256/'+d[7:]+'/link',d)
blob(md,raw);link('_manifests/revisions/sha256/'+md[7:]+'/link',md)
link('_manifests/tags/pool/current/link',md);link('_manifests/tags/pool/index/sha256/'+md[7:]+'/link',md)
(b/'fixture/manifest.json').write_bytes(raw);shutil.copy2(w/'results/expected.json',b/'fixture/expected.json')
versions={'architecture':platform.machine(),'build_os':pathlib.Path('/etc/os-release').read_text(),'glibc':platform.libc_ver(),
 'docker':'27.5.1','containerd':'1.7.36','overlaybd_base':'1.0.18','snapshotter_base':'1.4.5','mooncake_included':False}
for repo in ['overlaybd','snapshotter','mooncake']:
 versions[repo+'_commit']=subprocess.check_output(['git','-C',str(w/repo),'rev-parse','HEAD'],text=True).strip()
for repo in ['overlaybd','snapshotter']:
 versions[repo+'_diff_sha256']=hashlib.sha256(subprocess.check_output(['git','-C',str(w/repo),'diff','HEAD','--binary'])).hexdigest()
 versions[repo+'_dirty']=bool(subprocess.check_output(['git','-C',str(w/repo),'status','--porcelain'],text=True).strip())
(b/'VERSIONS.json').write_text(json.dumps(versions,indent=2)+'\n')
checks=[]
for f in sorted(b.rglob('*')):
 if f.is_file():checks.append(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(b)))
(b/'SHA256SUMS').write_text('\n'.join(checks)+'\n')
archive=pathlib.Path(str(b)+'.tar.gz')
with tarfile.open(archive,'w:gz',compresslevel=3) as t:t.add(b,arcname=b.name)
pathlib.Path(str(archive)+'.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n')
print(archive,archive.stat().st_size,'bytes')
