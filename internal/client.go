package internal

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/quic-go/quic-go"
)

type Logger interface {
	OnLog(line string)
	OnStatus(state int, msg string)
}

const (
	StateIdle       = 0
	StateConnecting = 1
	StateRegister   = 2
	StateOnline     = 3
	StateRetrying   = 4
	StateFailed     = 5
	StateClosed     = 6
)

func statusText(s int) string {
	switch s {
	case StateConnecting:
		return "连接中"
	case StateRegister:
		return "注册中"
	case StateOnline:
		return "在线"
	case StateRetrying:
		return "重连中"
	case StateFailed:
		return "失败"
	case StateClosed:
		return "已关闭"
	}
	return "空闲"
}

var (
	statusMu sync.Mutex
	lastSt   = StateIdle
)

func setStatus(l Logger, state int, msg string) {
	statusMu.Lock()
	lastSt = state
	statusMu.Unlock()
	if l != nil {
		l.OnStatus(state, msg)
	}
}

func lastStatus() int {
	statusMu.Lock()
	defer statusMu.Unlock()
	return lastSt
}

func logf(l Logger, format string, args ...interface{}) {
	if l != nil {
		l.OnLog(fmt.Sprintf(format, args...))
	}
}

type Client struct {
	Server   string
	DeviceID string
	Logger   Logger

	mu       sync.Mutex
	stopOnce sync.Once
	cancel   context.CancelFunc
	wg       sync.WaitGroup
}

func NewClient(server, deviceID string, logger Logger) *Client {
	return &Client{Server: server, DeviceID: deviceID, Logger: logger}
}

func (c *Client) Start() error {
	if c.Server == "" || len(c.DeviceID) != 32 {
		return errors.New("参数不完整（服务器地址 + 32 位设备ID）")
	}
	ctx, cancel := context.WithCancel(context.Background())
	c.cancel = cancel

	c.wg.Add(2)
	go c.cmdLoop(ctx)
	go c.quicLoop(ctx)
	return nil
}

func (c *Client) Stop() {
	c.stopOnce.Do(func() {
		if c.cancel != nil {
			c.cancel()
		}
	})
	c.wg.Wait()
	setStatus(c.Logger, StateClosed, "已停止")
}

func (c *Client) sleep(ctx context.Context, d time.Duration) bool {
	select {
	case <-ctx.Done():
		return false
	case <-time.After(d):
		return true
	}
}

// ---------------- TCP cmd 通道 ----------------

func (c *Client) cmdLoop(ctx context.Context) {
	defer c.wg.Done()
	backoff := 2
	for {
		ok := c.cmdOnce(ctx)
		if ctx.Err() != nil {
			return
		}
		if ok {
			backoff = 2
		} else {
			setStatus(c.Logger, StateRetrying, "cmd 通道断开，"+fmt.Sprint(backoff)+"s 后重连")
			if !c.sleep(ctx, time.Duration(backoff)*time.Second) {
				return
			}
			backoff = backoff * 2
			if backoff > 60 {
				backoff = 60
			}
		}
	}
}

func (c *Client) cmdOnce(ctx context.Context) bool {
	var d net.Dialer
	dialCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	conn, err := d.DialContext(dialCtx, "tcp", c.Server)
	if err != nil {
		logf(c.Logger, "[cmd] TCP 连接失败: %v", err)
		return false
	}
	defer conn.Close()
	logf(c.Logger, "[cmd] TCP 已连接")

	reg := (&CmdMessage{Type: CmdTypeConnect, Key: c.DeviceID, Version: "5.0"}).Marshal()
	if _, err := conn.Write(writeFrame(nil, reg)); err != nil {
		logf(c.Logger, "[cmd] 发送失败: %v", err)
		return false
	}
	logf(c.Logger, "[cmd] -> CmdMessage{CONNECT, key=%s, version=5.0}", c.DeviceID)

	type result struct {
		msg *CmdMessage
		err error
	}
	msgc := make(chan result, 8)
	go func() {
		for {
			payload, err := readFrame(conn)
			if err != nil {
				msgc <- result{nil, err}
				return
			}
			m, err := ParseCmdMessage(payload)
			if err != nil {
				logf(c.Logger, "[cmd] 解码失败 帧长 %d: %v", len(payload), err)
				continue
			}
			msgc <- result{m, nil}
		}
	}()

	keepalive := time.NewTicker(15 * time.Second)
	defer keepalive.Stop()
	for {
		select {
		case <-ctx.Done():
			return false
		case r := <-msgc:
			if r.err != nil {
				if !strings.Contains(r.err.Error(), "bad magic") {
					logf(c.Logger, "[cmd] 断开: %v", r.err)
					return false
				}
				continue
			}
			dataStr := string(r.msg.Data)
			logf(c.Logger, "[cmd] <- CmdMessage{type=%d data=%s}", r.msg.Type, dataStr)
			if r.msg.Type == CmdTypeTips && lastStatus() < StateOnline {
				setStatus(c.Logger, StateOnline, "cmd 通道心跳正常")
			}
		case <-keepalive.C:
			keep := (&CmdMessage{Type: CmdTypeTips, Key: c.DeviceID}).Marshal()
			if _, err := conn.Write(writeFrame(nil, keep)); err != nil {
				logf(c.Logger, "[cmd] keepalive 发送失败: %v", err)
				return false
			}
			logf(c.Logger, "[cmd] keepalive")
		}
	}
}

// ---------------- QUIC 数据通道 ----------------

func (c *Client) quicLoop(ctx context.Context) {
	defer c.wg.Done()
	backoff := 2
	for {
		ok := c.quicOnce(ctx)
		if ctx.Err() != nil {
			return
		}
		if ok {
			backoff = 2
		} else {
			setStatus(c.Logger, StateRetrying, "QUIC 断开，"+fmt.Sprint(backoff)+"s 后重连")
			if !c.sleep(ctx, time.Duration(backoff)*time.Second) {
				return
			}
			backoff = backoff * 2
			if backoff > 60 {
				backoff = 60
			}
		}
	}
}

func (c *Client) quicOnce(ctx context.Context) bool {
	tlsConf := &tls.Config{
		ServerName:         strings.Split(c.Server, ":")[0],
		NextProtos:         []string{"HP_PRO"},
		InsecureSkipVerify: true,
	}
	qcfg := &quic.Config{
		MaxIdleTimeout:       120 * time.Second,
		HandshakeIdleTimeout: 10 * time.Second,
		KeepAlivePeriod:      15 * time.Second,
	}

	dialCtx, cancel := context.WithTimeout(ctx, 12*time.Second)
	defer cancel()
	conn, err := quic.DialAddrEarly(dialCtx, c.Server, tlsConf, qcfg)
	if err != nil {
		logf(c.Logger, "[quic] 拨号失败: %v", err)
		return false
	}
	defer conn.CloseWithError(0, "")
	logf(c.Logger, "[quic] QUIC 握手完成")

	stream, err := conn.OpenStreamSync(ctx)
	if err != nil {
		logf(c.Logger, "[quic] 打开流失败: %v", err)
		return false
	}

	reg := (&HpMessage{
		Type:     HpTypeRegister,
		MetaData: &MetaData{Key: c.DeviceID},
	}).Marshal()
	if _, err := stream.Write(writeFrame(nil, reg)); err != nil {
		logf(c.Logger, "[quic] 注册发送失败: %v", err)
		return false
	}
	logf(c.Logger, "[quic] -> HpMessage{REGISTER, key=%s}", c.DeviceID)
	setStatus(c.Logger, StateRegister, "已发送注册")

	var framer StreamFramer
	buf := make([]byte, 32*1024)
	keepalive := time.NewTicker(15 * time.Second)
	defer keepalive.Stop()
	keepErr := make(chan error, 1)
	go func() {
		for range keepalive.C {
			k := (&HpMessage{Type: HpTypeKeepalive}).Marshal()
			if _, err := stream.Write(writeFrame(nil, k)); err != nil {
				keepErr <- err
				return
			}
			logf(c.Logger, "[quic] keepalive")
		}
	}()

	readErr := make(chan error, 1)
	go func() {
		for {
			n, err := stream.Read(buf)
			if n > 0 {
				for _, payload := range framer.Feed(buf[:n]) {
					m, perr := ParseHpMessage(payload)
					if perr != nil {
						logf(c.Logger, "[quic] 解码失败 帧长 %d: %v", len(payload), perr)
						continue
					}
					md := ""
					if m.MetaData != nil {
						md = fmt.Sprintf(" key=%s ch=%s success=%v reason=%s",
							m.MetaData.Key, m.MetaData.ChannelId, m.MetaData.Success, m.MetaData.Reason)
					}
					logf(c.Logger, "[quic] <- HpMessage{type=%d%s data=%s}", m.Type, md, string(m.Data))
					switch m.Type {
					case HpTypeRegisterResult:
						if m.MetaData != nil && m.MetaData.Success {
							setStatus(c.Logger, StateRegister, "注册成功")
						} else {
							reason := ""
							if m.MetaData != nil {
								reason = m.MetaData.Reason
							}
							setStatus(c.Logger, StateFailed, "注册被拒: "+reason)
						}
					case HpTypeConnected:
						ch := ""
						if m.MetaData != nil {
							ch = m.MetaData.ChannelId
						}
						setStatus(c.Logger, StateOnline, "隧道在线 ch="+ch)
					case HpTypeKeepalive:
						if lastStatus() != StateOnline {
							setStatus(c.Logger, StateOnline, "QUIC 心跳正常")
						}
					}
				}
			}
			if err != nil {
				readErr <- err
				return
			}
		}
	}()

	for {
		select {
		case <-ctx.Done():
			return false
		case err := <-readErr:
			if err != nil && err != io.EOF {
				logf(c.Logger, "[quic] 断开: %v", err)
			}
			return false
		case err := <-keepErr:
			logf(c.Logger, "[quic] keepalive 失败: %v", err)
			return false
		}
	}
}
