# ESP32 (pseudo) /dev/log
A simple little tool that allows you to capture all "console" output and
pipe it to a remote syslog daemon, to a local file, and/or a memory buffer.

All you need to do is add it to your components directory, include the
header into your main app and set the destination to any combination of the
three destinations.

It starts capturing "fairly" early in the boot process and could be made
to capture even earlier, but that would require patching the bootloader
code.

Example:

```c
#include "devlog.h"

void app_main(void)
{

    ... get wifi going ...

    devlog_set_udp_destination("192.168.2.1", 514);

    printf("This should show up on the local \"console\" and the remote syslog daemon\n");
}
```
