# PWMWorker

This is a sub-project of Pinscape Pico that turns any Pico into a 24-channel
PWM controller with an I2C bus interface.  It's designed to serve as a
substitute for a dedicated PWM controller IC chip like TLC5940, TLC59116,
or PCA9685.

The main reason to use a Pico instead of one of the dedicated chips is
that the Pico is easy to work with in a DIY project, whereas all of
the dedicated PWM chips currently in production are only available in
fine-pitch SMD packages.  A Pico is as easy to solder to a circuit
board as a traditional through-hole chip, and you can even use it in a
completely solderless setup, connecting wires to its standard 0.1" pin
headers.  The Pico has a few secondary advantages over the dedicated
chips as well: it's inexpensive, readily available, and has some
high-level features that go beyond basic PWM output control, such as
integrated high-resolution gamma correction, configurable PWM
frequency over a very wide range (8 Hz to 65535 Hz), and direct
device-side support for Pinscape's "Flipper Logic" timer system (which
protects high-current devices by shutting down ports that the host
leaves stuck on for too long at a time).

Even though PWMWorker is nominally part of the Pinscape Pico project,
it's readily usable with essentially any microcontroller host, running
any software, that has support for a 3.3V I2C bus.  A Pico running
PWMWorker presents itself on the I2C bus the same way any other PWM
controller chip would, with a set of register locations that the host
can write to, to configure the output ports and set their PWM levels.
The register interface is very similar in style to that used by most
of the dedicated PWM controller chips, which was done deliberately
to make it easy to adapt existing microcontroller software that can
talk to any of those other chips to also talk to a PWMWorker.  PWMWorker
doesn't emulate any of those other chips, so it's not a drop-in replacement
for any of them, but its I2C register interface is so similar in design
that it should be light work to adapt an existing TLC59116 or PCA9685
driver to work with this device instead.

## Documentation

Documentation is provided in the form of a "data sheet", in keeping
with the conceit that this is a virtual "chip" in the PWM Controller
category.  The data sheet documents the I2C register interface and
circuit wiring details, and provides several examples of how to
connect output devices.
 
[PWMWorker Data Sheet](http://mjrnet.org/pinscape/PWMWorker/DataSheet/PWMWorkerDataSheet.htm)

## Build and install

PWMWorker is a stand-alone Pico C++ SDK project.  Refer to the main Pinscape Pico
build instructions, and do the same thing you'd do there, but working in this
directory.

The resulting .UF2 file can then be installed onto a Pico in the
standard way, by entering the Pico's Boot Loader mode (by power cycling
or resetting the Pico while holding down the BOOTSEL button on top of
the Pico) and then copying the .UF2 file onto the virtual USB thumb
drive that the Pico presents.

Once installed, the Pico acts like an I2C chip.  The physical I2C bus connection
is through the Pico's GP26 (SDA) and GP27 (SCL).  **No USB connection is required**,
since the Pico communicates with the microcontroller host purely through I2C,
but the USB port can be used to supply the Pico with power if desired.
Alternatively, the Pico can be powered by supplying 5V to the VSYS pin,
with a low-voltage-drop diode to protect against power back-flow when
the USB cable is connected.  See the Data Sheet for details.
