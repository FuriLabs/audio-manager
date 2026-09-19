# audio-manager

## CLI

```sh
make
G_MESSAGES_DEBUG=all ./audio-manager-ctl
```

```text
output receiver|speaker|headphones|headset|bluetooth|usb
input builtin-mic|headset-mic|bluetooth|usb
devices OUTPUT INPUT
call start hostless|hostful
call route OUTPUT INPUT hostless|hostful
call stop
call bluetooth narrow|wide nrec|no-nrec
call volume 0.0..1.0
call downlink start|stop
call downlink info
call downlink read BYTES
call uplink start|stop
call uplink info
call uplink tone MILLISECONDS
mute uplink on|off
mute downlink on|off
status
help
quit
```
