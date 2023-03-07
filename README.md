# Arduino pluggable discovery for dfu devices
[![Sync Labels status](https://github.com/arduino/dfu-discovery/actions/workflows/sync-labels.yml/badge.svg)](https://github.com/arduino/dfu-discovery/actions/workflows/sync-labels.yml)
[![Check Go Dependencies status](https://github.com/arduino/dfu-discovery/actions/workflows/check-go-dependencies-task.yml/badge.svg)](https://github.com/arduino/dfu-discovery/actions/workflows/check-go-dependencies-task.yml)
[![Check License status](https://github.com/arduino/dfu-discovery/actions/workflows/check-license.yml/badge.svg)](https://github.com/arduino/dfu-discovery/actions/workflows/check-license.yml)
[![Check Go status](https://github.com/arduino/dfu-discovery/actions/workflows/check-go-task.yml/badge.svg)](https://github.com/arduino/dfu-discovery/actions/workflows/check-go-task.yml)
[![Publish Tester Build status](https://github.com/arduino/dfu-discover/actions/workflows/publish-go-tester-task.yml/badge.svg)](https://github.com/arduino/dfu-discovery/actions/workflows/publish-go-tester-task.yml)
[![Check Taskfiles status](https://github.com/arduino/dfu-discovery/actions/workflows/check-taskfiles.yml/badge.svg)](https://github.com/arduino/dfu-discovery/actions/workflows/check-taskfiles.yml)

The `dfu-discovery` tool is a command line program that interacts via stdio. It accepts commands as plain ASCII strings terminated with LF `\n` and sends response as JSON.

## How to build:

[Docker](https://www.docker.com/) is required to build this project, along with [task](https://taskfile.dev/).

Just run `task download-dfu-util` to download dfu-util source code and then `task dist:<platform-to-build>`
The platform to build can be one of:
- Windows_32bit
- Windows_64bit
- Linux_32bit
- Linux_64bit
- Linux_ARMv6
- Linux_ARMv7
- Linux_ARM64
- macOS_64bit
- macOS_ARM64

The executable `dfu-discovery` will be produced inside `dist/dfu-discovery-<platform>/dfu-discovery`.

## Usage

After startup, the tool waits for commands. The available commands are: `HELLO`, `START`, `STOP`, `QUIT`, `LIST` and `START_SYNC`.

#### HELLO command

The `HELLO` command is used to establish the pluggable discovery protocol between client and discovery.
The format of the command is:

`HELLO <PROTOCOL_VERSION> "<USER_AGENT>"`

for example:

`HELLO 1 "Arduino IDE"`

or:

`HELLO 1 "arduino-cli"`

in this case the protocol version requested by the client is `1` (at the moment of writing there were no other revisions of the protocol).
The response to the command is:

```json
{
  "eventType": "hello",
  "protocolVersion": 1,
  "message": "OK"
}
```

`protocolVersion` is the protocol version that the discovery is going to use in the remainder of the communication.

#### START command

The `START` starts the internal subroutines of the discovery that looks for ports. This command must be called before `LIST` or `START_SYNC`. The response to the start command is:

```json
{
  "eventType": "start",
  "message": "OK"
}
```

#### STOP command

The `STOP` command stops the discovery internal subroutines and free some resources. This command should be called if the client wants to pause the discovery for a while. The response to the stop command is:

```json
{
  "eventType": "stop",
  "message": "OK"
}
```

#### QUIT command

The `QUIT` command terminates the discovery. The response to quit is:

```json
{
  "eventType": "quit",
  "message": "OK"
}
```

after this output the tool quits.

#### LIST command

The `LIST` command returns a list of the currently available dfu ports. The format of the response is the following:

```json
{
  "eventType": "list",
  "ports": [
    {
      "address": "0-1",
      "label": "0-1",
      "properties": {
        "pid": "0x0399",
        "vid": "0x2341",
        "serialNumber": "321315255931323671D633314B572C41"
      },
      "hardwareId": "321315255931323671D633314B572C41",
      "protocol": "dfu",
      "protocolLabel": "USB DFU"
    }
  ]
}
```

The `ports` field contains a list of the available serial ports. If the serial port comes from an USB serial converter the USB VID/PID and USB SERIAL NUMBER properties are also reported inside `properties`.

The list command is a one-shot command, if you need continuous monitoring of ports you should use `START_SYNC` command.

#### START_SYNC command

The `START_SYNC` command puts the tool in "events" mode: the discovery will send `add` and `remove` events each time a new port is detected or removed respectively.
The immediate response to the command is:

```json
{
  "eventType": "start_sync",
  "message": "OK"
}
```

after that the discovery enters the "events" mode.

The `add` events looks like the following:

```json
{
  "eventType": "add",
  "port": {
    "address": "0-1",
    "label": "0-1",
    "protocol": "dfu",
    "protocolLabel": "USB DFU",
    "properties": {
      "pid": "0399",
      "serial": "321315255931323671D633314B572C41",
      "vid": "2341"
    },
    "hardwareId": "321315255931323671D633314B572C41"
  }
```

it basically gather the same information as the `list` event but for a single port. After calling `START_SYNC` a bunch of `add` events may be generated in sequence to report all the ports available at the moment of the start.

The `remove` event looks like this:

```json
{
  "eventType": "remove",
  "port": {
    "address": "0-1",
    "label": "0-1",
    "protocol": "dfu",
    "protocolLabel": "USB DFU",
    "properties": {
      "pid": "0399",
      "serial": "321315255931323671D633314B572C41",
      "vid": "2341"
    },
    "hardwareId": "321315255931323671D633314B572C41"
  }
}
```

in this case only the `address` and `protocol` fields are reported.

### Example of usage

A possible transcript of the discovery usage:

```bash
$ ./serial-discovery
HELLO 1 "arduino-cli"
{
  "eventType": "hello",
  "protocolVersion": 1,
  "message": "OK"
}
START
{
  "eventType": "start",
  "message": "OK"
}
LIST
{
  "eventType": "list",
  "ports": [
    {
      "address": "0-1",
      "label": "0-1",
      "protocol": "dfu",
      "protocolLabel": "USB DFU",
      "properties": {
        "pid": "0399",
        "serial": "321315255931323671D633314B572C41",
        "vid": "2341"
      },
      "hardwareId": "321315255931323671D633314B572C41"
    }
  ]
}
START_SYNC
{
  "eventType": "start_sync",
  "message": "OK"
}
{
  "eventType": "list",
  "ports": [
    {
      "address": "0-1",
      "label": "0-1",
      "protocol": "dfu",
      "protocolLabel": "USB DFU",
      "properties": {
        "pid": "0369",
        "serial": "321315255931323671D633314B572C41",
        "vid": "2341"
      },
      "hardwareId": "321315255931323671D633314B572C41"
    }
  ]
}
{
  "eventType": "remove",
  "port": {
    "address": "0-1",
    "label": "0-1",
    "protocol": "dfu",
    "protocolLabel": "USB DFU",
    "properties": {
      "pid": "0399",
      "serial": "321315255931323671D633314B572C41",
      "vid": "2341"
    },
    "hardwareId": "321315255931323671D633314B572C41"
  }
}
{
  "eventType": "list",
  "ports": [
    {
      "address": "0-1",
      "label": "0-1",
      "protocol": "dfu",
      "protocolLabel": "USB DFU",
      "properties": {
        "pid": "0369",
        "serial": "321315255931323671D633314B572C41",
        "vid": "2341"
      },
      "hardwareId": "321315255931323671D633314B572C41"
    }
  ]
}
QUIT
{
  "eventType": "quit",
  "message": "OK"
}
$
```

## Security

If you think you found a vulnerability or other security-related bug in this project, please read our
[security policy](https://github.com/arduino/dfu-discovery/security/policy) and report the bug to our Security Team 🛡️
Thank you!

e-mail contact: security@arduino.cc

## License

Copyright (c) 2018 ARDUINO SA (www.arduino.cc)

The software is released under the GNU General Public License, which covers the main body
of the dfu-discovery code. The terms of this license can be found at:
https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

See [LICENSE.txt](https://github.com/arduino/dfu-discovery/blob/master/LICENSE.txt) for details.
