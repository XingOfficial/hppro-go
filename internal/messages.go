package internal

// TCP cmd 通道命令枚举
const (
	CmdTypeConnect        uint64 = 0
	CmdTypeDisconnect     uint64 = 1
	CmdTypeLocalInnerWear uint64 = 2
	CmdTypeTips           uint64 = 3
)

// QUIC 数据通道消息枚举
const (
	HpTypeRegister       uint64 = 0
	HpTypeRegisterResult uint64 = 1
	HpTypeConnected      uint64 = 2
	HpTypeDisconnected   uint64 = 3
	HpTypeData           uint64 = 4
	HpTypeKeepalive      uint64 = 5
)

// MetaData.Type 枚举
const (
	ConnTypeTCP    uint64 = 0
	ConnTypeUDP    uint64 = 1
	ConnTypeTCPUDP uint64 = 2
)

// CmdMessage (TCP cmd 通道): 1:type 2:key 3:data 4:version
type CmdMessage struct {
	Type    uint64
	Key     string
	Data    []byte
	Version string
}

func (m *CmdMessage) Marshal() []byte {
	var b []byte
	b = appendTag(b, 1, 0)
	b = appendVarint(b, m.Type)
	if m.Key != "" {
		b = appendTag(b, 2, 2)
		b = appendVarint(b, uint64(len(m.Key)))
		b = append(b, m.Key...)
	}
	if len(m.Data) > 0 {
		b = appendTag(b, 3, 2)
		b = appendVarint(b, uint64(len(m.Data)))
		b = append(b, m.Data...)
	}
	if m.Version != "" {
		b = appendTag(b, 4, 2)
		b = appendVarint(b, uint64(len(m.Version)))
		b = append(b, m.Version...)
	}
	return b
}

func ParseCmdMessage(b []byte) (*CmdMessage, error) {
	m := &CmdMessage{}
	err := parseFields(b, func(field int, wire int, val []byte) {
		switch field {
		case 1:
			m.Type = readVarint(val)
		case 2:
			m.Key = string(val)
		case 3:
			m.Data = append([]byte(nil), val...)
		case 4:
			m.Version = string(val)
		}
	})
	return m, err
}

// MetaData (HpMessage.field2): 1:key 2:type 3:channelId 4:success 5:reason
type MetaData struct {
	Key       string
	Type      uint64
	ChannelId string
	Success   bool
	Reason    string
}

func (md *MetaData) marshal() []byte {
	var b []byte
	if md.Key != "" {
		b = appendTag(b, 1, 2)
		b = appendVarint(b, uint64(len(md.Key)))
		b = append(b, md.Key...)
	}
	if md.Type != 0 {
		b = appendTag(b, 2, 0)
		b = appendVarint(b, md.Type)
	}
	if md.ChannelId != "" {
		b = appendTag(b, 3, 2)
		b = appendVarint(b, uint64(len(md.ChannelId)))
		b = append(b, md.ChannelId...)
	}
	if md.Success {
		b = appendTag(b, 4, 0)
		b = append(b, 1)
	}
	if md.Reason != "" {
		b = appendTag(b, 5, 2)
		b = appendVarint(b, uint64(len(md.Reason)))
		b = append(b, md.Reason...)
	}
	return b
}

func parseMetaData(b []byte) *MetaData {
	md := &MetaData{}
	parseFields(b, func(field int, wire int, val []byte) {
		switch field {
		case 1:
			md.Key = string(val)
		case 2:
			md.Type = readVarint(val)
		case 3:
			md.ChannelId = string(val)
		case 4:
			md.Success = readVarint(val) != 0
		case 5:
			md.Reason = string(val)
		}
	})
	return md
}

// HpMessage (QUIC 数据通道): 1:type 2:metaData 3:data
type HpMessage struct {
	Type     uint64
	MetaData *MetaData
	Data     []byte
}

func (m *HpMessage) Marshal() []byte {
	var b []byte
	b = appendTag(b, 1, 0)
	b = appendVarint(b, m.Type)
	if m.MetaData != nil {
		if md := m.MetaData.marshal(); len(md) > 0 {
			b = appendTag(b, 2, 2)
			b = appendVarint(b, uint64(len(md)))
			b = append(b, md...)
		}
	}
	if len(m.Data) > 0 {
		b = appendTag(b, 3, 2)
		b = appendVarint(b, uint64(len(m.Data)))
		b = append(b, m.Data...)
	}
	return b
}

func ParseHpMessage(b []byte) (*HpMessage, error) {
	m := &HpMessage{}
	err := parseFields(b, func(field int, wire int, val []byte) {
		switch field {
		case 1:
			m.Type = readVarint(val)
		case 2:
			m.MetaData = parseMetaData(val)
		case 3:
			m.Data = append([]byte(nil), val...)
		}
	})
	return m, err
}
