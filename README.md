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

* Windows 10 x64 (testing/backport to Windows 7 32/64-bit is in-progress).
* An audio interface which supports ASIO. If your hardware doesn't come with a
  native ASIO driver, you may be able to use ASIO4ALL instead.
* Digital audio workstation software. SAR is mainly tested using [REAPER](http://www.reaper.fm/).

## HOWTO

Once you've installed SAR, nothing will immediately happen. To configure it,
first start your DAW (for beta: as admin!) and open its audio configuration
settings. Select the Synchronous Audio Router ASIO driver and open the ASIO
configuration dialog. Under Hardware Interface, select the ASIO driver for
your physical audio device.

You can add an unlimited number of Windows audio endpoints from the SAR
configuration dialog. "Playback" endpoints will allow Windows applications
to play sound and show up as corresponding input channels on your ASIO device,
while "Recording" endpoints allow Windows apps to record output sound and
correspond to ASIO output channels. Once you've added your channels, make sure
they're enabled in your DAW -- most have a dialog or dropdown where you can
select which channels are to be used.

If you receive errors initializing the SAR ASIO driver, make sure you are
running your DAW as admin. This is a requirement of the SAR alpha build and
will be relaxed eventually.

Note that the endpoints created by SAR are only active while your DAW is
running and has started ASIO. They are automatically disconnected when the
ASIO driver is closed.

