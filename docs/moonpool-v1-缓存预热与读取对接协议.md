# moonpool-v1 缓存预热与读取对接协议

更新日期：2026-11-21。

用途：供 OCIOverlayBD 预热工具、OverlayBD/snapshotter 读取端和联调调用方共同实现与验收。本文规定可互操作的数据格式和职责，不要求双方互相引用实现代码。

## 1. 状态与适用范围

**本文描述已实施的“缓存块大小与 ZFile 压缩块大小解耦”契约。2026-11-21 已在远端 ARM64 验证机完成真实 Mooncake 与 Docker 拉取验证；新环境仍需使用本次构建产物验收。**

本次改动基于 OverlayBD `1a1ec5f`、snapshotter `b42fccc` 的工作区，尚未提交。上述原始提交及旧迁移包仍有块大小耦合，不能只修改配置就用于本协议验收。新版脚本通过 `--cache-block-size` 统一生成两端配置和预热参数。

外部预热工具需要独立实现本协议并与本次读取端通过第 12 节验收，才能声明互操作。本文保留 URI 的 `v1`、Key 结构和 `OBDPOOL1` Value 编码；这次修改不引入新的 Value 格式。

本轮范围：

- 原生 OverlayBD 远端只读层的原始 blob 字节缓存。
- Mooncake Store 对象的 Key、Value、切块规则及生命周期约定。
- 当前使用 TCP；不修改 Docker、containerd 或 Mooncake 源码，不启用 UB。
- 不实现 manifest/config 的 Mooncake 缓存、Registry 网关、旧格式兼容、旧块自动重组或缓存保留租约。

文中的“必须”是互操作要求；“建议”是实施建议。“全预热”指选定平台镜像的所有必需远端层数据块均已写入并确认可读，不表示从此永久存在。

## 2. 核心约定：两个块大小独立

| 名称 | 本文符号 | 来源与用途 |
| --- | --- | --- |
| ZFile 压缩块大小 | Z | 转换阶段确定，记录在真实 ZFile 头部，由原有 ZFile 解析逻辑使用。 |
| Mooncake 缓存块大小 | B | 预热和读取配置确定；对完整原始 blob 等长切片，参与 Key 和读取偏移计算。 |
| 原始层 blob 长度 | L | 实际原始 blob 字节数，必须与层元数据一致。 |

**B 不要求等于 Z。** 例如 Z=4096、B=262144 是必须支持的联调场景。

缓存处理的是原始 blob 的字节范围。即使一次请求跨越多个 ZFile 压缩块，或者一个 ZFile 编码记录跨越缓存块边界，也必须返回与原始 blob 完全相同的字节。

`containerd.io/snapshot/overlaybd/zfile-config`：

- 不再是启用 Mooncake 缓存的必填注解。
- 不参与 B 的选择，不得把 B 写进表示真实 ZFile 参数的字段。
- 若实现保留对已有注解的验证，该验证必须独立于缓存块大小；无注解不能成为拒绝缓存接入的原因。
- 原有 ZFile 解析与格式检查继续保留。取消 B=Z 约束不意味着接受损坏的 ZFile。

因此，接入缓存不要求重新转换有效镜像，也不要求补写该注解或改变原 manifest digest。

## 3. 配置、优先级与部署对齐

### 3.1 缓存块大小范围

B 使用字节为单位的整数：

- 最小 4096（4 KiB）。
- 最大 4194304（4 MiB）。
- 必须为 4096 的整数倍。
- 新配置统一默认 262144（256 KiB）。

无效配置必须在生成 URI、预热写入或打开层之前明确失败，不能自动取整或静默改成另一个大小。

### 3.2 三方配置映射

| 含义 | 预热端 | OverlayBD | snapshotter |
| --- | --- | --- | --- |
| 新数据的缓存块大小 | 独立的缓存块配置；现有 `ChunkSize` 可承担此含义 | `pool.blockSize` | 新增 `poolBlockSize` |
| Key 命名空间 | `Namespace` | `pool.nameSpace`，注意大小写 | 当前无独立对应字段 |
| 租户标识 | `Tenant`，建议新增 | 来自层 URI | `poolTenant` |
| 公共/私有范围 | 明确的 `Scope` 或等价选项，建议新增 | 来自层 URI | `poolPrivate` |
| 协议 | TCP | `pool.protocol="tcp"` | 不控制 SDK 传输协议 |

预热工具的字段名可以自行设计，但必须在接口中明确这些独立语义；不能只传 Namespace 然后猜测 tenant 或公共/私有范围。

首次联调建议统一设置：

```text
Namespace = obd-pool
Tenant    = mooncake
Scope     = private
B         = 262144
```

OverlayBD 配置片段：

```json
{
  "pool": {
    "enable": true,
    "nameSpace": "obd-pool",
    "protocol": "tcp",
    "blockSize": 262144,
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

snapshotter 配置片段：

```json
{
  "poolEnable": true,
  "poolFsVersion": "v1",
  "poolBlockSize": 262144,
  "poolTenant": "mooncake",
  "poolPrivate": true,
  "poolPreflightAddress": "http://127.0.0.1:9862"
}
```

以上片段需合并已有 master、metadataServer、localHostname、日志和数据目录等配置。各进程的 localHostname 是本机可被其他节点访问的地址，不要求上传端和读取端填同一 IP。

### 3.3 优先级

1. **打开某个已有层时，URI 中的 `bs` 是唯一的缓存块大小依据。**
2. snapshotter 的 `poolBlockSize` 只决定新生成 URI 的 `bs`，不会改写已有快照。
3. OverlayBD 的 `pool.blockSize` 是全局默认配置，兼容原有透明缓存用途；不得覆盖具体层 URI 的 `bs`。
4. 预热端必须使用消费者实际需要的 B。本版本某 URI 为 4096 时，即使全局配置为 262144，该层仍按 4096 的 Key 读取。

新部署应把三端配置显式设为相同 B。不同进程的配置文件不会自动同步；建议由调用方用一份部署参数生成三端配置，并在联调记录中输出最终生效值。

迁移测试包此前默认生成随机 namespace，并将 tenant 设为相同值。联调时必须读取实际配置，不能假定它固定为 `obd-pool/mooncake`。

## 4. 预热输入与完整性

上传器读取已完成、不可变的 OCI Layout，或等价的经过验证的原始层 blob。Converter 与 Uploader 保持独立，不要求在同一进程运行。

每个必需远端层至少提供：

- digest：完整的 `sha256:<64位小写十六进制>`。
- size：实际 blob 长度 L，必须为正数。
- 原始 blob 字节。
- B、namespace、tenant、scope。

digest 和 size 必须与读取端使用的层描述一致。当前 snapshotter 使用 `containerd.io/snapshot/overlaybd/blob-digest`、`containerd.io/snapshot/overlaybd/blob-size`；原生层的对应 annotations、descriptor 和实际数据应一致。不使用镜像 manifest digest、config digest、Docker ImageID 或 diffID 代替层 blob digest。

预热前必须验证：

```text
实际文件字节数 == L
SHA256(完整原始 blob) == digest 中的摘要
```

**原始 blob 是 Registry 该 digest 对应的完整响应体。** 必须保留实际存在的封装、头部、压缩数据、索引及尾部；不解压，不去掉 tar 封装，不仅提取 ZFile，不按容器文件切块，也不再对块追加另一层压缩或 Base64 编码。

首轮按单一选定平台（例如 linux/arm64）的最终 manifest 验收；多平台索引的遍历和发布由调用方另行明确。

## 5. URI 与 Key

### 5.1 层 URI

```text
公共层：moonpool://v1/b/<digest>?bs=<B>&tenant=<tenant>
私有层：moonpool://v1/t/<tenant>/<digest>?bs=<B>&tenant=<tenant>
```

`bs` 明确定义为缓存块大小。公共 URI 仍携带 tenant 参数；私有 URI 的路径 tenant 与参数 tenant 必须一致。

### 5.2 Store Key

```text
公共块：<namespace>/v1/b/<digest>/<B>/<i>
私有块：<namespace>/v1/t/<tenant>/<digest>/<B>/<i>
```

示例（假设 `digest` 为某层的真实摘要）：

```text
obd-pool/v1/t/mooncake/sha256:<64位摘要>/262144/0
obd-pool/v1/t/mooncake/sha256:<64位摘要>/262144/1
```

规则：

- i 是从 0 开始的十进制块号，不是字节偏移、ZFile 内部块号或逻辑磁盘扇区。
- B 和 i 不补前导零；第一个块号为字符串 `0`。
- Key 不带前导 `/`、尾部换行、NUL 或 URI 的查询参数。
- namespace 和 tenant 区分大小写，长度 1～64，仅使用 ASCII 字母、数字、点、下划线、连字符；不能为 `.` 或 `..`。
- 公共 Key 不包含 tenant，同一 namespace 内可以共享；私有 Key 包含 tenant。
- namespace 是我们在字符串中使用的前缀，不是要求 SDK 自动添加的隐藏前缀。不得重复添加。
- 公共/私有 Key 约定不是服务端认证、访问控制或配额机制。

URI 不是 Store Key。不得把 `moonpool://...` 整串作为对象 Key。

首轮不使用按 chunk 自身内容摘要定位的 Key，不将 TransferID、镜像 tag、repository 或本地路径加入数据 Key。不同 repository 引用同一原始 blob 时，可以按上述规则复用对象。

旧透明缓存使用的 `<namespace>/blob-v1/...` 不属于本次 scheme 联调协议，不能与 `/v1/b/`、`/v1/t/` 混用。

## 6. 切片与 Value 格式

块数量：

```text
N = (L + B - 1) // B
i = 0 .. N-1
offset = i * B
length = min(B, L - offset)
payload = blob[offset : offset + length]
```

最后一块按实际长度保存，不补齐；L 正好整除 B 时不增加空块。

每个对象的 Value 是以下二进制字节串：

| 范围（左闭右开） | 字节数 | 内容 |
| --- | ---: | --- |
| `[0,8)` | 8 | 固定 ASCII 字节 `OBDPOOL1`，没有结尾 NUL |
| `[8,16)` | 8 | payload 实际长度，uint64 小端 |
| `[16,24)` | 8 | 块在完整原始 blob 中的字节偏移，uint64 小端 |
| `[24,56)` | 32 | 下述 SHA256 的原始二进制结果 |
| `[56,64)` | 8 | 保留字段，写入全零 |
| `[64,64+length)` | length | payload |

计算规则：

```text
checksum = SHA256(Key 的 ASCII 字节 || Value[0:24] || payload)
Value 总长度 = 64 + length
```

`||` 表示无分隔符拼接。校验不包含 checksum 字段和保留字段。checksum 是 32 字节，不是 64 字符的十六进制文本。

Key 参与校验，因此修改 namespace、tenant、scope、B 或 i 后，需要重新计算校验，不能把旧 Value 原样搬到新 Key。

读取端仍必须校验总长度、magic、payload 长度、期望偏移和 checksum。当前实现未验证保留字段，但本协议要求写入端置零，为后续版本保留空间。

### 6.1 大小示例

对于 L=600000、B=262144：

| i | offset | payload 长度 | Value 总长度 |
| ---: | ---: | ---: | ---: |
| 0 | 0 | 262144 | 262208 |
| 1 | 262144 | 262144 | 262208 |
| 2 | 524288 | 75712 | 75776 |

这些长度与 Z 无关，不能用 4096 补齐最后一个 Value。

### 6.2 参考编码

以下 Python 仅定义跨语言对照逻辑，不要求预热工具使用 Python，也不代替生产实现的流式读取和内存控制：

```python
import hashlib
import struct

def encode_blocks(blob, digest, namespace, tenant, block_size, private=True):
    assert 4096 <= block_size <= 4194304
    assert block_size % 4096 == 0
    assert len(blob) > 0
    assert digest == "sha256:" + hashlib.sha256(blob).hexdigest()

    scope = f"t/{tenant}" if private else "b"
    for offset in range(0, len(blob), block_size):
        index = offset // block_size
        payload = blob[offset:offset + block_size]
        key = f"{namespace}/v1/{scope}/{digest}/{block_size}/{index}"
        head = b"OBDPOOL1" + struct.pack("<QQ", len(payload), offset)
        checksum = hashlib.sha256(
            key.encode("ascii") + head + payload
        ).digest()
        value = head + checksum + b"\x00" * 8 + payload
        yield key, value
```

## 7. SDK、批量与资源限制

双方必须接入相同 Mooncake Store 集群及对应 metadata 服务，SDK 与原生库版本需要兼容。当前联调 SDK 基线为 `boostkit/mooncake` 的 `dev-master_ub_transport`，commit `ad3e63a3a599a902ffcc138cc60670771392da8a`。

读取端继续引用正式 C 头文件、动态链接 `libmooncake_store.so`，使用 `mooncake_store_batch_get_into`。预热端可用该版本兼容的 Go/CGo、C 或 C++ Store API，但必须原样保存协议要求的 Key/Value。只在 Transfer Engine 中注册内存、没有创建 Store 对象，不满足要求。

`batchBlocks=16` 表示一次读取最多涉及 16 个独立 Key，不表示把 16 块合并为一个 Value。上传的并发度、批量 Put 数和内部流式缓冲大小可独立配置；这些性能参数不能改变对象的切片格式。

B 的最大值 4 MiB 不表示任意批次大小都能无限制分配。读取端实施时必须：

- 根据实际请求的 URI B 计算单块容量，包含 64 字节头部。
- 正确处理注册缓冲的对齐、批次数量乘法及整数溢出。
- 允许全局 `pool.blockSize` 与已有 URI B 不同；分配足够缓冲，或在受资源上限限制时明确拒绝。
- 不得越界、截断 Value，或把本地缓冲配置不足伪装成缓存缺失后静默回源。
- 保留有界队列、内存预算、超时和异步调用生命周期保护。

当前实现要求全局配置和每层 URI 的 B × batchBlocks 不超过 64 MiB；超出在初始化或打开层时明确拒绝。单次注册区包含对齐开销，最多 64 MiB 加 256 KiB。实际缓冲按本批请求容量分配/扩容。分配或注册失败直接返回资源错误，direct 模式也不回源。预热工具不能依赖读取端一次取完一个层或固定取满 16 块。读取端也不能要求上传端与自身使用相同的 batch 数量。

## 8. 上传完成、重试与数据生命周期

一个镜像可以声明“全预热完成”，至少应满足：

1. 输入 OCI 产物完整、不可变，层摘要和长度校验通过。
2. 选定镜像的每个必需远端层，所有 i=0..N-1 对象均已写入并确认可读。
3. 复用已有 Key 时确认 Value 符合当前协议及预期内容；仅 Exists 或 OBJECT_ALREADY_EXISTS 不足以证明有效。
4. 所有在途 SDK 调用结束，buffer 可以安全释放；不能在 Put 尚未完成时复用或关闭 session。
5. 数据位于持续运行的存储节点，上传进程退出不应导致刚写入的数据失效。

重试使用相同 Key 和 Value；失败不自动删除其他任务可能共享的块。遇到已有但损坏的不可变对象，应报告冲突或损坏，由独立修复策略处理，不盲目覆盖共享数据。

上传工具可以保留 TransferID、传输索引、ManifestKey、CommitState 和 Lookup，作为自己的状态管理。**当前读取端不读取传输索引或完成标记，只按本协议直接推导 Key。** 它也不接收 TransferID 来选择某一套块。

建议上传结果或可序列化的旁路报告至少包含：

```text
storageFormat = moonpool-v1
manifestDigest、platform
namespace、tenant、scope
各层 digest、size、cacheBlockSize、blockCount
TransferID、CommitState（若工具采用完成记录）
```

这是建议的报告语义，不是新增的必读 Store 对象格式。上传状态和连接配置不写回不可变 OCI manifest/artifact。调用方应在全预热完成后再发起消费；当前读取端不会仅因为没有完成标记而拒绝一套已经完整可读的块。

完成记录不能阻止内存缓存被淘汰。首轮不要求实现固定、租约或持久化，但部署必须提供足够存储空间，并明确“成功时可读”与“长期可用”的区别。验收必须包含预热进程退出后的独立读取。

## 9. 读取、回源与 pull 预检

```text
Docker/containerd 获取 Registry manifest/config
  → snapshotter 为新快照生成带独立 B 的 URI
  → OverlayBD 优先读取本地缓存
  → 本地未命中，按 URI B 计算 Key 并批量 Get
  → 校验 Value，提取 payload，返回所需原始字节
  → 原有 tar/ZFile/镜像层解析继续工作
```

`fallback=none` 的现有语义保留：

- 缺块：ENOENT。
- RPC 失败、超时或校验失败：EIO。
- 不回源、不回填。
- 本地缓存已有有效内容时，可正常读取，并不要求 Mooncake 再次提供同一块。

需要在 pull 阶段拒绝缺块时，启用配套 snapshotter 预检和 OverlayBD serviceConfig。预检读取完整原始层，使用同一个本地缓存和 PoolFile；它不得要求 zfileBlockSize 存在或等于 B。

预检目前有全层读取和本地缓存填充成本，不是轻量 Exists 检查。检查通过后若数据再次淘汰，后续读取仍可能失败，不能据此宣称已实现镜像保留租约。

## 10. Registry 发布与模块职责

本协议不让 Docker 直接从 Mooncake 获取 manifest/config。调用方仍需提供客户端可访问的 Registry 镜像引用。

| 组件 | 职责 |
| --- | --- |
| Converter | 输出有效、完整、不可变的原生 OverlayBD OCI 产物；不决定 Mooncake 缓存 B。 |
| Uploader | 校验产物，按显式配置的 B 和本协议写入 Store；不重新转换、不修改 manifest。 |
| Registry 发布方 | 将相同产物发布到客户端可访问的 Registry，并提供 PullRef；可由调用方使用独立工具实现。 |
| snapshotter/OverlayBD | 从元数据定位层，按 URI B 读取并校验字节；不依赖上传器的运行状态、代码库或 TransferID。 |
| 联调调用方 | 对齐配置，确认上传完成、Registry 引用与产物属于同一个最终 manifest digest，然后发起拉取。 |

首轮可保真发布完整产物到 Registry，再用 `fallback=none` 验证层数据零回源。不要求 Uploader 自身依赖 Registry，不要求建立新的网关，也不能因 Mooncake 中存在 OCI 元数据就认为 Docker 已能发现镜像。

建议使用 digest 固定的 PullRef，避免 tag 在上传与拉取之间变化。暂存 Registry 若允许清理，不能未经约定就作为长期消费入口。

## 11. 缓存大小切换与重新部署

本轮不实现旧格式、旧快照或旧缓存迁移兼容。使用新版二进制、新测试目录、新快照和按目标 B 重新预热的数据，不复用旧迁移包的 options.json 或已有 snapshotter 数据目录。

Key 中包含 B，因此同一 blob 使用不同大小时会产生独立的对象集合：

```text
<namespace>/v1/t/<tenant>/<digest>/4096/0
<namespace>/v1/t/<tenant>/<digest>/262144/0
```

二者的 payload 范围、长度和 checksum 不同，不能直接替代。4 KiB 仍是本版本可配置的合法缓存大小，不表示本轮承诺旧版本兼容。

运行中的具体层以 URI 为准，全局配置变更不会自动改写它。切换 B 时，先准备新 B 的预热数据，并明确重新创建快照；单独清理本地文件缓存不会重写 URI。当前不自动拼接、删除或迁移不同大小的数据对象。

## 12. 联调验收

### 12.1 前置条件

- 读取端已完成解耦实现并记录二进制/源码版本；旧基线不计为本协议验收环境，使用新快照及新预热数据。
- 固定同一份有效原生 OverlayBD 镜像和最终 manifest digest。
- 对齐 namespace、tenant、scope、B、Store 集群和 Registry PullRef。
- 主验收使用 Z=4096、B=262144，镜像无 `zfile-config` 注解。
- 使用全新或已清空的本地文件缓存，防止掩盖 Mooncake 缺块。
- 开启 `fallback=none` 和 pull 预检，保留读取端日志及 Registry 层请求证据。

### 12.2 必测用例

| 用例 | 预期 |
| --- | --- |
| 无 zfile-config，Z=4 KiB、B=256 KiB，全量预热后上传进程退出 | pull、启动容器、业务文件校验成功；发生真实 Mooncake 命中；层数据零回源。 |
| 缺一个必需块 | pull 因缺块失败，不以网络异常冒充缺块测试通过。 |
| 破坏头部、长度、偏移或 checksum | 明确失败，不回源。 |
| 尾块、整除长度、B 大于 blob 长度 | 无补齐、无额外空块，返回字节正确。 |
| 不对齐请求、跨块读取、跨 ZFile 编码边界、preadv | 结果与原始 blob 对应字节范围一致。 |
| 本版本生成的 URI 大小与全局默认不同 | 以 URI 为准，按对应 Key 读取；不按全局值重新解释。 |
| 全局默认小于已有 URI B | 正确分配并读取，或因明确资源限制拒绝；不能越界或静默回源。 |
| B=4 KiB、256 KiB、4 MiB；合法的不同批量大小 | 在声明的资源预算内正确读取，头部和对齐开销已计入容量。 |
| 非法 B：0、非 4096 倍数、超过 4 MiB | 双方明确拒绝，不静默修正。 |
| 同摘要同 B，不同 tenant/namespace/scope | 仅按协议规定共享或隔离，不误读。 |
| 同一产物重复预热、多个任务复用块 | 不改变镜像身份，内容一致，状态可核实。 |
| 无效 ZFile 内容 | 原有格式检查仍失败，不能因缓存命中而绕过。 |

验收结果应记录：镜像 digest、真实 Z、B、各层长度和块数、写入端/读取端版本、实际 URI、配置身份、命中/回源情况、容器内约定文件的摘要。

`batchBlocks=16` 是上限，不要求一次预检的总调用数严格等于 `ceil(总块数/16)`；上层请求边界、层数及本地缓存会影响实际批次。应验证每批不超过配置上限且字节结果正确。

### 12.3 跨语言编码测试向量

以下是纯编码向量，不是可启动的 OverlayBD 镜像，用于独立验证 Go/C/C++ 编码是否一致：

```python
blob = bytes(range(256)) * 16 + b"end"
namespace = "obd-pool"
tenant = "mooncake"
private = True
block_size = 4096
```

L=4099，digest：

```text
sha256:0965d51d70c20360dbdff742a9046907659ed88a042935faabebc72f02a0be4a
```

第 0 块 Key：

```text
obd-pool/v1/t/mooncake/sha256:0965d51d70c20360dbdff742a9046907659ed88a042935faabebc72f02a0be4a/4096/0
```

第 0 块 Value 长度 4160，头部的十六进制表示：

```text
4f4244504f4f4c31001000000000000000000000000000001898026638261c2e6dd49ce131f22082cb42dfab751e70d02e7009cfc69fb22b0000000000000000
```

第 0 块完整 Value 的 SHA256（仅作测试对照，不是头部 checksum）：

```text
3d49281431024a03830dc5543e6a91b7809e599775a7ee522bce6388b87ba2d9
```

第 1 块 Key 的最后一段为 `/1`，payload 为 `end`，Value 长度 67，头部：

```text
4f4244504f4f4c31030000000000000000100000000000001c83a6871a737f5770cf5f72e73a4c57646b6b5d91c5e83ba86cf5a4ef6fb5ba0000000000000000
```

第 1 块完整 Value 的 SHA256：

```text
5e9e447e147f837427047de8fd4752949c13fc4dabdfe351b94bccde29da9f45
```

建议把本节向量和第 6.1 节的 256 KiB 尾块算例放入双方各自的测试，不依赖对方运行环境即可核对。缓存编码测试通过后，再进行真实镜像端到端验收。

## 13. 实施分工与交付门槛

| 负责方 | 实现职责 |
| --- | --- |
| 预热工具 | 独立 B 配置，明确 namespace/tenant/scope；不读取 ZFile 注解决定 B；按协议编码与写入；完整性与完成状态检查；上述编码向量测试。 |
| snapshotter | 新增默认 256 KiB 的 poolBlockSize；新 URI 使用它；不强制 zfile-config，不把 B 写进 zfileBlockSize。 |
| OverlayBD | 放宽 URI B 的合法范围；移除 B=Z 约束和预检残留耦合；按实际 URI 保障单块/批量缓冲容量；保留原有格式检查及回源策略。 |
| 迁移脚本 | 对齐两端 B；预热列表按独立 B 生成；适配无注解镜像；允许外部预热、外部镜像的只读验收。 |
| 联调调用方 | 固定协议配置与版本、发布 Registry 引用、安排新版本部署与缓存重新预热、共同执行验收。 |

本次 `tools/portable-test/pool-seed.cpp` 和脚本已按独立 B 切块；新包基于无 zfile-config 的 native 原始镜像。外部团队可参考头部编码，但仍应独立验证自己的工具。本包的 verify 支持外部固定 digest 引用、manifest 文件、容器校验命令和期望输出，无需执行本包 seed。

本文冻结的数据 Key、Value 和缓存块语义应共同维护。后续若修改编码、寻址或引入内容摘要 chunk 索引，应显式定义兼容规则或新版本，不能在保持相同 Key 的同时悄悄改变 Value 含义。

## 14. 本次读取端验证记录

- SDK 开启/关闭两种构建的单元测试通过，snapshotter 单元测试通过。
- 真实 C SDK 验证 4 KiB、256 KiB、4 MiB、尾块、跨块 preadv、缓冲扩容、全局默认与 URI 不同等场景。
- 原始无 zfile-config 镜像的真实 ZFile 块为 4096，池块为 262144。预热 211 块、54919680 字节后，Docker pull、容器启动和文件 SHA256 校验通过；211 次块命中、54 次批量请求、零层数据回源。上层预检以 1 MiB 请求读取，因此最大实际批次为 4，未强制凑满 16。
- 空池 pull 失败且零回源；外部预热工具尚需按本文独立联调，以上结果不替代对方验收。
- 远端证据目录：`/home/cyx/overlaybd-mooncake-test/check-v2/obd-mooncake-arm64/results/`；汇总报告为 `results/cache-block-decoupling.json`。
