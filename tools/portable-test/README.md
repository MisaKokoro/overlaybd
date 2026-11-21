# Mooncake + OverlayBD 隔离迁移测试包

适用：ARM64、openEuler、root、Python 3.8 以上。无需预装 Docker/containerd，无需联网拉镜像。
包内提供 Docker 27.5.1、containerd 1.7.36、修改版 OverlayBD 1.0.18、snapshotter 1.4.5、本地 Registry 和已经转换的 ARM64 测试镜像。

**不打包、不安装、不启动 Mooncake。** 使用系统安装的 `libmooncake_store.so` 和现有 master、metadata、storage 服务。系统 SDK 需要提供与 `boostkit/mooncake` 的 `dev-master_ub_transport` 分支构建一致的 C ABI，特别是 `mooncake_store_batch_get_into`。原需求分支名写作 `dev_master_ub_transport`，实际验证源码分支为前述连字符名称；准确提交见 `VERSIONS.json`。当前仅 TCP，未启用 UB。

## 最快跑通

将 tar.gz 和 sha256 文件复制到目标机器，使用 root：

```bash
sha256sum -c obd-mooncake-arm64.tar.gz.sha256
tar -xzf obd-mooncake-arm64.tar.gz -C /opt
cd /opt/obd-mooncake-arm64
sha256sum -c SHA256SUMS

# local-ip 必须是当前测试节点上能被 Mooncake 存储节点访问的 IPv4。
# 以下连接地址是示例，请替换成实际部署的值。
./test.sh init \
  --master 10.0.0.11:9003 \
  --metadata http://10.0.0.11:8080/metadata \
  --local-ip 10.0.0.21

./test.sh doctor
./test.sh start
./test.sh verify-missing   # 新随机命名空间、空本地缓存：应因缺块导致 pull 失败
./test.sh seed             # 将包内测试镜像的全部层块写入指定 Mooncake 测试命名空间
./test.sh verify           # docker pull、启动容器、校验文件、检查零回源和真实批量命中
./test.sh stop
```

默认 `fallback=none`、`batchBlocks=16`、`protocol=tcp`、缓存块大小 256 KiB（`--cache-block-size 262144`），`timeoutMs=2000`。这里将测试等待预算放宽为 2 秒以减少负载引起的等待失败；产品默认值仍是 200ms，可用 `--timeout-ms 200` 验证。`verify-missing` 只有在错误明确来自预检 HTTP 404 缺块时才算通过，连接失败或其他错误不会冒充通过。

`seed` 只是迁移验证用的预热工具，使用正式 C 头文件、构建时链接 SDK，按现有块键和校验格式写入。它不会删除对象或清理池，也不是生产环境的镜像发布流程。当前 TCP SDK 会产生较多短连接。脚本在预热前后和验证前检查临时端口余量，不足时等待 TIME_WAIT 自然释放并打印进度（最多 90 秒），不会修改 sysctl；不要将等待误认为卡死。默认抑制 Mooncake INFO 级日志，可设置 `GLOG_minloglevel=0` 查看详细 SDK 日志。

测试镜像原始层约 55 MB、默认 256 KiB 切片时 211 块，请确保存储节点有足够空间（建议至少 128 MiB 空闲，包含分配器开销）；同名对象已存在时不覆盖。

第一次成功的 `verify` 应看到 `pool_hits > 0`、`max_batch <= 16`、`registry_layer_requests = 0`。验证既读取镜像 marker，也计算确定性 8 MiB 文件的 SHA256。它还检查完整层 blob 未被下载到 containerd 内容存储。结果位于 `results/verify-*.json`。

## SDK 与操作系统依赖

如果 `libmooncake_store.so` 已通过系统动态链接器加载，init 不需要额外参数。如果安装在未加入链接器搜索路径的目录：

```bash
./test.sh init --master 10.0.0.11:9003 \
  --metadata http://10.0.0.11:8080/metadata --local-ip 10.0.0.21 \
  --mooncake-lib-dir /usr/local/lib64
```

SDK 的其他私有依赖若在不同目录，可以用冒号分隔多个目录。脚本只设置本目录进程的 `LD_LIBRARY_PATH`，不修改 `/etc/ld.so.conf`。包内只附带 OverlayBD 所需的私有 libext2fs，不包含 Mooncake 库，也不复制 libc、libstdc++、OpenSSL 等系统库。

`doctor` 会检查架构、动态库解析及关键 C 符号。库中存在符号不能替代 ABI 兼容性保证，后续 `seed` 和 `verify` 才是实际连通性验证。若 SDK 只安装了 C++ 库、没有 C shared library，需要安装相匹配的 C SDK。运行本包不需要 SDK 头文件或编译器。

本包在现有 ARM64/openEuler 验证机上构建，准确构建系统和 glibc 见 `VERSIONS.json`。不同 openEuler 版本若出现 `GLIBC_xxx not found` 等错误，应在目标环境或更老的兼容构建环境重建 OverlayBD，不能替换目标机 libc 来规避。常见系统依赖包括 libcurl、OpenSSL 3、libaio、libnl3、libstdc++，以及 Mooncake 自身依赖；以 doctor 的 ldd 输出为准，不自动运行系统包安装。

内核必须支持 `target_core_user`、`tcm_loop`、configfs、overlayfs 和 ext4。脚本按需加载模块和挂载 configfs；停止时不卸载这些主机资源。不要在不支持这些内核接口的普通容器里运行。

## 全部组件手动管理

```bash
./test.sh status
./test.sh docker version
./test.sh docker info
./test.sh docker pull 127.0.0.1:16000/obd-baseline:pool
./test.sh docker run --rm --network none \
  127.0.0.1:16000/obd-baseline:pool cat /opt/obd-test/marker.txt
./test.sh stop
```

`start` 用普通后台进程启动五个组件：Registry、overlaybd-tcmu、snapshotter、containerd、dockerd。无 systemd unit、无开机启动，不安装到系统 PATH，不占用 `/var/run/docker.sock`。关闭终端后进程继续运行；用 `stop` 停止，不使用 `killall`。

- 二进制：`bin/`
- 配置：`options.json` 和生成的 `config/`
- socket/PID：`state/`
- 日志：`logs/`，其中 `overlaybd.log` 包含命中、批量及回源记录
- 数据：`data/`、`registry-data/`
- 证据：`results/`

`test.sh docker` 始终指向本目录 socket，不会误连系统 Docker。原生 `docker` 命令如未指定 `--host`，不会连接这个实例。默认测试容器使用 `--network none`；此工具不配置容器访问外网所需的 NAT。

## 再次验证 Mooncake 读取

重复 verify 可能全部命中本地文件缓存。需要排除本地缓存时：

```bash
./test.sh stop
./test.sh fresh-cache      # 归档本目录旧缓存，保留原数据
./test.sh start
./test.sh verify
./test.sh stop
```

此时 snapshotter 中已有的快照也会重新预检。严格模式会检查全部原始层，填充本地缓存，增加拉取时间；尚未实现轻量完整性清单或缓存保留租约。manifest/config 始终来自包内 Registry，零回源只指层数据没有从 Registry 获取，并不表示完全跳过 Registry。

## 配置与隔离边界

初始化生成随机私有 `namespace`。更换 namespace 或连接另一个池做全新测试时，使用新的解压目录；不要复用已有快照的数据目录。停止后可修改 `options.json` 的 `fallback`（`none`/`direct`）、`timeout_ms` 或连接地址，再执行 start 重新生成组件配置。不要直接编辑 `config/`，启动时会重建它。

默认回环端口 16000（Registry）、16056（预检），网桥 `obdp-test0`、网段 `172.30.253.1/24`、snapshotter 设备 tenant 30。端口或网段冲突可以在 init 时指定 `--registry-port`、`--api-port`、`--bridge`、`--bridge-cidr`、`--device-tenant`。其他实例不能共用设备 tenant。

Docker 使用独立且带归属标记的网桥，不使用 `bridge=none`，避免 Docker 27 的该配置对已有 docker0 的影响。不修改 iptables、ip6tables、IP forwarding 或系统 Docker/containerd 配置。停止时仅删除带本目录归属标记的网桥。

TCMU 使用全局内核资源，**不能承诺与另一个 overlaybd-tcmu 实例完全隔离**。脚本发现其他实例会拒绝启动，需要先由操作者安排测试窗口。它不会自动停止其他实例。stop 会检查本目录容器、任务和挂载，存在使用中的设备则拒绝停止；日志和数据保留。没有一键删除共享 Mooncake 数据的操作。

## 从验证工作区重新打包

脚本源文件可存放在 OverlayBD 仓库 `tools/portable-test/`。在 ARM64 验证机上：

```bash
python3 tools/portable-test/package.py \
  --workspace /home/cyx/overlaybd-mooncake-test \
  --output /home/cyx/overlaybd-mooncake-test/releases/obd-mooncake-arm64
```

该导出脚本依赖当前验证工作区的目录结构和已经构建的二进制，需要 g++、patchelf、strip、Mooncake C SDK 开发头文件。它不是通用源码构建器。产物含 VERSIONS.json、SHA256SUMS 和压缩包校验文件；会去掉 OverlayBD 二进制中的旧实验绝对 RPATH，通过系统路径加载目标机的 Mooncake。

## 独立缓存块大小

此版本将 `--cache-block-size` 同时写入 OverlayBD 的 `pool.blockSize`、snapshotter 的 `poolBlockSize`，并用于 seed 的原始 blob 切片。范围为 4096～4194304，必须为 4096 的整数倍。它与 ZFile 压缩块大小无关，不再读取 zfile-config 注解决定缓存块大小。

自带测试镜像来自未补注解的 native 原始 manifest，包内标签为 `obd-baseline:pool`，层内 ZFile 为 4 KiB，默认池块为 256 KiB。Key/Value 仍遵守 moonpool-v1 / OBDPOOL1 格式。此前的测试包不具备此解耦功能。

按当前范围，不提供旧测试目录、旧快照或旧缓存迁移兼容。请使用新解压目录、全新 snapshotter 数据目录及新的预热命名空间。改块大小时需要生成新快照和重新预热，单独 fresh-cache 不会重写已有 URI。打开层始终以其 URI 的 bs 为准；全局配置不覆盖它。

## 对接外部预热工具

不需要运行本包的 seed。由外部工具按 options.json 的 namespace（本包 tenant 与其相同）、private 范围和 cache_block_size 写入完整镜像层。将同一转换产物发布到 Registry，提供最终单平台 manifest 文件及镜像引用，然后执行：

```bash
./test.sh verify \
  --image registry.example/team/image@sha256:实际manifest摘要 \
  --manifest /data/artifact/blobs/sha256/实际manifest摘要 \
  --check-command 'sha256sum /path/to/known-file' \
  --expect-output '预期SHA256  /path/to/known-file'
```

镜像摘要必须与提供的 manifest 文件一致。测试命令在容器的 /bin/sh 内执行，stdout 去掉首尾空白后必须与 expect-output 完全相等；不硬编码自带镜像的 marker。verify-missing 也接受 image/manifest，用于外部空池缺块测试。文件摘要应由镜像发布方独立提供。

外部 Registry 需已满足网络、TLS 和认证条件；本包的匿名 HTTP 放行默认仅针对自带 Registry，可先用独立发布工具将镜像放到自带 Registry。verify 只拉取、运行和观察，不执行预热。Registry 回源计数仅覆盖自带 Registry 的日志；使用外部 Registry 时必须另收集该 Registry 的访问记录，不能把本地日志零请求当作外部零回源证据。
