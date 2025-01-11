# Building the project from source

If you wish to build the project from source, clone the github
repository to your PC.  (You can also download a snapshot of the
github code as a ZIP file and unpack it into a local directory on your
PC.)  The firmware and Windows tools use completely separate build
systems; see below for instructions for each.


## Pico SDK setup

Building the firmware requires the Pico C++ SDK from Raspberry Pi.
For most of the project's development phase, I was using SDK 1.5.1;
shortly before the first release, I made the (perhaps risky) move of
updating to the latest SDK version, 2.1.0.  I did most of my
development and testing against 1.5.1, but I'd nonetheless recommend
moving to 2.1.0, so that you're on the latest from Raspberry Pi,
**with the the important caveat** that you use my fixed version of
2.1.0 instead of the official one.

### How to install SDK 1.5.1

If you want to go with the tried-and-true 1.5.1, that's easy.
Raspberry Pi provides an official one-click Windows installer, at
https://github.com/raspberrypi/pico-setup-windows/.  Download
and run the installer, and follow the on-screen prompts.

### How to install SDK 2.1.0

Raspberry Pi deprecated the Windows installer for 2.1.0, in favor of a
Visual Studio Code extension.  In principle, that should be just as
easy to set up, but it still seems to be a work in progress - I can't
even get it to install on my machine.  What's more, the official 2.1.0
library has a couple of serious errors, one that will simply cause a
compiler error when you try to build, and a second, more insidious
error that will make the USB connection unstable when you deploy the
firmware.

To address the install problems and library errors, I've created my
own unofficial 2.1.0 Windows build environment, with everything you
need pre-built and pre-packaged, and fixes for the library
regressions.  This is for x64 Windows only.  If you want to build on
another platform, you should go directly to the Raspberry Pi site and
follow their instructions.  However, you should still get my modified
snapshot of the pico-sdk/ (library source code) tree, and replace
the one in the official version, so that you get my library fixes.

My 2.1.0 snapshot is available at https://github.com/mjrgh/pico-sdk-2.1.0.
Unlike the official 1.5.1 installer, this is just a snapshot of my
environment, so there's nothing to "install"; you just copy the whole
directory tree into a folder on your hard disk.  To do that, you can
either `git clone` the repository, or you can download it all as a ZIP
and unpack it in a local folder on your machine.  (To get the ZIP,
click the **Code** drop-down box at top right of the main page, and 
select **Download Zip**.)

### NMAKE

For any version of the SDK on Windows, you'll also need to install
Microsoft's NMAKE build tool.  That's not included in any of the
pre-built snapshots (mine or the official Raspberry Pi releases)
because it's proprietary Microsoft software - but Microsoft makes it
freely available, even though they don't let anyone else distribute
it.  The easiest way to get that is to install the free Visual Studio
Community Edition.

### Pico SDK documentation 
For tons of documentation about how to use the Pico SDK, see
[Getting started with Raspberry Pi Pico](https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf).
That has instructions for setting up the build environment on other
platforms (Linux, MacOS) if you prefer to work on one of those.
But you won't need to read all of that if you just want to build the
Pinscape firmware, as the instructions below should cover it.

## Building the main Pinscape Pico firmware program

The steps below assume that you've already set up the SDK tools
on Windows as explained above, and assumes that you're using the
command-line build process (as opposed to VS Code or some other IDE).

* Open a Windows Terminal window (i.e., a CMD prompt/DOS box)

* Set the working directory (`CD`) to your PinscapePico\firmware folder

* Type `c:\pico-sdk-2.1.0\pico-env.cmd` (replacing the path with the actual
location where you installed the Pico SDK files, if it's different)

* `del CMakeCache.txt` and `rmdir /s CMakeFiles` if those exist from previous builds

* Type `cmake -S . -G "NMake Makefiles"` (**exactly** as shown, including capitalization and quotes)

* Type `nmake`

That should produce a long series of progress messages from the compiler, hopefully
all bearing good news (no warnings or errors), and ultimately finishing with:

`[100%] Built target PinscapePico`

You should now see a file in the working folder called `PinscapePico.uf2`.  That's
the compiled firmware program, ready for installation onto your Pico.  Install
this on the Pico using the standard procedure for all Pico firmware installs:

* Unplug the Pico from USB (and all other power sources)

* Press **and hold** the button on top of the Pico while plugging in the Pico's USB cable, then release the button

* Copy the UF2 file onto the Pico's Boot Loader virtual thumb drive

## Building the PWM Worker firmware

There's a separate firmware program, called PWMWorker, in another folder
(of that name) alongside `firmware`.  This program is designed for use with
the DIY-friendly expansion boards, which use auxiliary Picos to perform the
job of controlling PWM outputs, which we might otherwise do with a dedicated
PWM chip like a TLC59116 if any such chips were available in a DIY-friendly
package.  But all of the PWM chips on the market today are fine-pitch SMD
chips that are difficult to solder by hand, so we use Picos instead.  To
make the auxiliary Picos act like PWM controller chips, we install this
special firmware.

You can build this firmware using the same procedure outlined above
for the main Pinscape firmware.


## Building the Windows programs (GUI Config Tool, Command Line Config Tool, API libraries)

All of the Windows programs can be built through a single Visual Studio "solution" (.sln)
file.  The process is fully automatic once you load the .sln into Visual Studio.

* Install Visual Studio 2022 or later.  Community Edition is free;
download it from the official Microsoft site at https://visualstudio.microsoft.com/vs/community/.

* Launch Visual Studio

* Click **Open a project or solution**

* In the file selector, navigate  to your PinscapePico folder, and select **PinscapePico.sln**

* Select **Build** > **Build Solution** on the main menu
