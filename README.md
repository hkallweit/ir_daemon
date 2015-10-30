# ir_daemon
Daemon to trigger actions with an IR remote control

Prerequisite is a proper setup of a RC kernel driver.
The daemon reads keyboard input events from /dev/input/ir.
Use an udev rule to link eventx -> ir.

Alternatively you can provide another device with option -d.
ir_daemon -d event3 would read from /dev/input/event3.

The full path to the command to run when a key is pressed has to be provided
as command line argument. The command is run with the name of the key
as parameter.

Known restrictions:
- Path to PID file is hardcoded
- List of supported keycodes is hardcoded
