#!/bin/bash
set -e
export PATH=~/go/bin:$PATH
export GO111MODULE=on
export GOPROXY=https://goproxy.cn,direct

echo "=== [1/3] 拉依赖 ==="
go mod download

echo "=== [2/3] 安装 gomobile ==="
command -v gomobile >/dev/null || go install golang.org/x/mobile/cmd/gomobile@latest
command -v gobind >/dev/null || go install golang.org/x/mobile/cmd/gobind@latest
gomobile init 2>/dev/null || true

echo "=== [3/3] bind AAR ==="
gomobile bind -androidapi 26 -target=android -o hpclient.aar .
echo "产物: $(pwd)/hpclient.aar"
