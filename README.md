# Arduino pluggable discovery for dfu devices

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

## Security

If you think you found a vulnerability or other security-related bug in this project, please read our
[security policy](https://github.com/arduino/mdns-discovery/security/policy) and report the bug to our Security Team 🛡️
Thank you!

e-mail contact: security@arduino.cc