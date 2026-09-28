# HP-PRO第三方客户端

hppro 5.0的开源复刻版：协议层用Go（quic-go，与官方同款），UI用Jetpack Compose。
协议字段逆向自官方`libgojni.so`。

## 使用

安装 APK → 右上角「设置」填 32 位设备ID → 连接。
日志页看协议走向：`[cmd]` 是 TCP 心跳通道，`[quic]` 是 QUIC 注册/数据通道。

## 已实现 / 路线图

- [x] TCP cmd 通道（CONNECT 注册 + TIPS 心跳，实测服务器回包稳定）
- [x] QUIC 通道握手 + HpMessage REGISTER
- [ ] REGISTER_RESULT / CONNECTED 解析校准（真机联调中）
- [ ] channelId 数据面转发（本地 TCP/UDP 代理，对标官方 LocalProxyHandler）