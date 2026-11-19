# dm-32-m17

Experimental firmware for the Baofeng DM-32UV with analog FM and M17 transmit
and receive. It is based on RadioFoundry and uses a fixed-point Codec2 3200
encoder and decoder for M17 voice.

## Install on the radio

> [!IMPORTANT]
> **Back up your radio before installing.** Use the Open25 flasher to make a
> verified full-radio backup and save it somewhere private. A codeplug export
> alone is not enough to restore the radio.

For the Baofeng DM-32UV handheld, open the
[Releases page](https://github.com/radiosification/dm32-m17/releases) and select
the newest release. Under **Assets**, download the versioned firmware ZIP
(for example, `dm32-m17-v1.zip`) and leave it zipped for the flasher.

1. Open [Open25-DM32 Flasher v3](https://kd8zmm.com/open25-dm32/v3/firmware.html)
   in desktop Chrome or Edge and connect the radio with its USB programming
   cable.
2. Complete and save the flasher's full-radio backup before selecting a
   firmware ZIP.
3. Use the flasher's local firmware ZIP option to select the downloaded
   `dm32-m17` ZIP and follow the prompts.
4. After the flasher verifies the write, restart the radio and check that it
   boots and shows the expected release in **Settings → My radio → About**.

The package contains the application and a checksum manifest. It does not
include a bootloader, stock AMBE codec, or codeplug.

## Get on M17

1. In **Settings → My radio → Radio name / M17 callsign**, enter your callsign
   as the radio name. It must be a valid M17 callsign of up to eight characters.
2. Select **M17** as the mode in the VFO or channel editor. Set the transmit
   and receive frequencies and the CAN (Channel Access Number) for the
   station or repeater you want to use. CAN values run from 0 to 15. This
   version sends voice calls to `@ALL`.

FM and M17 are the supported voice modes. Saved DMR, P25 and NXDN channels
from another firmware cannot be used in their original mode.

## Build the firmware

On Linux or WSL, you'll need Git, GNU Make, Python 3.11 or newer, and a
`csky-elf` cross toolchain with binutils and newlib for the CK803S. If you
don't already have the toolchain, see the prerequisites at the top of
[`firmware/tools/build-csky-toolchain.sh`](firmware/tools/build-csky-toolchain.sh).
Running that script with its default settings installs it under
`~/toolchains/csky-elf`.

From the root of the repository:

```sh
make bootstrap
CSKY_BIN="$HOME/toolchains/csky-elf/bin" make firmware
```

Set `CSKY_BIN` to the toolchain's `bin` directory if you installed it
elsewhere. If `csky-elf-gcc` is already on your `PATH`, use `make firmware`.

`make bootstrap` fetches the Codec2-mod and libm17 revisions pinned in
[`sources.json`](sources.json). The build runs a stack check and writes the
application binary to `firmware/build/dm32-m17/rfoundry_dm32.bin`. It does
not require stock firmware, an AMBE codec image, or an emulator.

Run `make test-host` after bootstrapping to check the M17 modem, framing,
and codec code on your computer. The host tests use Clang with AddressSanitizer
and UndefinedBehaviorSanitizer.

To build the installer ZIP instead of just the application binary, run
`CSKY_BIN="$HOME/toolchains/csky-elf/bin" make release-zip` after bootstrapping
(or `make release-zip` with the toolchain on `PATH`). This builds the same
firmware and writes `artifacts/dm32-m17-v1.zip` for version
`v1`. The [`VERSION`](VERSION) file sets the version shown on the radio,
in the ZIP manifest and in the filename. Increment it by one for each release
(`v1`, `v2`, `v3`, and so on). Use the same version for the Git tag.
The ZIP build checks that both dependencies match their pinned revisions
and have no local changes. For distribution, build from the commit you tag
and attach the generated ZIP to its GitHub release.

## Licenses

The original RadioFoundry code retains its [MIT license](firmware/LICENSE).
See [`COPYING`](COPYING), the [derivative notice](firmware/P25-DERIVATIVE.txt),
and the individual source notices for the other terms that apply to this tree.
The pinned dependencies and their licenses are listed in [`sources.json`](sources.json).
Their license texts are included for [Codec2-mod](firmware/LICENSES/Codec2-mod-LGPL-2.1.txt)
and [libm17](firmware/LICENSES/libm17-GPL-2.0.txt).
The firmware links newlib 4.5.0.20241231. Its upstream copyright and license
notices are in [`COPYING.NEWLIB`](firmware/LICENSES/COPYING.NEWLIB).
