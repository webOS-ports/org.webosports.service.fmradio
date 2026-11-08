org.webosports.service.fmradio
==============================

Summary
-------
LuneOS service for a phone's FM radio: tuning, seeking, scanning and RDS.

Description
-----------
A phone's FM radio as a LuneOS service: on and off, tuning, seeking and
scanning the 87.5 - 108 MHz band, the signal strength, and the station's name
and RadioText from RDS - also the names of the stations not listened to,
looked for with the radio off. Three receivers are known, and the service
uses the first that is there (`fm_tuner_detect`):

- MediaTek's `fmradio_drv` on `/dev/fm` (`src/fm_mtk.c`), the MT66xx combo
  chips. Tested on the MindPhone.
- Qualcomm's `radio-iris` on `/dev/radio0` (`src/fm_iris.c`), the FM core
  reached over SMD (WCNSS). Tested on tissot-halium (msm8953), which turns
  on, tunes, seeks and scans. Reception there is weak: indoors 95.0 MHz
  plays through noise, seek and scan stop on noise, and RDS is untested.
- Qualcomm's WCN3990-class chips ("cherokee", e.g. the BlackBerry KEY2's
  SDM660), whose FM has no kernel driver (`src/fm_helium.c`): HCI commands
  through `vendor.qti.hardware.fm@1.0::IFmHci`, which the vendor's Bluetooth
  HAL registers, over libgbinder (`src/fm_helium_hidl.c`). Found by the
  vendor's `vendor.qti.hardware.fm@1.0-impl.so`. Written from the radio-iris
  command set and Qualcomm's helium FM HAL; not yet run on a device.

Sound goes through PulseAudio: while the radio is on, `fmradiod` keeps
`module-droid-fm` (pulseaudio-modules-droid) loaded, which patches the chip's
output to the audio HAL's speaker or headphones. The wired headphones are the
antenna. On both Qualcomm receivers the module is loaded with `mode=parameters`: Qualcomm's audio
HALs take FM through `set_parameters` (`handle_fm`, `fm_volume`), not as an
audio patch. Where the module is missing the status has an `audioError`.

A device with none of them still runs the service; `getStatus` then says
`available: false`, and a client shows no FM.

API
---

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
`name` and `text` only once the driver has passed them on (on MediaTek the
name after three receptions agree, the text once a whole RadioText came in;
on IRIS what the driver queues), in UTF-8 from the RDS character set. Both are cleared when the frequency changes.
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

MediaTek's driver has no seek or scan ioctl, so both are done channel by
channel with the soft-mute tune between `PRE_SEARCH` and `RESTORE_SEARCH`.
A station is also heard on the channels next to it, so each run of
neighbouring valid channels counts as one station, at its strongest channel.

On IRIS the chip searches itself. Before the node is opened the SMD link to
it is opened (`radio_iris_transport`'s `fmsmd_set`), and the chip is given
200 ms to answer; if turning it on still fails, the link is closed and opened
once more. A seek is the chip's, round the band; it stops at once on the
channel it starts from, so it is then repeated from the next channel, and up
from 108.0 it starts from 87.5. A scan is the chip's list of the 20 strongest
from a channel up (about 9 s each), repeated past the last while it lists 20,
then each station tuned to for its strength. The driver keeps a list until it
is read, so any left over are read away before each search.

On helium the service sends the chip what radio-iris' `initialise_recv`
sends after the receiver is on (soft mute, stereo with blending, the event
mask), then the band, the wired antenna and the channel, and opens the
SLIMbus port the audio leaves the chip on, as Qualcomm's FM app does before
it routes FM. Seeking is the chip's, as on IRIS, with the same care for a
chip that stops on the channel it starts from. A scan is seek after seek
from 87.5 up until the chip comes round again: the chip's station list is
not used, because radio-iris and helium lay its request out differently and
which one this chip reads is not known. A cancelled search is waited out
for its Search Complete, which would otherwise end the next search at once.

Building and installing
-----------------------

CMake with the webOS build macros (`webos_build_system_bus_files`), like the
other `org.webosports.service.*`. It needs luna-service2, glib-2.0, libpbnjson,
libpulse-mainloop-glib and libgbinder. It installs `fmradiod`, `fmradiod.service`, and
the sysbus files, including a client grant for the Music app.

`fmradiod.service` runs it as root, so it can open `/dev/fm` and reach the
system PulseAudio at `unix:/var/run/pulse/native`. It does not wait for
PulseAudio: it connects when PulseAudio is there, and again after a restart,
trying every second at first and then less often, up to every 16 seconds,
with one warning per outage.

Tests
-----

    tests/run.sh [workdir]               # unit tests: gcc, ASan+UBSan (gcc, clang), valgrind
    tests/static-analysis.sh             # gcc -fanalyzer, clang --analyze, clang-tidy,
                                         # cppcheck, flawfinder, sparse, Coccinelle

`tests/test_tuner.c` drives `fm_tuner.c`, `fm_mtk.c` and `fm_band.c` against a
stand-in MediaTek chip: power,
tuning, seek (leaving the current station, climbing to the peak, round the band,
cancelled), scan (merging runs, a channel that fails, cancelled) and the ioctl
encoding, and `fm_rds.c` (the RDS character set, trimming, the end of a
RadioText, control codes, never overrunning, the driver's record layout and
which of its copies count), and looking for a station's name (found, a
station with no RDS, one whose name is never confirmed, a failed read,
cancelled, off). `tests/test_iris.c` does the same for `fm_iris.c` against a
stand-in radio-iris: the link (opened first, slow, staying down), power-up
settings and their failures, a tune the chip reports late, the signed RSSI,
the PS, RadioText and station-list records, the group counters, the chip's
seek (from the top, stopping on its start, round the band, cancelled, timed
out) and its list scan (several passes, a list left over, cancelled).
`tests/test_helium.c` drives `fm_helium.c` against a stand-in HCI chip: the
link, the power-up commands and their parameters, each failing, answers
matched to their command, a command never answered, malformed events, a
tune reported late or never, the signed RSSI, RDS on and off, the PS and
RadioText events, the group counters, the chip's seek (round the band, from
the top, stopping on its start, cancelled, timed out, a cancel ended late or
not at all) and the scan by seeks.

`tests/test_audio.c` drives `fm_audio.c` with the real libpulse against a
socket standing in for PulseAudio: refusing (not up yet), missing, taking a
connection and dropping it. fmradiod holds one connection at a time, logs one
warning per outage and leaves no timer behind when freed.

`tests/test_join.c` checks the join at exit: a thread that, winding down,
waits for an answer only the main context delivers (as the helium link's
are) gets it, and the join neither hangs nor sits out the thread's timeout.
`run.sh` builds and runs all five in `~/claude-scratch/fmradio-tests` unless
given a directory. `static-analysis.sh` takes the headers from pkg-config, or
from `FM_CFLAGS` for a Yocto sysroot, and the Coccinelle checks from a
kernel's `scripts/coccinelle` in `COCCI_DIR`.
