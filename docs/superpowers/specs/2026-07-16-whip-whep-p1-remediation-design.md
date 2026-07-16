# WHIP/WHEP P1 问题修复设计

状态：设计章节已确认，等待文档审阅。

## 背景

当前 WHIP/WHEP 补丁已经覆盖标准 HTTP 信令、WHEP 406 counter-offer、Trickle ICE 与 ICE restart，但代码审查确认存在四个 P1 问题：

1. 活跃会话 ICE restart 后替换了 SDP 对象，`MediaTrack` 内部裸指针悬空。
2. WHEP 406 路径把本地 counter-answer 当成远端 SDP，导致 candidate 与媒体方向判断错误。
3. 标准 `http(s)://` WHIP/WHEP URL 无法从公共播放器和推流器工厂到达 WebRTC 实现。
4. 自定义认证头会在跨源 HTTP 重定向中被原样转发。

本设计把四项修复作为一个变更处理，因为它们共同决定标准 WHIP/WHEP 客户端从公共入口到协商、运行和重启阶段的正确性与安全边界。

## 目标

- 活跃媒体轨道引用的 SDP media 与 codec plan 地址在 transport 生命周期内保持稳定。
- 普通 201 answer 和 WHEP 406 counter-offer 都使用正确的本地/远端 SDP。
- `PlayerBase` 能通过 HEAD 自动发现标准 HTTP(S) WHEP endpoint；`PusherBase` 能直接分派 HTTP(S) WHIP endpoint。
- 跨源重定向或 session `Location` 不会把凭据发送到未经信任的 origin。
- 保持已有 HLS、TS、FLV 和 `webrtc(s)` 入口行为兼容。
- 用单元测试、回归测试和 lab 活跃媒体测试证明四项 P1 均已关闭。

## 非目标

- 不实现通用 HTTP 媒体协议嗅探；HEAD 自动发现只识别 WHEP 的 `application/sdp`。
- 不改变所有 `HttpRequester` 的默认重定向策略；凭据策略限定在 WHIP/WHEP 请求链路。
- 不支持 SDP m-line 重新协商；ICE restart 仍只更新 ICE 代次相关状态。
- 不重建活跃 `MediaTrack`，也不重置 NACK、RTCP、TWCC 或 RTP 排序状态。
- 不增加 WHIP HEAD 探测；HTTP(S) 推流目前没有其他公共协议冲突，直接按 WHIP 分派。

## 方案选择

公共 HTTP(S) WHEP 入口评估过三种方案：

1. 未识别的 HTTP URL 直接兜底为 WHEP。改动小，但会把任意未知 HTTP 资源误判为 WHEP。
2. 要求调用者显式设置 `schema=whep`。无歧义，但标准 URL 不能自动工作。
3. 对未知 HTTP URL 发送 HEAD，并根据 `Content-Type: application/sdp` 自动发现 WHEP。

采用方案 3。它符合当前 WHEP 草案的 endpoint discoverability 机制，并保留 `schema=whep` 作为不支持 HEAD 的旧 endpoint 的显式兼容通道。

## 总体架构

变更划分为四个边界清晰的组件：

1. `HttpPlayerAutoDetect`：只负责未知 HTTP(S) 播放 URL 的 HEAD 探测与最终播放器装配。
2. SDP 所有权视图：在不改动 offer/answer 存储语义的前提下，统一提供本地和远端 SDP 访问。
3. ICE restart 原地提交器：在临时状态中准备新代次，成功后原地更新现有 SDP 的 ICE 字段。
4. WHIP/WHEP 凭据感知请求器：统一处理 HEAD、POST、PATCH、DELETE 的 origin 信任与重定向规则。

这些组件通过窄接口协作，不把 HTTP 自动发现、SDP 角色判断或凭据策略混入 RTP/RTCP 数据面。

## 公共 HTTP(S) 工厂与 WHEP 自动发现

### 播放器分派顺序

`PlayerBase::createPlayer()` 按以下顺序处理 HTTP(S) URL：

1. `.m3u8` 或 `schema=hls`：直接创建 HLS player。
2. `.ts` 或 `schema=ts`：直接创建 TS player。
3. `.flv` 或 `schema=flv`：直接创建 FLV player。
4. `schema=whep`：直接创建 `WebRtcProxyPlayerImp`，跳过 HEAD。
5. schema 为空且类型未知：创建 `HttpPlayerAutoDetect`。
6. 显式但不支持的 schema：保持“不支持”错误，不进入自动探测。

关闭 `ENABLE_WEBRTC` 时不创建自动发现器，未知 HTTP URL 继续返回现有“不支持”错误。

### `HttpPlayerAutoDetect`

新增独立的 `PlayerBase` 实现，内部拥有 HEAD requester 和最终 `PlayerBase` delegate。它必须：

- 使用工厂传入的同一 `EventPoller` 发起 HEAD，避免跨 poller 装配播放器。
- 发送 `HEAD`、`Accept: application/sdp`，并携带与后续 WHEP 请求相同的客户端配置和自定义认证头。
- 只接受 2xx 且规范化媒体类型等于 `application/sdp` 的响应。
- 识别成功后以显式 `schema=whep` 再次调用工厂，避免递归探测。
- 在启动最终 delegate 前复制 mINI 配置、媒体源、播放结果/关闭/恢复回调和 Socket 创建器。
- 使用递增请求代次标识每次 `play()`；旧 HEAD 的迟到回调不得创建 delegate 或触发用户回调。
- `teardown()` 同时取消探测和已创建的 delegate。

为保证包装播放器能够透传自定义 Socket 创建器，在 `PlayerBase` 增加统一的虚拟 Socket 创建器设置入口。普通网络播放器将其应用到自身 `SocketHelper`；`HttpPlayerAutoDetect` 同时保存该回调并应用到 HEAD requester 和最终 delegate。`MediaPlayer` 不再依赖只对当前 concrete delegate 做一次动态转换。

### 推流器分派

`PusherBase::createPusher()` 对 `http` 和 `https` 直接创建 `WebRtcProxyPusherImp`。HTTP(S) WHIP 与 `webrtc(s)` 使用相同的输入媒体约束，源必须能转换为 `RtspMediaSource`；不满足时在建连前返回类型错误。

该改动不引入新的推流 schema 配置，现有 API 中用于选择源媒体的 `schema` 参数保持原语义。

## SDP 本地/远端所有权

### 状态模型

`_offer_sdp` 和 `_answer_sdp` 继续表示 SDP 协议角色，新增显式本地角色状态：

```text
LocalSdpRole = Unset | Offer | Answer
```

- `createOfferSdp()` 成功后设置为 `Offer`。
- `getAnswerSdp()` 成功后设置为 `Answer`。
- 未完成 SDP 设置时访问本地/远端视图属于状态错误，应快速失败而不是默认猜测。

统一提供：

- `localSdp()`
- `remoteSdp()`
- 按 MID 或媒体类型查找的 `localMedia()` 与 `remoteMedia()`

### 两条协商路径

| 协商路径 | 本地 SDP | 远端 SDP | 连通性 candidate | 本地媒体方向 |
|---|---|---|---|---|
| 客户端 offer，服务端 201 answer | offer | answer | answer | offer |
| 服务端 406 counter-offer，客户端 answer | answer | offer | offer | answer |

`connectivityCheckForSFU()` 始终遍历 `remoteSdp()`。`canSendRtp()` 与 `canRecvRtp()` 始终解释本地 media 的方向：

- 本地可发送：`sendonly` 或 `sendrecv`。
- 本地可接收：`recvonly` 或 `sendrecv`。

这些判断不再根据 `Role::CLIENT` 或 `Role::PEER` 反转远端方向。

### 轨道装配

`onStartWebRTC()` 继续使用 answer SDP 作为最终协商 codec、payload type 和 RTP extension plan 的来源，因为无论本地是否为 answer，answer 都表示协商结果。发送/接收通道是否创建，则使用同 MID 的本地 media 判断。

实现时必须审计 `_offer_sdp` 与 `_answer_sdp` 的全部直接引用，并明确分类为：

- 协议角色访问：确实需要 offer 或 answer。
- endpoint 角色访问：必须改为 local 或 remote helper。

不允许在客户端完成路径中继续以 `_answer_sdp` 隐式代表远端。

## ICE restart 原地提交

### 不变量

活跃会话执行 `onStartWebRTC()` 后，以下地址在 transport 销毁前必须稳定：

- `MediaTrack::media`
- `MediaTrack::plan_rtp`
- `MediaTrack::plan_rtx`

因此 ICE restart 不得替换 `_offer_sdp`、`_answer_sdp`、`RtcSession::media` vector 或 codec plan vector。

### 事务顺序

`restartWhipWhepIce()` 按准备、切换、提交三阶段执行：

1. 校验线程、信令协议、ICE 实现、远端凭据、MID 和 candidate。
2. 克隆当前 offer/answer，仅作为 staging 数据。
3. 根据 `LocalSdpRole` 在 staging remote SDP 中应用远端 ICE 凭据，在 staging local SDP 中生成新的本地凭据；不得继续假定 offer 永远是远端、answer 永远是本地。
4. 验证当前与 staging 的 media 数量、MID 和类型完全一致。
5. 从 staging 数据生成待返回的本地 SDP fragment。
6. 把 transport manager 的注册从旧 ufrag 切换到新 ufrag。
7. 调用 ICE agent restart；失败则恢复 manager 的旧 ufrag。
8. agent 成功后，用不抛异常的字符串交换原地提交 staging offer/answer 各 media 的 `ice_ufrag` 与 `ice_pwd`。
9. 更新 completed MID，并输出已经准备好的本地 fragment。

所有可能失败的解析、分配和结构校验必须发生在 manager/agent 切换之前。提交阶段不得整体赋值 `RtcMedia`，避免替换其 plan 容器。

## HTTP origin 与凭据策略

### Origin 规范化

共享的 origin helper 将 URL 规范化为：

```text
lowercase(scheme) + lowercase(host) + effective-port
```

其中 HTTP 默认端口为 80，HTTPS 默认端口为 443；IPv6 地址按等价 authority 比较。可信 origin 配置只接受绝对 HTTP(S) origin，不接受 path、query、fragment 或 userinfo。

### 凭据判定

满足任一条件即视为携带凭据：

- 配置了任意自定义 HTTP 头。
- 请求 URL 含 userinfo。

所有自定义头都按潜在凭据处理，不使用基于头名称的猜测，以覆盖 `Authorization`、`X-Api-Key` 以及业务自定义 secret header。

### 重定向规则

WHIP/WHEP 专用 requester 覆盖重定向决策：

- 同源重定向允许，并保留自定义头。
- 不携带凭据时允许跨源重定向。
- 携带凭据时，跨源目标必须出现在可信 origin allowlist 中，否则拒绝跟随。
- HTTPS 到 HTTP 的降级始终拒绝，allowlist 不能覆盖该规则。
- 拒绝时记录结构化原因，但只包含脱敏 origin，不包含完整 query、header value 或 SDP。

新增每客户端配置：

```text
whip_whep_trusted_origins=https://edge-a.example,https://edge-b.example:8443
```

初始 endpoint origin 自动受信任；配置只扩展跨源目标，不替代初始 origin。

### Session `Location`

HTTP 201 和 WHEP 406 返回的 session `Location` 在写入 `_delete_url` 前执行同一套检查：

- 解析相对或绝对 URL。
- 只允许 HTTP(S)。
- 拒绝 HTTPS 降级。
- 如果后续请求会携带凭据，跨源目标必须受信任。

该检查同时保护 counter-answer PATCH、ICE PATCH 和 DELETE，不能只修初始 POST 的 307。

## HEAD 探测和错误语义

- HEAD 使用与播放器握手一致的超时、代理、网卡与 Socket 创建配置。
- HEAD 网络错误、超时、非 2xx、401/403、缺少或错误的 Content-Type 都只触发一次播放失败回调。
- HEAD 返回 405 或 endpoint 不符合发现约定时，不自动发送 SDP POST；错误信息提示调用者可显式设置 `schema=whep`。
- 安全策略拒绝重定向时，错误与普通网络失败区分，明确指出目标 origin 未受信任或发生 TLS 降级。
- 新的 `play()` 或 `teardown()` 使旧请求代次失效；迟到回调静默丢弃。
- 日志不得输出认证值、URL query token、SDP 正文或 ICE 密码。

## 兼容性

- HLS、TS、FLV 的 URL 后缀和显式 schema 分派顺序不变，不增加 HEAD 延迟。
- `webrtc://` 与 `webrtcs://` 入口保持现有行为。
- 以前立即拒绝的未知 HTTP 播放 URL，现在会异步执行一次 HEAD 后成功识别 WHEP 或返回错误。
- `schema=whep` 提供确定性、无探测路径。
- HTTP(S) 推流以前不受支持，现在按 WHIP 处理。
- 新的可信 origin 配置默认空；默认行为安全收紧为不向未经信任的跨源目标发送自定义凭据。

## 测试设计

### SDP 角色回归

- 普通 201 路径：验证本地 offer、远端 answer、answer candidate 和本地媒体方向。
- WHEP 406 路径：验证本地 answer、远端 offer、offer candidate 和本地 `recvonly`。
- 两条路径都验证 `canRecvRtp()`、远端 DTLS fingerprint 和实际送入 ICE agent 的 candidate。
- 对所有 endpoint 角色访问增加回归断言，防止重新引入 `_answer_sdp == remote` 假设。

### ICE restart

- 重启前后记录 `RtcSession`、`RtcMedia`、`plan_rtp` 和 `plan_rtx` 地址并断言不变。
- 验证 SDP、ICE agent 和 transport manager 同时进入新 ufrag 代次。
- 构造无效 fragment、candidate 或 ufrag 冲突，断言失败后所有组件仍处于旧代次。
- 连续多次 restart，验证 completed MID 与远端 candidate 不跨代残留。

### HTTP 工厂与自动发现

- 已知 HLS、TS、FLV 和显式 schema 不发送 HEAD。
- 未知 HTTP(S) URL 返回自动发现器并发送 HEAD。
- `application/sdp` 的大小写和参数形式能够规范化识别。
- 错误类型、缺少类型、405、401、超时及迟到响应只回调一次。
- `schema=whep` 绕过 HEAD。
- HTTP(S) pusher 创建 WHIP delegate，并拒绝不兼容媒体源。
- `ENABLE_WEBRTC=OFF` 构建不引用自动发现或 WebRTC 类型。

### 凭据安全

- 同源 307 保留自定义凭据。
- 无凭据跨源 307 可以跟随。
- 有凭据且未受信任的跨源目标不收到请求。
- allowlist 目标可以收到凭据。
- HTTPS 降级被拒绝。
- 跨源 session `Location` 对 PATCH/DELETE 使用相同策略。
- 错误与日志不包含测试 token、API key 或 SDP 内容。

## Lab 验收

在 lab 集群使用真实媒体链路验证：

1. `MediaPlayer` 传入标准 HTTP(S) WHEP URL，不设置 schema，HEAD 自动发现并播放成功。
2. `MediaPusher` 传入标准 HTTP(S) WHIP URL，推流成功。
3. 分别验证 201 answer 和 406 counter-offer 协商。
4. RTP 持续传输期间循环执行 ICE restart，观察媒体连续性、SSRC/RTCP 状态和进程稳定性。
5. 使用两个 origin 的 307 服务验证未受信任目标无法收到认证头，allowlist 后可以正常负载均衡。
6. 在 ASan/UBSan 构建下重复活跃会话 ICE restart，确认无 use-after-free、越界或未定义行为报告。

## 预计文件边界

- `src/Common/config.h/.cpp`：新增可信 origin 客户端配置键。
- `src/Player/PlayerBase.h/.cpp`：公共 HTTP 分派与 Socket 创建器转发入口。
- `src/Player/HttpPlayerAutoDetect.h/.cpp`：HEAD 自动发现包装播放器。
- `src/Player/MediaPlayer.cpp`：使用统一 Socket 创建器入口。
- `src/Pusher/PusherBase.cpp`：HTTP(S) WHIP 工厂分派与媒体源校验。
- `webrtc/WebRtcTransport.h/.cpp`：本地/远端 SDP 视图、方向判断和 ICE 原地提交。
- `webrtc/WebRtcClient.h/.cpp`：406 完成路径、凭据感知 requester 与 session URL 策略。
- `webrtc/WhipWhepProtocol.h/.cpp`：媒体类型、URL/origin 规范化和信任判断的可测试纯函数。
- `tests/test_webrtc_regression.cpp`：SDP 角色和 transport 回归。
- `tests/test_whip_whep_protocol.cpp`：origin、Location、重定向及 ICE fragment 单元测试。
- `tests/test_http_player_auto_detect.cpp`：公共工厂、HEAD 识别、取消和回调语义测试，并在 `tests/CMakeLists.txt` 注册。

## 验收标准

变更只有同时满足以下条件才可视为完成：

- 四条审查意见均有直接覆盖其失败模式的自动化测试。
- 普通 201 与 WHEP 406 均能使用远端 candidate 建链，播放器可接收 RTP。
- 活跃 ICE restart 前后的轨道 SDP 指针地址保持不变。
- 未受信任的跨源服务在任何测试路径中都收不到自定义凭据。
- 现有 HLS、TS、FLV 和 `webrtc(s)` 回归测试通过。
- `ENABLE_WEBRTC=OFF` 构建通过。
- lab 的 201、406、WHIP/WHEP 公共入口、跨源重定向和活跃 ICE restart 全部通过。
- ASan/UBSan 未报告与本变更相关的问题。

## 参考规范

- RFC 9725, WebRTC-HTTP Ingestion Protocol (WHIP): <https://www.rfc-editor.org/rfc/rfc9725.html>
- WebRTC-HTTP Egress Protocol (WHEP), draft-ietf-wish-whep-04: <https://datatracker.ietf.org/doc/html/draft-ietf-wish-whep-04>
- RFC 9110, HTTP Semantics: <https://www.rfc-editor.org/rfc/rfc9110.html>
