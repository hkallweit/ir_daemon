# ir_daemon
Daemon to trigger actions with an IR remote control

Prerequisite is a proper setup of a RC kernel driver.
The daemon reads keyboard input events from /dev/input/ir.
Use an udev rule to link eventx -> ir.

When a key on the RC is pressed /tmp/ir.sh is called with the name of the key as parameter.

Known restrictions:
- Path to PID file is hardcoded
- Path to triggered script is hardcoded
- List of supported keycodes is hardcoded
