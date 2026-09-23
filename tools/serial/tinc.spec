# TINCLIB frame (tinclib-protocol) for serial-passthrough's -p option.
# Numbers print in decimal: flags 1 = reply, 5 = error reply; payload[0] of an
# error reply is the error code. Types: 1 HELLO, 2 STATUS, 16 REQ_BEGIN,
# 17 REQ_STATUS, 20 REQ_ABORT, 33 BODY_READ, 64 WIFI_LIST, 65 WIFI_SET,
# 66 WIFI_FORGET.
sof:u8=0xA5
flags:u8
type:u8
seq:u8
len:u16
payload:bytes(len)
crc:crc16ccitt(flags..payload)
