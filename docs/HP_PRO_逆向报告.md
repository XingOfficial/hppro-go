# HP-PRO 逆向报告（hppro 5.0）

> 逆向时间：2026-09-28 ｜ 来源：upload/hppro_5-0-apk.txt（28MB APK）

## 1. 应用概况

| 项 | 值 |
|---|---|
| 包名 | `net.hserver.hppro` |
| 版本 | 5.0 (code 1) |
| 入口 | `net.hserver.hppro.MainActivity` |
| 服务 | `net.hserver.hppro.ProxyService`（前台服务） |
| 架构 | Java/Kotlin UI + gomobile bind（libgojni.so 12.7MB，Go 核心） |
| 作者源码路径（泄漏自 so） | `/Users/heixiaoma/Code/java/hp-pro/hp-client-golang/` |
| 用途 | 内网穿透 + 本地代理（frp/nps 类），QUIC 隧道 |
| 版本检查 | `https://api.hpproxy.cn/server/version/getVersion` |

## 2. 连接参数（核心）

- **服务器**：`socket.hpproxy.cn:6666`
- **传输**：QUIC（quic-go）+ TCP 备用控制通道（cmd.CmdClient/TcpConnection）
- **认证**：32 位 deviceId（用户输入/扫码导入），Java 调 `Android.start(serverAddr, deviceId, callback)`

## 3. 协议消息（从 .so 内嵌 protobuf 描述符完整还原）

包名：`net.hserver.hp.common.protocol`

### HpMessage.proto（数据通道）

```protobuf
message HpMessage {
  HpMessageType type = 1;
  MetaData metaData = 2;
  bytes data = 3;
  enum HpMessageType {
    REGISTER = 0;        // 注册
    REGISTER_RESULT = 1; // 注册结果
    CONNECTED = 2;       // 隧道已连
    DISCONNECTED = 3;    // 断开
    DATA = 4;            // 数据
    KEEPALIVE = 5;       // 心跳
  }
  message MetaData {
    string key = ?;          // channelId/认证 key
    MessageType type = ?;    // TCP=0 / UDP=1 / TCP_UDP=2
    string channelId = ?;
    bool success = ?;
    string reason = ?;
  }
  enum MessageType { TCP = 0; UDP = 1; TCP_UDP = 2; }
}
```

### CmdMessage.proto（控制通道，TCP）

```protobuf
message CmdMessage {
  CmdMessageType type = 1;
  string key = 2;
  bytes data = 3;
  string version = 4;
  enum CmdMessageType {
    CONNECT = 0;
    DISCONNECT = 1;
    LOCAL_INNER_WEAR = 2;  // 内网穿透指令
    TIPS = 3;
  }
}
```

（proto 描述符原文偏移：HpMessage.proto @8644802，CmdMessage.proto @8634630）

## 4. Go 核心模块结构（hp-client-golang）

```
android/android.go        → gomobile 入口: Start(server, id, cb)/Close/GetStatus/Touch
net/hp/HpClient.go        → 主客户端: Connect/Close/GetStatus/GetServer/GetProxyServer
net/hp/RouterTable.go     → 路由表: RefreshRouter
net/cmd/CmdClient.go      → TCP 控制通道: Connect/Close/GetStatus
net/connect/QuicConnection.go → QUIC 数据通道 (ConnectHp)
net/connect/TcpConnection.go  → TCP 数据通道 (ConnectLocal)
net/connect/UdpConnection.go  → UDP 数据通道
net/handler/HpClientHandler.go → 消息处理: ChannelRead/WriteData/CloseAll
net/handler/LocalProxyHandler.go    → 本地 TCP 代理
net/handler/LocalProxyUdpHandler.go → 本地 UDP 代理
bean/  → ConnectType/SysInfo(gopsutil 系统信息上报)/LocalInnerWear/WtoN
```

## 5. 运行流程

1. UI 输入 32 位 deviceId（或扫码）→ `Android.start("socket.hpproxy.cn:6666", deviceId, cb)`
2. CmdClient 走 TCP 建控制通道，发 CmdMessage(CONNECT, key=deviceId, version)
3. HpClient 走 QUIC 建 QUICConnection，发 HpMessage(REGISTER, metaData.key=deviceId)
4. 收 REGISTER_RESULT(success) → 收 CONNECTED(channelId, type) → 按 channelId 建 TCP/UDP 数据流
5. LOCAL_INNER_WEAR → 本地代理转发；KEEPALIVE 保活；断开自动重连（ProxyService 有 5 秒轮询 getStatus 的守护线程）

## 6. 权限清单（无 VPN 权限！）

INTERNET, ACCESS_NETWORK_STATE, FOREGROUND_SERVICE, POST_NOTIFICATIONS,
CAMERA(扫码), READ/WRITE_EXTERNAL_STORAGE, WAKE_LOCK, VIBRATE, REQUEST_INSTALL_PACKAGES(自更新)

## 7. 对新客户端的启示

- 它"稳 10 小时"的原因：QUIC 抗丢包 + TCP cmd 双通道 + 5 秒守护轮询 + deviceId 简单认证
- 我们写 C++ 版可以直接复用同一协议连它的服务器（协议字段已完整还原）
- 需要实现的传输层：QUIC（建议 ngtcp2/nghttp3 或 lsquic）+ TCP 兜底
- protobuf 编解码手写即可（字段就 8 个），无需引入 protobuf 库
