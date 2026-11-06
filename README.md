# org.webosports.service.fmradio

The FM radio of a MediaTek phone (`/dev/fm`, the `fmradio_drv` driver) as a
LuneOS service: on and off, tuning, seeking and scanning the 87.5 - 108 MHz
band, the signal strength, and the station's name and RadioText from RDS -
also the names of the stations not listened to, looked for with the radio
off. Sound goes through PulseAudio: while the radio is
on, `fmradiod` keeps `module-droid-fm` (pulseaudio-modules-droid) loaded,
which patches the chip's output to the audio HAL's speaker or headphones.
The wired headphones are the antenna.

A device without `/dev/fm` still runs the service; `getStatus` then says
`available: false`, and a client shows no FM.

## API

All on `luna://org.webosports.service.fmradio/`, in the `fmradio.operation`
group. Frequencies are in kHz (95.0 MHz is `95000`).

| Method | Arguments | Reply |
| --- | --- | --- |
| `getStatus` | `subscribe` | the status below; with `subscribe`, again on every change |
| `powerOn` | `frequency` (optional: where it was) | `frequency` |
| `powerOff` | | |
| `tune` | `frequency` | `frequency` |
| `seek` | `direction`: `"up"` or `"down"` | `frequency` of the station found |
| `scan` | | `stations`: the frequencies found |
| `findNames` | `stations` (optional: the last scan's) | `names`, once all were tried |

The status:

    { "returnValue": true, "available": true, "powered": true,
      "frequency": 95000, "busy": "idle" | "seeking" | "scanning" | "naming",
      "audio": true, "band": { "min": 87500, "max": 108000, "step": 100 },
      "stations": [ ... ], "rssi": -67,
      "rds": { "name": "100% NL", "text": "Onair: ..." },
      "names": { "95000": "100% NL", "99500": "NOORDZEE" },
      "audioError": "..." }

`rssi` (dBm) and `rds` are there only while the radio is on; `rds` holds
`name` and `text` only once the driver has confirmed them (the name after
three receptions agree, the text once a whole RadioText came in), in UTF-8
from the RDS character set. Both are cleared when the frequency changes.
`names` has every station's name the service heard since it started,
whether listened to or looked for.

The receiver hears one frequency at a time, so a station's name can only
be read tuned to it. `findNames` does that with the radio off: powered up
with no sound, as for a scan, each station tuned to until the driver has
confirmed its name, at most 5 s, or 1.5 s for one sending no RDS at all.
Each name is in the status as it is found. It is refused (`errorCode` 16,
EBUSY) while the radio is on, since it would take the station away, and a
newer request - a powerOn first of all - ends it within a quarter of a
second. A scan made with the radio off looks for its stations' names the
same way right after, so a first scan gives a named list.

`audioError` is there only after PulseAudio refused the module. A refusal is `returnValue: false` with
`errorCode` (an errno) and `errorText`.

Seek and scan run on a worker thread. A newer request (tune, seek, scan,
power off) ends one that is still running, and that one is answered with
`errorCode` 125 (ECANCELED). A scan with the radio off powers the receiver up
without sound and down again afterwards.

The driver on this kernel has no seek or scan ioctl, so both are done
channel by channel with the soft-mute tune between `PRE_SEARCH` and
`RESTORE_SEARCH`. A station is also heard on the channels next to it, so
each run of neighbouring valid channels counts as one station, at its
strongest channel.

## Building and installing

CMake with the webOS build macros (`webos_build_system_bus_files`), like the
other `org.webosports.service.*`. It needs luna-service2, glib-2.0, libpbnjson
and libpulse-mainloop-glib. It installs `fmradiod`, `fmradiod.service`, and
the sysbus files, including a client grant for the Music app.

`fmradiod.service` runs it as root, so it can open `/dev/fm` and reach the
system PulseAudio at `unix:/var/run/pulse/native`. It does not wait for
PulseAudio: it connects when PulseAudio is there, and again after a restart.

## Tests

    tests/run.sh [workdir]               # unit tests: gcc, ASan+UBSan (gcc, clang), valgrind
    tests/static-analysis.sh             # gcc -fanalyzer, clang --analyze, clang-tidy,
                                         # cppcheck, flawfinder, sparse, Coccinelle

The unit tests drive `fm_tuner.c` and `fm_band.c` against a stand-in chip: power,
tuning, seek (leaving the current station, climbing to the peak, round the band,
cancelled), scan (merging runs, a channel that fails, cancelled) and the ioctl
encoding, and `fm_rds.c` (the RDS character set, trimming, the end of a
RadioText, control codes, never overrunning, the driver's record layout and
which of its copies count), and looking for a station's name (found, a
station with no RDS, one whose name is never confirmed, a failed read,
cancelled, off). `run.sh` builds and runs in `~/claude-scratch/fmradio-tests` unless
given a directory. `static-analysis.sh` takes the headers from pkg-config, or
from `FM_CFLAGS` for a Yocto sysroot, and the Coccinelle checks from a
kernel's `scripts/coccinelle` in `COCCI_DIR`.
