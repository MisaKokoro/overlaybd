# Mooncake 镜像块缓存（实验性功能）

可选的 `PoolFS`/`PoolFile` 层为 Registry 中不可变的 blob 数据提供共享缓存，位于 OverlayBD 现有的本地文件缓存之下：

```text
OverlayBD 层读取器 → 本地文件缓存 → PoolFile
                                     ├─ 查询缓存 → Mooncake Store
                                     └─ 回源读取 → RegistryFS
```

默认的 `fallback="direct"` 模式下，缓存未命中、访问出错、超时、工作队列已满或缓存对象校验失败时，读取请求会通过 RegistryFS 回源。完整块回源成功后，异步写入 Mooncake。缓存写入失败不会导致镜像读取失败；源站读取失败仍会将错误返回给调用方。该功能默认关闭。

当前缓存的是 ZFile 解压前的原始 blob 字节，不负责将 OCI 镜像转换为 OverlayBD 格式。已支持 `moonpool://` URI scheme：配套 snapshotter 将远端只读层写成池地址，OverlayBD 解析后交给 PoolFile。containerd 无需修改，snapshotter 需要配套的路径生成改动。旧的 Registry 透明缓存模式仍可通过 `pool.cacheRegistry` 保留。

## 配置方式

将以下配置合并到 `overlaybd-tcmu` 使用的配置文件中：

```json
{
  "pool": {
    "enable": true,
    "cacheRegistry": false,
    "master": "127.0.0.1:15051",
    "metadataServer": "http://127.0.0.1:18080/metadata",
    "localHostname": "127.0.0.1",
    "nameSpace": "overlaybd",
    "protocol": "tcp",
    "batchBlocks": 16,
    "fallback": "direct",
    "blockSize": 262144,
    "timeoutMs": 200,
    "retryMs": 1000,
    "queueDepth": 32
  }
}
```

适配层通过 `pool.protocol` 选择传输协议，目前仅实现 TCP，不启用 UB 或 RDMA。OverlayBD 中的 Mooncake 客户端不提供存储容量，缓存对象由独立的存储节点持有；因此，要正常回填缓存，需要先启动 Mooncake master 和存储节点。`localHostname` 必须能被后端访问，示例中的回环地址仅适用于单机测试。tcmu 进程启动时必须能由系统动态链接器找到 `libmooncake_store.so` 及其依赖库，例如通过安装目录的加载器配置或 `LD_LIBRARY_PATH` 指定搜索路径。运行时不再使用 `pool.libraryPath`，旧配置中的该字段应删除。

已验证的 C ABI 来自 `boostkit/mooncake` 仓库的 `dev-master_ub_transport` 分支，提交为 `ad3e63a3a599a902ffcc138cc60670771392da8a`，构建选项为 `WITH_STORE_C_SHARED=ON`、`USE_TCP=ON`、`USE_UB=OFF`。由于上游指定的 yalantinglibs 子模块提交不可获取，测试构建改用了 v0.5.7 对应的提交 `73dea196d23ad8fcd4914c6ef1238f390b9a1c48`。尚未验证与其他 SDK 版本的兼容性。适配层包含官方 `store_c.h`，使用其中声明的 `mooncake_store_t` 和函数，并在构建时直接链接现有 C 接口共享库。没有手写接口声明、函数指针表或 `dlopen/dlsym`。启用 Mooncake 的构建需要匹配的头文件和共享库；无需改动或重新编译当前 Mooncake 库，也不引入 Mooncake C++ 头文件或升级 OverlayBD 的 C++ 标准。

直接链接后，共享库是进程启动依赖：缺库或 ABI 不匹配属于部署错误，不能当作缓存未命中处理，即使 `pool.enable=false` 也需要动态库。Mooncake 服务不可达、超时或读写失败由 `pool.fallback` 决定是否回源。

配置约束如下：

| 配置项 | 含义与约束 |
| --- | --- |
| `protocol` | 默认 `tcp`，当前仅支持 TCP。`rdma`、`ub` 明确返回不支持，其他值视为无效配置；不会静默改用 TCP。 |
| `batchBlocks` | 默认 16，范围 1～64。每次 SDK 批量查询的块数上限；批次数据区上限为 64 MiB，另有每槽最多 4096 字节的头部/对齐开销。 |
| `fallback` | 默认 `direct`：允许回源并尽力回填；`none`：仅本地缓存与 Mooncake，缺块、错误或损坏均失败且不回填。 |
| `blockSize` | 缓存块大小，单位为字节；必须是 4096 的整数倍，范围为 4096～4194304。 |
| `timeoutMs` | 单批缓存查询的等待上限，包含排队时间，范围为 1～60000 毫秒；不代表整个镜像读取或源站请求的超时上限。 |
| `retryMs` | 出错后的重试间隔，单位为毫秒，必须大于 0。 |
| `queueDepth` | 待处理操作数量上限，范围为 1～256；此外最多还有一个正在执行的操作。 |
| `nameSpace` | 缓存命名空间，长度为 1～64 个字符，只允许 ASCII 字母、数字、点、下划线和连字符。不同环境应使用不同命名空间。 |

配置不满足约束时，服务初始化会失败。

## 缓存数据与执行机制

显式 URI 的键格式为 `<nameSpace>/v1/b/<digest>/<bs>/<blockIndex>`（公共层）或 `<nameSpace>/v1/t/<tenant>/<digest>/<bs>/<blockIndex>`（私有层）。公共层按摘要共享，私有层按租户隔离；这不代表已实现服务端的租户认证或配额。节点本地文件缓存使用包含命名空间、完整 URI 身份、大小和回源地址的 SHA256 文件名，避免不同租户、版本与旧缓存混用。

兼容透明缓存模式仅缓存以 `/sha256:<64 位小写十六进制摘要>` 结尾的路径，保留第一版键格式：

```text
<nameSpace>/blob-v1/sha256:<digest>/<blockSize>/<blockIndex>
```

缓存值由 64 字节头部和完整数据块组成；blob 末尾不足一个块时，按实际剩余长度保存。头部记录格式标识、数据长度、偏移，以及对缓存键、相关头部字段和数据共同计算的 SHA256。每次命中都检查这些字段和校验值。这能检测意外损坏或缓存键与数据不匹配的情况，但不能防止恶意缓存写入者伪造数据，因此缓存应部署在可信环境中。

SDK 及其注册的临时缓冲区由一个原生工作线程持有。Photon 读取协程在等待期间主动让出执行权，SDK 调用不在 Photon 事件线程上运行。每个任务持有自己的缓冲区，因此超时后仍在执行的操作不会访问调用方缓冲区或已销毁的文件对象。关闭时丢弃待处理任务，正在执行的 SDK 调用则保留相关状态直到完成，无法强制取消。持续出现 SDK 错误时，会延迟重试并重新初始化客户端。

## 编译与测试

准备仓库正常构建依赖和 Mooncake C SDK，然后配置并构建：

```sh
cmake -S . -B <build-dir> \
  -DENABLE_MOONCAKE=ON -DBUILD_MOONPOOL_TESTING=ON \
  -DMOONCAKE_STORE_INCLUDE_DIR=/path/to/mooncake/mooncake-store/include \
  -DMOONCAKE_STORE_LIBRARY=/path/to/libmooncake_store.so
cmake --build <build-dir> --target overlaybd-tcmu moonpool_test
ctest --test-dir <build-dir> -R '^moonpool_test$' --output-on-failure
./build/output/moonpool_test --real
```

`MOONCAKE_STORE_INCLUDE_DIR` 应指向包含 `store_c.h` 的目录。也可用 `MOONCAKE_STORE_ROOT` 指定 SDK 安装前缀，让 CMake 查找头文件和共享库。显式指定静态库会被拒绝。构建机与运行机器都需提供共享库的依赖；部署时不要依赖开发目录的构建 RPATH。

`ENABLE_MOONCAKE` 默认关闭，以保留不安装 SDK 的构建方式。关闭时不会链接 Mooncake；在 direct 模式下 URI 解析仍可用于直接回源，但运行时启用 `pool.enable` 会明确报错，要求重新构建。`pool.enable` 是运行时开关，不能替代构建选项。

默认测试使用注入的模拟后端，覆盖跨块读取、分散读取、文件末尾、短读、校验失败、源站错误、命名空间隔离、队列满、Photon 调度进展，以及对象销毁后异步操作才完成的情况。

`--real` 测试需要前述回环地址上的 Mooncake 服务。测试先回填缓存，再让测试源站读取强制失败，以确认后续读取确实命中 Mooncake。测试使用独立命名空间，产生的少量缓存对象会保留到被淘汰或存储服务重启。

外部测试脚本还可以调用：

```sh
moonpool_test --real-recovery <existing-marker-directory>
```

其中 `<existing-marker-directory>` 是已创建的标记文件目录。测试协调流程为：

1. 等待测试生成 `ready` 文件，然后停止 Mooncake，并创建 `down` 文件。
2. 等待测试生成 `fallback` 文件，然后重启 Mooncake，并创建 `up` 文件。
3. 等待测试结束，检查返回码和日志。

整个过程中保持同一个文件系统对象和客户端存活，验证自动重连、回填，以及后续无需回源的缓存命中。

开发机器上的辅助脚本和测试结果保存在仓库外的 `/home/cyx/overlaybd-mooncake-test` 目录：

| 脚本 | 用途 |
| --- | --- |
| `scripts/build-overlaybd.sh` | 使用测试目录中的独立依赖重新构建。 |
| `scripts/test-overlaybd-moonpool.sh` | 在真实容器中测试缓存关闭、冷缓存、热缓存、后端不可用和恢复后的行为。每个阶段使用全新的本地缓存并重新创建快照，检查文件 SHA256，同时确认镜像层 blob 未被预先完整下载到 containerd 内容存储。 |
| `scripts/test-overlaybd-pool-recovery.sh` | 测试同一客户端在 Mooncake 停机、重启后的回源、重连、回填与缓存命中。 |

## 当前限制

- 显式 URI 使用层配置中的已知大小，延迟到未命中时才打开 RegistryFile；完整热缓存的层数据读取无需源站请求。兼容透明模式仍有打开时的鉴权、重定向和长度探测流量。镜像 manifest/config 拉取仍可能访问 Registry，因此这不等于完整离线运行能力。
- 缓存回填采用尽力而为的方式，尚未实现跨节点合并同一块回源请求、租约、预取、跨请求批次合并、RDMA、容量管理策略或生产级指标接口。调试日志包含 `moonpool hit` 和 `moonpool fallback`，内部计数器可供测试使用。
- 单个工作线程限制了 SDK 并发度，大块读取按有界批次查询；不预读请求范围外的数据，不合并不同请求。性能优化和多机测试尚未开展。
- 对已存在的损坏对象，direct 模式绕过缓存回源，none 模式返回错误；不主动删除或覆盖共享的不可变键。
- 首轮验证在单台 ARM64 机器上完成，使用 containerd 1.7.36、OverlayBD 1.0.18 源码和 overlaybd-snapshotter 1.4.5。目前验证的是基础功能，不代表已达到生产可用性或吞吐性能目标。

## `moonpool://` 层路径契约

公共层和私有层分别使用以下格式：

```text
moonpool://v1/b/sha256:<摘要>?bs=4096&tenant=mooncake
moonpool://v1/t/team-a/sha256:<摘要>?bs=4096&tenant=team-a
```

`<摘要>` 必须替换为真实的 64 位小写十六进制摘要。路径描述整个 blob，块编号由读取偏移计算。解析器拒绝未知版本、无效摘要、重复或未知参数、缺失参数、非法租户，以及私有层路径与参数中的租户不一致。租户字符约束与命名空间相同，且不能单独为 `.` 或 `..`。

**当前 OverlayBD 1.0.18 的 ZFile 最大块大小是 65536 字节。** URI 中 `bs` 必须为 4096～65536 之间的 4096 整数倍，不能照搬设计文档中 524288 的示例。全局 `pool.blockSize` 是兼容透明模式的原始字节块大小；显式 URI 使用各层的 `bs`。

snapshotter 配置示例：

```json
{
  "poolEnable": true,
  "poolFsVersion": "v1",
  "poolTenant": "mooncake",
  "poolPrivate": false
}
```

原有数字型 `tenant` 用于 snapshotter 设备隔离，与新增的 `poolTenant` 不同。`poolPrivate=true` 生成私有层地址，否则生成公共层地址。只对原生 OverlayBD 远端只读层生成 URI，本地层、可写层和 turboOCI 保持原有路径。

生成 URI 需要有效的 blob 摘要、正数 blob 大小，以及该层的 `containerd.io/snapshot/overlaybd/zfile-config` 注解，例如 `{"algorithm":"lz4","blockSize":4096}`。缺少注解、算法或块大小不支持时返回错误，不猜测块大小。旧镜像需要按真实 ZFile 元数据补齐注解，或关闭 `poolEnable`。开关控制新生成的快照，已有快照不会自动改写。

层配置保留原摘要、大小和下载目录，同时新增 `file` 和 `zfileBlockSize`：

```json
{
  "repoBlobUrl": "https://registry.example/v2/example/image/blobs",
  "lowers": [{
    "file": "moonpool://v1/b/sha256:<摘要>?bs=4096&tenant=mooncake",
    "digest": "sha256:<摘要>",
    "size": 123456789,
    "zfileBlockSize": 4096,
    "dir": "/var/lib/overlaybd/snapshots/123/fs"
  }]
}
```

OverlayBD 在访问缓存前校验 URI 与层元数据一致，读取层头部后还会校验 `bs` 与真实 ZFile 头部一致。失败时拒绝打开该层，不静默切换到另一种解释。回源地址由 `repoBlobUrl` 和摘要构造，复用 RegistryFS 的认证、证书与请求逻辑。URI 不携带凭据或任意回源地址；内部编码的回源上下文不是对外参数，在层 URI 中自行附加会被拒绝。

OverlayBD 的 `pool.cacheRegistry` 默认是 `true`，兼容第一版透明缓存。按 scheme 接入建议设为 `false`，此时只有池地址走 Mooncake，普通远端层保持原有读取方式。`pool.enable=false` 仍能解析池地址，但直接回源，不创建 SDK 客户端；已链接的动态库仍是进程启动依赖。

开发机器上的 `scripts/start-overlaybd-scheme.sh` 启动配套二进制，`scripts/test-overlaybd-scheme.sh` 验证 snapshotter 自动生成的 URI、旧路径兼容、关闭缓存、冷读、热读和故障恢复。每阶段使用全新的本地缓存和快照。测试镜像的注解由 `scripts/prepare-scheme-fixture.py` 从实际 ZFile 头部提取后写入独立的 `obd-baseline:scheme` 标签，不修改原 `native` 标签。

`moonpool_test` 另覆盖 URI 错误参数、元数据不一致、公共/私有层与租户隔离，以及新文件对象命中缓存时不打开源站。snapshotter 配套单元测试命令是 `go test ./pkg/snapshot -run TestMoonpoolLayer -count=1`。

实际容器测试结果保存在 `results/overlaybd-scheme-e2e.json`：旧路径、关闭缓存、冷缓存、热缓存、后端不可用、恢复回填和恢复后热读均通过。两次热读各有 3234 次池命中、0 次回源、0 次 Registry blob 请求，所有阶段的文件 SHA256 一致。

`scripts/test-overlaybd-scheme-invalid.sh` 将注解块大小故意写成 8192，真实头部为 4096；容器启动被明确拒绝，结果见 `results/overlaybd-scheme-invalid.json`。`scripts/test-overlaybd-scheme-recovery.sh` 保持同一池 URI、文件对象与工作线程存活，验证后端重启后重连、回填，并在源站读取强制失败时再次命中，结果见 `results/overlaybd-scheme-recovery.json`。

## C 接口直接链接验证

适配层已改为官方 C 头文件加构建时动态链接。CMake 会拒绝缺失头文件、缺失库和误传静态库的配置；未启用 `ENABLE_MOONCAKE` 时不产生 Mooncake 动态库依赖，运行时启用池会明确报错。原先的“缺少 SDK 时静默回源”测试已移除，服务故障回源测试继续保留。测试程序的 `--real` 和 `--real-recovery` 参数不再接收库路径。


## 传输协议、批量读取及禁止回源

传输选择集中在 `moonpool/transport.cpp`，Mooncake 后端使用适配结果调用正式 C SDK。当前只接受 TCP；后续实现 RDMA/UB 时在该适配层补充协议、设备及初始化要求，不需要改动 PoolFile 的读取流程。没有启用 UB 构建，也没有修改 Mooncake 代码。

一次连续读取涉及的块按 `batchBlocks` 分组，通过 `mooncake_store_batch_get_into` 查询；`preadv` 也使用有界中间缓冲区合并连续的向量段。每块分别校验和处理命中/缺失，部分命中时仅缺失或无效块在 direct 模式回源。SDK 缓冲区按批次注册并由原生工作线程持有，超时后继续保持生命周期隔离。批次上限不表示预读范围，也不改变现有缓存键和块格式。

`fallback="none"` 的运行时读取规则是：

1. 优先读取已有的本地文件缓存。
2. 本地未命中时查询 Mooncake；有效块正常返回。
3. 缺块返回 ENOENT；RPC 错误、超时、队列满或校验失败返回 EIO。不会打开 RegistryFile、回源或写入 Mooncake。
4. 此模式要求 `pool.enable=true`，且远端层使用带可信大小的 `moonpool://` URI。普通 Registry URL 缺少可信大小，无法避免打开时的源站探测，因此会被拒绝。
5. 禁止 P2P 代理及后台整层下载，避免绕过策略；缓存关闭与 none 不能同时配置。

### 让错误在 pull 阶段返回

按需拉取只建立远端快照，单独修改 PoolFile 无法保证 pull 阶段发现缺块。严格部署需要启用 OverlayBD 的服务接口，并配置配套 snapshotter 进行预检。

OverlayBD 配置片段：

```json
{
  "pool": {
    "enable": true,
    "protocol": "tcp",
    "batchBlocks": 16,
    "fallback": "none",
    "cacheRegistry": false
  },
  "serviceConfig": {
    "enable": true,
    "address": "http://127.0.0.1:9862"
  },
  "download": {"enable": false},
  "p2pConfig": {"enable": false}
}
```

以上片段需与原有 master、metadataServer、nameSpace 等连接配置合并。snapshotter 配置：

```json
{
  "poolEnable": true,
  "poolPreflightAddress": "http://127.0.0.1:9862"
}
```

`poolPreflightAddress` 必须指向**同一个运行中的 TCMU/ImageService**，使用回环 IP 地址。为空时保留旧的按需拉取行为，不能保证缺块在 pull 阶段暴露；仅设置 OverlayBD 的 fallback 不等于完成严格拉取接入。none 模式未启用 serviceConfig 时，OverlayBD 会拒绝启动。

snapshotter 在提交新远端快照前调用 `POST /pool/check`；已有远端快照的 Stat 路径也进行检查。接口通过当前进程的本地缓存和 PoolFile 顺序读取完整原始层，确保每个块都可读。失败则拒绝 pull，不让 containerd 转为下载完整 layer；新快照事务会回滚。已有快照缺块时返回 Unavailable 并携带缺块原因，避免 containerd 把它误判为元数据不存在后反复返回 AlreadyExists。direct 模式的接口立即跳过全层检查。snapshotter 的单层预检 HTTP 超时为 10 分钟，超时或调用取消均使本次拉取失败。

这一功能不需要修改 Docker 或 containerd 源码，已用 Docker 27.5.1 + containerd 1.7.36 验证。它会增加严格模式的拉取时间并填充本地缓存；现阶段采用全层扫描保证语义，尚未实现完整性清单、租约或缓存保留。检查通过后若缓存再次被逐出，后续读取仍可能失败，但不会回源。

manifest/config 仍由 Docker/containerd 从 Registry 获取；策略只覆盖当前原生 OverlayBD 池层，不能把普通 OCI 镜像自动转换为池镜像。启用预检后拒绝 `rpull --download-blobs` 这种显式整层下载路径。

本轮测试包括：TCP 真实批量 C API；SDK 开/关构建的单元测试；空池和缺单块时 Docker pull 失败且零回源；完整池拉取/运行成功；本地缓存满足读取；已有快照在缓存失效后拒绝拉取；direct 模式冷读回源及热读命中。direct 的并发容器读在默认 200ms 预算下出现过等待失败后回源；2000ms 下已回填块正常命中，但尽力回填可能遗留缺块，需要再次读取才能补齐。这属于当前单工作线程和有界队列的限制，不能保证一次冷读后立即全量热命中。完整池 13410 块通过 839 次批量调用读取，最大批次为 16。仓库外证据见 `results/overlaybd-pool-options.json`。

本轮只增加 protocol、batchBlocks 和 fallback；设计中的多 master、owner-only 写策略、fillLease、watcher 等未在本轮实现，原来的 nameSpace 等配置名称也没有统一重命名。
