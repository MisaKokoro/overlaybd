# Mooncake 大块缓存支持

本次修改基于原生 OverlayBD 分支 `mooncake-cache-v1.0.18` 和 snapshotter 分支 `moonpool-v1.4.5`，不涉及 TurboOCI、Docker、containerd 或 Mooncake 源码。

## 配置方法

OverlayBD 实际加载的配置（通常 `/etc/overlaybd/overlaybd.json`）：

```json
{"pool": {"enable": true, "blockSize": 8388608, "batchBlocks": 16}}
```

snapshotter 实际加载的配置（通常 `/etc/overlaybd-snapshotter/config.json`）：

```json
{"poolEnable": true, "poolBlockSize": 8388608, "poolPrivate": false}
```

这些片段需要合并到现有配置。master、metadataServer、nameSpace 等连接配置仍按实际部署填写；metadataServer 使用部署所要求的地址，例如 etcd://host:port，不要将其随意替换为 HTTP metadata 服务地址。协议目前仍为 TCP。

预热端使用同样的 B=8388608，按原始 Registry blob 切片。公共层示例：

```text
moonpool://v1/b/sha256:<digest>?bs=8388608&tenant=<tenant>
<nameSpace>/v1/b/sha256:<digest>/8388608/<blockIndex>
```

Value 不变：64 字节 OBDPOOL1 头部，加原始 blob 切片；校验包含 Key、头部前 24 字节和 payload。满块 Value 为 B+64 字节，尾块为实际剩余长度加 64。不是解压后的 ZFile 数据。

## 限制和内存行为

取消原来 4 MiB 单块上限及 B×batchBlocks 超过 64 MiB 时拒绝配置的行为。B 仍至少 4096 且为 4096 的整数倍。配置和 URI 改为 64 位整数，不会在超过 4 GiB 时截断；64 位实现的算术上界为 `((2**63 - 1 - 4096) // 4096) * 4096`。该上界仅保证长度计算安全，不表示实际部署可以处理如此大的对象。

每批请求块数最多为 `min(batchBlocks, max(1, floor(64 MiB / B)))`。例如 B=8 MiB 默认最多 8 块，B=80 MiB 每批 1 块。超过 64 MiB 的单块仍需完整 GET，不能把现有对象内部再切成不同 Key。batchBlocks 仍为 1～64。

注册区按实际 Value 长度加页对齐分配；还有 SDK、结果副本、回源待写队列等内存，64 MiB 不是进程总内存限制。尾块按实际 payload 分配。分配或注册失败明确返回错误，direct 模式也不会以回源掩盖资源错误。SDK/存储节点能否接受和保存大对象仍取决于实际资源与 SDK 实现；timeoutMs 仍有效。

## 修改位置

- OverlayBD `src/overlaybd/moonpool/limits.h`：统一块大小、批量数量、注册容量的安全计算。
- `uri.h`、`uri.cpp`：64 位 bs，移除 4 MiB 校验，保留摘要、来源、blob 长度校验。
- `src/config.h`、`src/image_service.cpp`：pool.blockSize 接受 64 位无符号整数。
- `pool_fs.cpp`：按块大小自动拆批；preadv 中间缓冲不再使用可能溢出的 B×batchBlocks；分配失败返回错误。
- `mooncake_backend.cpp`：注册容量由本次实际 Value 大小计算，检查加法、乘法和对齐溢出，继续使用正式 C 头文件和动态链接。回填改用 `mooncake_store_put_from`，复用已注册缓冲区，避免 `put` 依赖的固定 SDK 内部 staging 缓冲区限制。
- snapshotter `pkg/snapshot/moonpool.go`、`overlay.go`：poolBlockSize 使用 64 位整数，生成新 URI，移除 4 MiB 校验。
- `tools/portable-test/test.py`、`pool-seed.cpp` 和独立测试套件对应文件：配置和预热支持大块，seed 对小于 B 的 blob 只分配实际长度。

## 更新和已有数据

需要同时更新并重新编译 OverlayBD、snapshotter，以及使用到的 pool-seed。只改配置无法让旧二进制接受大块。独立测试套件的脚本更新不能替代这些组件的二进制更新。

两端配置与预热使用相同大小；更改配置不会改写已有快照的 URI。已有 URI 继续按自身 bs 读，旧 Key 与新 Key 因大小字段不同而并存。若要切换现有镜像的缓存大小，应通过容器运行时正常清理并重新生成相关快照，勿直接删除运行中的 data 目录。准备好新大小的预热数据后，再使用 fallback=none 验证。

## 本次验证（2026-11-26）

在 192.168.25.140 的 ARM64 测试机上完成：

- overlaybd-tcmu（ENABLE_MOONCAKE=ON）、snapshotter 和两份 pool-seed 编译成功。
- C++ 单元测试、Go TestMoonpool 测试通过；验证自动拆批、长度溢出、超过 32 位的 bs、内存分配错误不回源。
- 真实 TCP Mooncake C SDK 验证 4 KiB、256 KiB、4 MiB、8 MiB、80 MiB，包括回填、命中、跨块 preadv、尾块和缓冲扩容。
- 独立 pool-seed 以 8 MiB 和 80 MiB 预热 80 MiB+137 字节的原始测试 blob，随后 PoolFile 用公共 URI、fallback=none 逐字节校验整层；没有源站读取或回填，Key/头部/校验契约一致。
- 大于 4 GiB 仅测试配置、URI 和小尾块处理，未分配或传输完整的 4 GiB 对象。没有重新执行完整 Docker pull 验收。

源码对应上述 OverlayBD 和 snapshotter 分支。编译产物汇总在 `/home/cyx/overlaybd-mooncake-test/results/large-blocks-20261126/`；日志位于测试目录 `logs/large-block-*` 及该 results 目录。运行中的服务和旧测试包二进制未替换，使用新功能时需部署本次构建产物并重启相应组件。
