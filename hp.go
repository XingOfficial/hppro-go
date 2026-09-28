package hp

import (
	"errors"
	"sync"

	"hpclient/internal"
)

type Callback interface {
	OnStatus(state int, msg string)
	OnLog(line string)
}

type gomobileLogger struct {
	cb Callback
}

func (g gomobileLogger) OnLog(line string) {
	if g.cb != nil {
		g.cb.OnLog(line)
	}
}

func (g gomobileLogger) OnStatus(state int, msg string) {
	if g.cb != nil {
		g.cb.OnStatus(state, msg)
	}
}

var (
	client *internal.Client
	mu     sync.Mutex
)

func Start(server string, deviceID string, cb Callback) error {
	mu.Lock()
	defer mu.Unlock()
	if client != nil {
		return errors.New("已在运行")
	}
	c := internal.NewClient(server, deviceID, gomobileLogger{cb})
	if err := c.Start(); err != nil {
		return err
	}
	client = c
	return nil
}

func Stop() {
	mu.Lock()
	defer mu.Unlock()
	if client != nil {
		client.Stop()
		client = nil
	}
}
