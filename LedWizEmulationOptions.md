# Pinscape Pico - LedWiz Emulation Options

Pinscape Pico offers two very different ways of emulating an LedWiz:

* Windows-side emulation via a replacement LEDWIZ.DLL

* USB protocol emulation

You should only use one or the other of these - each is a complete
emulation in its own way, and they're not meant to be used together.

The first approach, using the replacement LEDWIZ.DLL, is always the
better option - assuming that it's an option at all, which depends on
your situation.  It's preferable to USB emulation in two important
ways.  First, it makes it possible for legacy applications to access
all of the output ports on a Pinscape Pico, even if you have more than
32 ports, by making the Pico appear to legacy applications as multiple
virtual LedWiz units.  Second, it's fully compatible with all of the
*other* Pinscape features, such as all of the virtual input devices
(keyboard, gamepad, XInput, etc).  The DLL approach thus gives you
full access to all of the modern Pinscape features, while still
keeping your older LedWiz-only applications in the game, and giving
them access to all of your Pinscape-based feedback devices.

The only reason to use the second approach, USB protocol emulation, is
if the LEDWIZ.DLL replacement isn't on option.  Protocol emulation
might seem like a cleaner and purer way to emulate the LedWiz, but it
has some severe restrictions that arise from the limitations of the
old software that it's designed to support.  That's why it should only
be considered as a last resort, for cases where the LEDWIZ.DLL option
isn't workable.  That would include cases like a non-Windows OS on the
host machine, or a version of Windows that's too old to run the DLL,
or a machine with a brittle configuration that makes it impossible to
introduce the replacement DLL into the system.  If any of these apply,
or there's any other reason the DLL replacement won't work for you,
the USB protocol emulator provides a true, literally plug-and-play
solution that makes the Pico act exactly like an LedWiz at the USB
protocol level.



## Windows-side emulation via the LEDWIZ.DLL Replacement

This is the better option.  It's suitable if you're running Windows
8.1 or later, and all of your legacy LedWiz software accesses the
LedWiz through LEDWIZ.DLL (almost all LedWiz software does).

The replacement DLL is available here: [http://mjrnet.org/pinscape/dll-updates.html#LedWiz.dll](http://mjrnet.org/pinscape/dll-updates.html#LedWiz.dll).
That page has instructions for installing the DLL, which is mostly
just a matter of downloading the ZIP available there, unpacking it
to a local folder, and copying the file LEDWIZ.DLL to any application
folders where you have existing copies.

The replacement DLL exposes the same application interface as the
original manufacturer's DLL, so legacy applications that use the DLL
will think they're talking to the original.  The new DLL is also 100%
compatible with genuine LedWiz devices (and the various clones as
well), so you can throw away the original LEDWIZ.DLL and use the
replacement even if you're running a mixture of Picos and genuine
LedWiz devices.  In fact, you *should* throw away the original DLL,
because the new one is simply better; it's faster, and it fixes
some known problems in the original.

But for our purposes here, the real reason to use the replacement DLL
is that it can talk to a Pinscape Pico directly via Pinscape's native
USB protocols.  The DLL creates one or more virtual LedWiz units to
represent each Pico - more than one in cases where the Pico has more
than 32 output ports, since that's the limit for how many ports a
single LedWiz unit can have.  That makes it possible for legacy
LedWiz-based software to access all of the ports on the Pico, by
addressing the separate virtual LedWiz units.

The only thing you have to configure on the Pico side to use the
LEDWIZ.DLL replacement is the <tt>id.ledWizUnitNum</tt> property.
That specifies which LedWiz unit number (or numbers) LEDWIZ.DLL uses to
represent the Pico.  See the JSON Configuration Reference for details.


## USB protocol emulation

If you can't use the Windows-side emulation option for one reason or
another, Pinscape also offers the option of emulating the LedWiz's USB
protocol.  This makes the Pico emulate the LedWiz at the USB port
level, which makes it compatible with LedWiz-aware host software on
any operating system, and even on non-PC hosts.

This approach provides universal compatibility, unlike the LEDWIZ.DLL
replacement, which only works on Windows, and only with software that
accesses the LedWiz through the DLL.  So why is this a "last resort"
rather than the obvious first and best choice?  It's because this
approach places some severe restrictions on how you can set up the Pico,
limiting which Pinscape features you can take advantage of.
The restrictions don't come from Pinscape, but rather from the legacy
software that the protocol emulation exists to support.

The big limitation with the protocol emulator is that you'll have to
disable all of the Pinscape HID input devices: keyboard, gamepad,
XInput, Open Pinball Device, and Feedback Controller.  The reason this
is necessary is that most legacy LedWiz software doens't have any way
to distinguish among multiple HID interfaces when it recognizes an
LedWiz device.  The original LedWiz only has the one HID interface,
for its custom command protocol, so all of the old software based on
the interface will assume that the first HID it finds associated with
a device sporting the LedWiz VID/PID is in fact the LedWiz protocol
interface.  If you enable other HIDs, like the keyboard or gamepad
input, the old software will try sending LedWiz commands to the
keyboard or gamepad.  In the case of the original manufacturer
LEDWIZ.DLL, the result will be a hard crash of the application.

The other big restriction when using the LedWiz USB emulation is that
the LedWiz protocol is only capable of accessing 32 output ports per
device.  The LEDWIZ.DLL emulation works around that by making the Pico
appear as multiple virtual LedWiz devices, but that only works in the
DLL.  When using the actual USB protocol, there's no way that the
device can pretend to have multiple VID/PID codes, so it can't trick
the host into thinking it's more than one LedWiz.  So you're stuck
with the same 32-port limit that true LedWiz devices have.

I think the main scenario where you'd consider the protocol emulation
option is if you're just trying to replace a broken LedWiz in an older
system, and you don't need any other updates or upgrades - so you're
just using Pinscape Pico as a cheaper alternative to buying a new
LedWiz to replace your broken one.  In that limited scenario, it
shouldn't be a problem that you have to disable most of the other
Pinscape features, since you only want to get a broken system back to
working order, and you have no interest in adding new features.



### How to set up the protocol emulation

If you've read through the warnings above and you've decided that
the limitations are acceptable, here's how to set up the protocol emulator:

* Enable the LedWiz protocol on the Pico, via the JSON configuration file:
```
  ledWizProtocol: { enable: true },
```

* Disable all of the HID interfaces - keyboard, gamepad, XInput, Open Pinball Device.

* Disable the Feedback Controller interface (since it's yet another HID):
```
   feedbackController: { enable : false },
```

* Set the USB identifiers to the LedWiz codes:
```
   usb: { vid: 0xFAFA, pid: 0x00F0 },  // 00F0 -> unit #1
```
The <tt>pid</tt> determines the LedWiz unit number that applications
use to access the device: 0x00F0 is unit #1, 0x00F1 is unit #2, and
so on through 0x00FF for unit #16.

