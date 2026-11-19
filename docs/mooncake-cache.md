# Mooncake 镜像块缓存（实验性功能）

可选的 `PoolFS`/`PoolFile` 层为 Registry 中不可变的 blob 数据提供共享缓存，位于 OverlayBD 现有的本地文件缓存之下：

```text
OverlayBD 层读取器 → 本地文件缓存 → PoolFile
                                     ├─ 查询缓存 → Mooncake Store
                                     └─ 回源读取 → RegistryFS
```

缓存未命中、访问出错、超时、工作队列已满或缓存对象校验失败时，读取请求会通过 RegistryFS 回源。完整块回源成功后，异步写入 Mooncake。缓存写入失败不会导致镜像读取失败；源站读取失败仍会将错误返回给调用方。该功能默认关闭。

当前缓存的是 ZFile 解压前的原始 blob 字节，不改变镜像 manifest，也不负责将 OCI 镜像转换为 OverlayBD 格式。镜像准备和 snapshotter 拉取仍沿用现有流程。第一版尚未实现 `moonpool://` URI scheme，通过 OverlayBD 全局配置启用缓存包装层即可，无需修改 containerd 或 snapshotter 的代码。

## 配置方式

将以下配置合并到 `overlaybd-tcmu` 使用的配置文件中：

```json
{
  "pool": {
    "enable": true,
    "libraryPath": "/absolute/path/to/libmooncake_store.so",
    "master": "127.0.0.1:15051",
    "metadataServer": "http://127.0.0.1:18080/metadata",
    "localHostname": "127.0.0.1",
    "nameSpace": "overlaybd",
    "blockSize": 262144,
    "timeoutMs": 200,
    "retryMs": 1000,
    "queueDepth": 32
  }
}
```

适配层使用 TCP，不启用 UB。OverlayBD 中的 Mooncake 客户端不提供存储容量，缓存对象由独立的存储节点持有；因此，要正常回填缓存，需要先启动 Mooncake master 和存储节点。`localHostname` 必须能被后端访问，示例中的回环地址仅适用于单机测试。tcmu 进程必须能够加载 `libraryPath` 指定的库及其依赖库，例如通过 `LD_LIBRARY_PATH` 指定依赖库搜索路径。

已验证的 C ABI 来自 `boostkit/mooncake` 仓库的 `dev-master_ub_transport` 分支，提交为 `ad3e63a3a599a902ffcc138cc60670771392da8a`，构建选项为 `WITH_STORE_C_SHARED=ON`、`USE_TCP=ON`、`USE_UB=OFF`。由于上游指定的 yalantinglibs 子模块提交不可获取，测试构建改用了 v0.5.7 对应的提交 `73dea196d23ad8fcd4914c6ef1238f390b9a1c48`。尚未验证与其他 SDK 版本的兼容性。适配层通过动态加载调用 C ABI，OverlayBD 编译时不包含 Mooncake C++ 头文件，也不要求安装 Mooncake SDK。运行时缺少库或必要符号时，读取请求会回源。

配置约束如下：

| 配置项 | 含义与约束 |
| --- | --- |
| `blockSize` | 缓存块大小，单位为字节；必须是 4096 的整数倍，范围为 4096～4194304。 |
| `timeoutMs` | 单次缓存查询的等待上限，包含排队时间，范围为 1～60000 毫秒；不代表整个镜像读取或源站请求的超时上限。 |
| `retryMs` | 出错后的重试间隔，单位为毫秒，必须大于 0。 |
| `queueDepth` | 待处理操作数量上限，范围为 1～256；此外最多还有一个正在执行的操作。 |
| `nameSpace` | 缓存命名空间，长度为 1～64 个字符，只允许 ASCII 字母、数字、点、下划线和连字符。不同环境应使用不同命名空间。 |

配置不满足约束时，服务初始化会失败。

## 缓存数据与执行机制

仅缓存以 `/sha256:<64 位小写十六进制摘要>` 结尾的路径，其他文件直接访问下层文件系统。缓存键格式为：

```text
<nameSpace>/blob-v1/sha256:<digest>/<blockSize>/<blockIndex>
```

缓存值由 64 字节头部和完整数据块组成；blob 末尾不足一个块时，按实际剩余长度保存。头部记录格式标识、数据长度、偏移，以及对缓存键、相关头部字段和数据共同计算的 SHA256。每次命中都检查这些字段和校验值。这能检测意外损坏或缓存键与数据不匹配的情况，但不能防止恶意缓存写入者伪造数据，因此缓存应部署在可信环境中。

SDK 及其注册的临时缓冲区由一个原生工作线程持有。Photon 读取协程在等待期间主动让出执行权，SDK 调用不在 Photon 事件线程上运行。每个任务持有自己的缓冲区，因此超时后仍在执行的操作不会访问调用方缓冲区或已销毁的文件对象。关闭时丢弃待处理任务，正在执行的 SDK 调用则保留相关状态直到完成，无法强制取消。持续出现 SDK 错误时，会延迟重试并重新初始化客户端。

## 编译与测试

准备仓库正常构建所需的依赖，并在 CMake 配置中增加 `-DBUILD_MOONPOOL_TESTING=ON`，随后执行：

```sh
cmake --build <build-dir> --target overlaybd-tcmu moonpool_test
ctest --test-dir <build-dir> -R '^moonpool_test$' --output-on-failure
./build/output/moonpool_test --real /absolute/path/to/libmooncake_store.so
```

默认测试使用注入的模拟后端，覆盖跨块读取、分散读取、文件末尾、短读、校验失败、缺少动态库、源站错误、命名空间隔离、队列满、Photon 调度进展，以及对象销毁后异步操作才完成的情况。

`--real` 测试需要前述回环地址上的 Mooncake 服务。测试先回填缓存，再让测试源站读取强制失败，以确认后续读取确实命中 Mooncake。测试使用独立命名空间，产生的少量缓存对象会保留到被淘汰或存储服务重启。

外部测试脚本还可以调用：

```sh
moonpool_test --real-recovery <library> <existing-marker-directory>
```

其中 `<library>` 是动态库路径，`<existing-marker-directory>` 是已创建的标记文件目录。测试协调流程为：

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

- RegistryFS 仍需要打开源站 blob，并执行鉴权、重定向和长度探测。部分探测使用普通 GET，请求关闭前可能已经传输了响应体。因此，即使所有热缓存块读取都命中 Mooncake，仍可能存在 Registry 流量，目前不能脱离 Registry 离线运行。
- 缓存回填采用尽力而为的方式，尚未实现跨节点合并同一块回源请求、租约、预取、批量优化、RDMA、容量管理策略或生产级指标接口。调试日志包含 `moonpool hit` 和 `moonpool fallback`，内部计数器可供测试使用。
- 单个工作线程限制了 SDK 并发度，大块读取可能逐块等待缓存查询。性能优化和多机测试尚未开展。
- 对已存在的损坏对象，只绕过缓存回源，不主动删除或覆盖共享的不可变键。这类对象在被淘汰或切换命名空间前，可能持续触发回源。
- 首轮验证在单台 ARM64 机器上完成，使用 containerd 1.7.36、OverlayBD 1.0.18 源码和 overlaybd-snapshotter 1.4.5。目前验证的是基础功能，不代表已达到生产可用性或吞吐性能目标。
