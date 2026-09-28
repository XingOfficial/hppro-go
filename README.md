# HP-PRO 客户端（Go + Compose）

hppro 5.0 的开源复刻版：协议层用 Go（quic-go，与官方同款），UI 用 Jetpack Compose。
协议字段逆向自官方 `libgojni.so`，完整报告见文末。

## 协议速览

| 项 | 值 |
|---|---|
| 服务器 | `socket.hpproxy.cn:6666`（TCP + UDP/QUIC 同端口） |
| 帧格式 | `[4B magic=0x00001a0a][4B BE 长度][protobuf]` |
| TCP cmd 通道 | `CmdMessage{CONNECT/TIPS/...}`，15s 心跳 |
| QUIC 数据通道 | ALPN=`HP_PRO`，`HpMessage{REGISTER...}` 注册，TLS 1.3 |
| 认证 | 32 位 deviceId |

## 仓库结构

```
├── hp.go              Go 协议层入口（gomobile bind 生成 hpclient.aar）
├── internal/          协议编解码 + TCP/QUIC 双通道 + 重连心跳
├── app/               Android 工程（Kotlin + Compose，包名 cn.xing.thirdpartyapp.hppro）
└── .github/workflows  CI：gomobile bind → gradle assembleRelease → APK artifact
```

## GitHub Actions 自动构建

push 到 `main` 即触发（或手动 `workflow_dispatch`）：
1. Go 交叉编译出 `hpclient.aar`
2. Gradle 打出 release APK
3. 产物在 Actions → build → Artifacts（`HP-Pro-release`）

## 本地构建（Termux / Linux）

```bash
# Go 客户端 → AAR（需要 NDK，Flutter 构建过的机器一般都有）
export ANDROID_NDK_HOME=你的NDK路径
go install golang.org/x/mobile/cmd/gomobile@latest
go install golang.org/x/mobile/cmd/gobind@latest
$(go env GOPATH)/bin/gomobile init
$(go env GOPATH)/bin/gomobile bind -androidapi 26 -target=android -o app/libs/hpclient.aar .

# APK
cd app && ./gradlew assembleRelease
```

## 使用

安装 APK → 右上角「设置」填 32 位设备ID → 连接。
日志页看协议走向：`[cmd]` 是 TCP 心跳通道，`[quic]` 是 QUIC 注册/数据通道。

## 已实现 / 路线图

- [x] TCP cmd 通道（CONNECT 注册 + TIPS 心跳，实测服务器回包稳定）
- [x] QUIC 通道握手 + HpMessage REGISTER
- [ ] REGISTER_RESULT / CONNECTED 解析校准（真机联调中）
- [ ] channelId 数据面转发（本地 TCP/UDP 代理，对标官方 LocalProxyHandler）

## 协议逆向报告

见 [HP_PRO_逆向报告.md](docs/HP_PRO_逆向报告.md)（帧格式、proto 定义、官方客户端反汇编结论均在内）。
