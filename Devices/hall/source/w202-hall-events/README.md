# W202DS Hall GPIO events

Reads stock GPIO offset 148 on controller `64170000.gpio` with the GPIO-v2
character API. `--info` only reads line ownership/flags. A bounded capture requests
input plus both edges, then waits in `ppoll` until an IRQ event or its deadline.
There is no periodic GPIO read and no output, bias or regulator operation.
It refuses a different controller or an already owned line, and closes the
request on exit/signals. JSON output reports raw level and monotonic event time;
physical polarity is unverified, so no lid-close or suspend action is emitted.

Build the static arm64 helper on CM2, keeping output separate from source:

```sh
mkdir -p /absolute/build-output
make OUT=/absolute/build-output
/absolute/build-output/w202-hall-events --info
/absolute/build-output/w202-hall-events 30
```

The executable runs on the tablet. An idle capture without physical magnetic
stimulus does not verify Hall sensing, edge delivery or polarity.
