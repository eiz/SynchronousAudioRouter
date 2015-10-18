# Warning

This software is in early development. It is not ready for end-user consumption.
Check back soon for updates.

# Synchronous Audio Router

Synchronous Audio Router is a Windows audio driver which allows you to route
application audio through your digital audio workstation software. It has a few
unique characteristics compared to similar virtual audio cable software:

* All virtual audio streams are synchronized to a physical audio interface to
  mitigate clock drift/buffer underrun problems.
* Allows dynamic creation of an unlimited number of Windows audio devices with
  custom names/metadata.
* Allows per-app override of Windows default audio device for apps which
  don't include a way to select an audio device. Regex matching lets you set
  rules for many apps simultaneously, e.g. route every app under your steamapps
  directory to a specific endpoint.
* Designed for use with DAW software. Instead of treating all virtual audio
  streams independently, they're mapped to a single multichannel ASIO interface
  which also includes all channels from the physical audio device.
* Low latency. Since SAR is synchronous with the hardware audio interface and
  uses WaveRT to transport audio to/from applications, it doesn't impact your
  DAW's latency. It's practical to use 1-2ms buffer sizes on a cheap USB 2.0
  interface.
* Extremely simple UI. Because the DAW is expected to do all the hard work of
  audio processing, SAR doesn't need to include a complicated mixer interface or
  extensive configuration options.

## System Requirements

* Windows 10 x64.
* An audio interface which supports ASIO. If your hardware doesn't come with a
  native ASIO driver, you may be able to use ASIO4ALL instead.
* Digital audio workstation software. SAR is mainly tested using [REAPER](http://www.reaper.fm/).

## Installation

The alpha build of SAR requires test signing mode to be enabled on your machine
prior to installation. To enable test signing mode, open a command prompt as
admin, and run the command `bcdedit -set testsigning on`, then reboot.

Once you've installed SAR it will be available as an ASIO driver in any software
which supports ASIO. Your audio software should have an "ASIO Configuration" or
"Control Panel" button which will open the SAR user interface and let you select
your physical audio device and create endpoints. Note that some software may
need to be restarted to detect changes to the ASIO configuration (e.g. in REAPER
you need to close and re-open the configuration dialog after adding endpoints
for them to show up in the first/last dropdowns).

The alpha build also requires your DAW to be run as admin. This restriction will
be removed for the final release.

