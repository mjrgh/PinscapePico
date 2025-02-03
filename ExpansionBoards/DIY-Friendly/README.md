# Pinscape Pico - DIY-Friendly Expansion Board

This is an EAGLE design for a two-board expansion set for the Pinscape
Pico software.  Pinscape Pico is firmware for the Raspberry Pi Pico
that turns a Pico into a highly customizable I/O controller for
virtual pinball cabinets.

My first Pinscape Pico expansion board design was an All-In-One board
that packed everything you'd need for a pin cab into one 10x16 cm
board.  To achieve that kind of density, that design uses tiny SMD
(surface-mount) parts for just about everything, which makes it
difficult assemble by hand.  I sure don't want to try building it by
hand, and I doubt anyone else would either.  It's really designed to
be built in a factory by a pick-and-place machine.  That sort of
production is usually only cost-effective if you're going to make
at least 50 or 100 units, so I expect that the all-SMD board will
only be of interest to our more entrepreneurial community members
who might want to build the board and offer it for retail sale.
That's why I've taken to referring to that design as the "Pro"
expansion board.

But Pinscape has always been for DIYers.  My mission from the start
has been to make it possible to build high-end pin cabs without
depending on commercial devices, since we're such a niche hobby that
it's hard to get most commercial vendors to take us seriously as
customers.  So I didn't like the idea of only having a "Pro" design; I
also wanted to provide a DIY-friendly design that works with the new
software.  That meant designing another board using only parts that
are easy to solder by hand.

The result is this two-board expansion set.  It outfits the Pico with
a large set of physical input and output connections that let you
connect just about everything you'd find in a top-of-the-line pin cab,
from button inputs to feedback device outputs.  It has two main
design goals:

* It's comprehensive: it provides the extra peripherals required for
all of the Pinscape features (accelerometer, plunger input, button
input, feedback device control, IR remotes, TV ON power sensing), and
has ample button and feedback device ports to wire a fully decked out
pin cab

* It's DIY-friendly: it includes only through-hole parts that can
be easily soldered by hand

A third, lesser-but-still-important priority is to use common parts
that are widely available and are likely to remain in production for
many years.  The original Pinscape KL25Z boards are difficult
to build today because they depend on a few specific parts that are no
longer in production, most especially the KL25Z itself, but also the
TLC5940NT PWM chips.  I tried to avoid those sorts of dependencies as
much as possible in this design (and moving to the Pico as the
microcontroller at the heart of it is a big step in that direction).
The biggest unique-part dependency on these boards is the Pico itself,
but that's not just a requirement for the boards, but for the whole
software system.

Unlike the original Pinscape KL25Z expansion boards, where you were
encouraged to mix and match the Main, Power, and Chime boards in
different combinations to come up with a bespoke port layout, this
board set is designed as a One-Size-Fits-All solution.  The two boards
in the design are meant to work together as a set.  The second board
isn't an optional add-on, but is meant to be there from the start.
And the second board is meant to *complete* the set: there's no
provision for adding on a third board or a fourth board.  What we lose
in flexibility we gain in simplicity, in that you won't have to come
up with a wholly custom configuration for a wholly custom set of
boards.  And to make up for the loss in flexibility, the new board set
is ridiculously complete all by itself: it has so many ports that
you're almost certainly going to run out of space in your cabinet for
new toys before you run out of ports.  There are enough ports to cover
every standard toy in the DOF database, with a few left over for
your original ideas.

This is my "reference" expansion board, but I don't intend it to be
the one-and-only expansion board for Pinscape Pico.  The new software
is designed to work with lots of different hardware environments, so
there's lots of room for alternative board designs. 

## Files

Each board set consists of two sub-boards, titled **Main** and **Power**.
These two boards are designed to work together as a set.

Each sub-board design is contained in a pair of EAGLE files: **.sch** is the schematic,
and **.brd** is the physical board layout file.   If you want to have the
board manufactured, you only need the **.brd** file, since this is the
input to the EAGLE "CAM Processor", which generates the Gerber files
that you upload to the fabricator.

## Versions

* <b>Socketed Pico:</b>  This is the latest version of the board,
designed to install the Picos in 20-pin socket headers.  The sockets
are generic parts available from multiple manufacturers; one example
is Wurth 61302011821, but any generic 0.1" socket header will work.
On Mouser or Digikey, search the Headers & Wire Housings category for
2.54mm/0.1" pitch, 20 positions, 1 row, female, through-hole.)

* <b>SMD Sockets:</b> This is an intermediate design, with the
original direct-soldered Pico layout changed to use Harwin M20-7862042
sockets, which are surface-mounted (SMD) 20-pin sockets.  This design
was meant to be a stopgap until I could finish converting to through-hole
sockets, which are easier to solder to the board and easier to source.

* <b>Soldered Pico:</b> The original version of the board, designed
for the Pico to be directly soldered to the board.

The **Socketed Pico** version should be considered the most up-to-date
and authoritative version of the board.  As of this writing, they're
all functionally equivalent, but I don't plan to continue updating
the older designs, because I think almost everyone building the DIY
boards will prefer the through-hole-sockets version.  Installing the
Pico in sockets is a big advantage because it lets you easily remove
and replace the Pico if it ever breaks.  The SMD-sockets version of
the board accomplishes that, too, but it violates the "through-hole
parts only" mandate of this board set, plus the dependency on that
unique Harwin part number might make it difficult to source parts in
the future.  The through-hole sockets are generic commodity parts made
by multiple manufacturers, so they should be easy to source
indefinitely.


## Features

* 32 button inputs, using shift register chips for extremely low latency
scans (order of 10 microseconds)

* Accelerometer (LIS3DH), installed via a pre-assembled module
available from Adafruit (no need to hand-solder the tiny SMD chip),
for nudge sensing

* An optional ADC (analog-to-digital converter, ADS1115), installed
via a pre-assembled module available from Adafruit, for high-precision
analog input from a potentiometer-based plunger (improving on the Pico's
rather poor built-in ADC)

* 28 MOSFET outputs for high-current feedback devices, such as motors,
solenoids, and standard LED strips

* 16 flasher/strobe ports for high-current LEDs, up to 1A per port,
for driving the 3W RGB LEDs typically used for flasher panels plus
a strobe LED

* 32 medium-current ports, up to 500mA per port, for driving almost
any sort of lamp (small incandescent or LED bulbs like those used in
lighted pushbuttons, small LEDs, large LEDs), relays and contactors,
and anything else that runs on less than 500mA

* All output ports have full PWM control, with configurable PWM
frequencies up to 65000 Hz

* Plunger input port, pin-compatible with the original Pinscape KL25Z
plunger port; just plug in your existing Pinscape plunger

* IR transmitter and receiver, with the sensor and transmitter on
small separate boards (connected to the main board with cables), so
that can be positioned anywhere in (or outside) the cabinet, to place
them in line-of-sight with the devices they're communicating with

* Power-sensing circuit for the Pinscape TV ON function (to help you
implement seamless one-button startup of the whole pin cab, even when
using TVs that don't remember their power state)

* Real-time clock/calendar chip with battery backup, for time-keeping
across Pico resets, even when the board is unpowered

## How to connect devices

Connecting output devices works essentially the same way as with the
original Pinscape KL25Z expansion boards, so you can refer to
[Pinscape Outputs Setup](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=psOutputsExp)
in my virtual pinball Build Guide for general wiring instructions,
keeping in mind that these boards look a little different, and that
the software setup procedure is different.  There's also some more specific
advice about the different port types on this board in the subsections
below.

Some additional Build Guide chapters that might be helpful:

* [LED Resistors](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=ledResistors)
* [Coil Diodes](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=diodes)
* [Button Lamps](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=buttonLamps)
* [Flashers and Strobes](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=flashers)
* [Beacons](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=beacons)
* [Undercab Light Strips](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=lightStrips)
* [Contactors for flippers and bumpers](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=contactors)
* [Replay knockers](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=knockers)
* [Shaker motor](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=shakers)
* [Gear motor](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=gearMotors)
* [Blower fan](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=blowers)
* [Chimes and bells](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=chimes)


### Buttons

The board will work with any standard arcade-style pushbutton,
leaf-switch flipper button, microswitch, or just about any other sort
of button with a mechanical "Normally Open" switch.  For a microswitch
with three terminals, connect the Normally Open ("NO") and Common
("C", "GND") terminals, and ignore the third terminal.  Connect one
terminal to a button port on the expansion board, and connect the
other terminal to ground.  You can wire all of the button grounds
together in a daisy chain to save wire.  The order of the terminals on
the buttons doesn't matter.

Each button header includes a ground terminal.  This is purely for
your convenience in finding places to connect the grounds.  All of
the grounds are wired together, so none of them are "special" and
none of them have to be connected to specific buttons.

### Button lamps

For pushbuttons that have built-in lamps, you should think of the
lamp as a separate device from the button, and just wire it to one
of the lamp ports as though it were a standalone lamp.

### Lamps

Connect the positive terminal of the lamp or LED to its positive supply voltage.
(Incandescent lamps aren't polarized, so connect either terminal.  LEDs
have a "+" and "-" side that must be observed.)  Connect the negative
terminal to a lamp port on the board.

For LEDs, you must also connect a current-limiting resistor in series
with the LED.  Some LEDs have the necessary resistor built in.  See
[LED resistors](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=ledResistors).

The lamp ports are nominally for LEDs and incandescent lights, but you
can actually use them with any kind of device as long as its current
draw is below 500mA.  That's enough for most small relays and many
contactors, for example.  Be sure to use a [flyback diode](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=diodes)
for anything mechanical (relay, contactor, motor).  Diodes aren't
needed for LEDs or incandescent bulbs.

Each lamp port includes a 5V terminal, which can be used as the power
supply for lamps and LEDs.  The 5V terminal just connects straight back
to the secondary power supply 5V input to the board, so there's nothing
special about it; it's just there as a convenience, to give you another
place to connect to the 5V supply.  You don't have to use this terminal
for any of the lamps, and more importantly, you don't have to use 5V
for the lamps - each lamp can be connected to whatever voltage it
requires, as long as that doesn't exceed 40V.

### Flashers and strobe

The flasher ports are essentially the same as the lamp ports, except
that they have double the current capacity, up to 800mA, to allow
connecting larger LEDs (or to allow connecting two or three LEDs to
each port in parallel).  The flasher port is designed to be pin-compatible
with the flasher port on the original Pinscape KL25Z expansion boards,
using a 16-pin ribbon cable connector.  Other than the connector type,
it works exactly like the lamp ports.

The Strobe port is just a 16th flasher port on a separate connector,
with the same higher 800mA capacity.  Connect it the same way as any
lamp port.


### High-power outputs

Connect the positive terminal of the device to its positive supply
voltage.  Connect the negative terminal to a high-power port on the
expansion board.  Most motors and solenoids aren't polarized, so you
can connect the terminals in either order, but check each device to
make sure.

Motors, solenoids, contactors, relays, and anything else mechanical
require [flyback diodes](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=diodes).
Lamps and LEDs don't need them.


## Power limits

All of the output ports have limits on how much power they can handle.
The ports are all designed to have ample limits for the typical sorts
of devices used in virtual pinball machines, with the goal that you
shouldn't have to think about it much.  But even so, you should check
the points below to make sure that the devices you plan to connect are
within the limits.

### Combined active current level - main board

The combined current level for all devices on the main board that are
active **at the same time** must not exceed 44A.

This only applies to devices that are activated simultaneously.  The
total load of all devices *connected* can be higher, as long as you
don't attempt to run them all at the same time.

It's difficult to predict how devices will be activated while playing
virtual pinball or other games, so I usually just make some
conservative guesses, such as assuming the the maximum simultaneous
load in practice will be around four or five of the highest-current
devices.

(Rationale: all of the switched feedback device current ultimately
flows through the four ground pins on the 640445-x power supply
connector, so the maximum current shouldn't exceed what those pins can
safely handle.  Too much current can overheat the pins and damage the
circuit board.  That connector type's "absolute maximum" rating is 14A
per pin, so with four pins, that gives us a maximum of 56A.  If we
apply a 20% safety margin, we get 44A.)

### Combined active current level - power board

The power board has the same combined limit as the main board for
devices activated **at the same time**, 44A, for the same reason.

### MOSFET outputs (main board and power board)

Each individual MOSFET output must not exceed 11A, **or** 80% of the
I<sub>D</sub>[Max] for the MOSFET selected (22A for FDPF085N10A),
whichever is lower.

In addition, the power supply voltage for each device must be below
the MOSFET's drain-to-source voltage limit (100V for FDPF085N10A).

(Rationale: The current through these ports has to flow through both
the connector pins and the MOSFET, so it can't exceed the safe limit
for either part.  The 640445-x connector is rated for 14A maximum per
pin, or 11A after a 20% safety margin.  The MOSFET limit depends on
which type of MOSFET you use, but for the reference part, it's higher
than pin limit, so the pin's maximum current is the limiting factor.)

### Flasher and strobe outputs (main board)

Each individual flasher output must not exceed 800mA.  This is enough
current to run two typical 3W RGB LED channels in parallel.

In addition, the power supply voltage for the flashers shouldn't
exceed 40V.

(Rationale: the connector is rated for 1A per pin maximum, and the
ULN2803A is rated for 500mA per channel maximum, with each pin connected
to two channels.  This yields 800mA maximum with a 20% safety margin.)

### Lamp outputs (power board)

Each individual lamp output must not exceed 400mA.

In addition, the supply voltage for each lamp shouldn't exceed 40V.

(Rationale: each channel is driven by a single ULN2803A channel, which
is rated at 500mA maximum, or 400mA after a 20% safety margin.)


## ADS1115 ADC

This should be installed if, and only if, you're using a potentiometer
as your plunger sensor.

The Pico has its own built-in ADC (analog-to-digital converter), so
you might wonder why there's a slot for another ADC chip.  The reason
is that the Pico's built-in ADC has rather poor resolution and some
known problems with linearity.  A potentiometer-based plunger benefits
from a higher-quality ADC, in that it will yield smoother on-screen
animation tracking the motion of the physical plunger.

The ADS1115 is completely optional, even if you're using a
potentiometer plunger.  You can simply omit it if you don't need it or
want it.  If you're using some other kind of plunger sensor apart from
a potentiometer, the ADS1115 won't even be used for the plunger, so
you shouldn't bother with it.  You can even omit it if you *are* using
a potentiometer plunger, since the Pico's ADC will work for that in a
pinch, but the on-screen animation it produces might be a little
coarse.  If you find it good enough with the Pico ADC, though, you can
save a few dollars by not installing the ADS1115.  And you can always
add in the ADS1115 later, since it's on a separate board that just
plugs in.


## How to install the ADS1115 and LIS3DH modules

There are two easy ways to install these boards:

1. For permanent installation: you'll need four 6-pin, single-row,
standard 0.1" pin headers.  Install these in the matching pads inside
the marked "LIS3DH Module" and "ADS1115 Module" areas on the board, with
the plastic base on the top side of the board, and the short ends of the
pins facing down, through the holes in the pads.  To make sure they're
aligned properly, fit the module board over the top - you can leave
it there while soldering to keep the alignment right.  Solder the pins
from the bottom of the board.  Now complete the job by soldering the
pins to the module board from the top side.

2. To make the modules removable: you'll need four 6-pin, single-row,
standard 0.1" pin headers, **plus** matching through-hole sockets,
in the normal vertical orientation (**not** the bent-over 90&deg; kind).
Install the sockets in the pads on the Pinscape board with the plastic
socket parts on the top side.  Solder the pins to the board on the
bottom side.  Now insert the pin headers into the module boards
on the **bottom* of side of the module board, with the plastic base
on the bottom of the board and the **long** part of the pins facing
down away.  This will let you plug the board into the sockets you
just installed on the Pinscape board, with the module board facing up.
Solder the pin headers to the module boards from the top side.  Now
just plug the module board into the sockets on the Pinscape board.

## Connectors

### Buttons, High-Power Outputs, Lamp Outputs

All of these headers are designed for TE Connectivity's
[MTA 100 and MTA 156](https://www.te.com/en/products/connectors/pcb-connectors/intersection/mta-100-mta-156-connectors.html)
series connectors.  These are all single-row pin headers with
polarizing ramps, and matching wire housings with IDC or crimp-pin
termination.

MTA 100 uses 0.1" pin pitch, for low-current connections (button
inputs, lamp outputs).  MTA 156 uses 0.156" pitch (you can probably
see the naming pattern), for the high-power outputs.

You don't have to use MTA 100/156 specifically, because connectors
in the same size are available from many vendors, and they'll fit
the pads on the circuit boards.  However, the MTA 100/156 series
has some features that I like, so I used them as the reference.

* Ramps:  Plastic tabs on the backs of the board headers that
match cutouts in the wire housing/plug.  These prevent you from
plugging in the connector in the wrong orientation (the tab has to fit
the matching cutout in the housing to plug in properly).  They also
help lock the connector to the board so that it doesn't loosen or fall
out due to mechanical vibration.

* IDC housings:  TE makes matching connectors with IDC termination.
IDC stands for Insulation Displacement Connector, and it means that
you connect a wire to the plug by pushing the wire into a slot that
contains a metal blade that pierces the insulation and grabs onto
the wire.  It's a very quick and easy way to assemble the connectors;
I find it a lot more convenient than the crimp-pin style, which is
the main alternative.  TE also makes crimp-pin housings that match
these connectors for those who prefer that style.

Molex makes equivalent connectors that will fit the slots, also with
polarizing ramps, but Molex's are only available with crimp-pin housings.
There are also generic 0.1" and 0.156" headers that are just plain rows
of pins without the ramp.  Those are a lot cheaper, and they'll work
fine, but I think the ramp is worthwhile because of the locking action
and the protection against reversed connections.


### Plunger Connector and Flasher Outputs

These are two-row, 0.1" pitch headers, designed for use with IDC
ribbon cable connectors.  The footprints on the board are sized to
make space for shrouded headers, which include a plastic shell
around the pin header.  The shroud has a keying slot that matches
a tab on the IDC connector, so that you can only plug in the cable
in the correct orientation.

If you prefer, you can skip the shroud and use ordinary two-row 0.1"
headers.  Those are compatible with the same connectors, but you
won't get the wrong-way insertion protection.

These headers are designed to match the pin layout of the
corresponding connectors on the original KL25Z Pinscape boards, so
that you can move over existing equipment if you want to migrate an
existing KL25Z setup to the new system.  When installing the shrouded
headers, please double-check before soldering that the orientation of
the slot matches your existing connectors!


### QWIIC port

This is an optional connector for external boards using the QWIIC
standard, which is a modular cabling system created by SparkFun that
makes it easy to connect breakout boards to microcontrollers.
SparkFun, Adafruit, and many other hobbyist electronics/robotics
vendors make QWIIC-compatible boards for various chips, so I thought
this would be a handy thing to include to allow for experimentation
and custom add-ons.  This port doesn't have any pre-defined use in
Pinscape Pico, so you can omit the connector if you don't think you'll
use it (and you can always add it later in any case).

There's a similar standard called STEMMA, which Adafruit uses for all
of their boards.  STEMMA boards are compatible with QWIIC hosts, so
yes, you can use Adafruit boards with this port.  The difference
between the two standards is that a STEMMA board has voltage shifters
that let it work with 3.3V and 5V hosts, whereas QWIIC boards only
work with 3.3V hosts.  That makes STEMMA boards compatible with QWIIC
hosts.


## Parts Sourcing Notes

Most of the parts were chosen to be readily available generic parts
that are made by multiple manufacturers, and that have been around for
many years, and are likely to remain in production for years to come.
Hopefully that will make it easy to source parts, even if you're
embarking on this project long after 2025.

In case you do have difficulty sourcing any parts, though, here are
some notes on selecting substitutes.

QWIIC Connector: Optional; only needed if you want
to be able to plug in additional I2C boards that use the QWIIC cabling
system.  This connector is unfortunately only available in an SMD part,
so we had to violate our own "no SMD parts" design rule, but this is
one of the easier SMD parts to solder.  This is a 4-pin JST connector
with 1.0 mm pin pitch, currently part number SM04B-SRSS-TB(LF)(SN).

RGB LED: Most RGB LEDs in this form factor have similar specs, so it
should be easy to find a suitable substitute if necessary.  The LED is
powered from the Pico 3.3V supply, so the Vf (forward voltage) spec on
each channel must not exceed 3.3V.  If you substitute a different
part, you should recalculate the resistor sizes for the series
resistors on the three channels, based on 10mA current per channel.

SB140-T Schottky Diode: This was chosen to match the properties specified
in the [Raspberry Pi Pico Data Sheet](https://datasheets.raspberrypi.com/pico/pico-datasheet.pdf),
under **Powering Pico**.  Any similar device can be substituted:
If (forward current) at least 1A, Vf (forward voltage drop) at 1A
less than 400mV.

2N5195G: PNP switching transistor.  This switches 3.3V power to the
accelerometer and ADC, allowing the Pico to fully reset those devices by
power-cycling them.  Any PNP with gain of at least 50 and maximum
collector current of at least 200mA should work.

2N2222: Small-signal NPN transistor.  Any common small-signal NPN
should work, as this is used only for logic voltage switching.

1N5711: Small-signal Schottky diode.  Any similar Schottky should
work; look for Vf (forward voltage drop) around 500mV or less.

DS1307: A real-time clock/calendar chip with battery backup.  You can
simply omit this chip (as well as the battery and crystal) if you
can't find a substitute or don't need the RTC feature.  This type of
chip is one of those de facto industry standards where many companies
have been making clones of the same design for many years, so you
might actually be able to find a suitable replacement if DS1307 is no
longer available.  The new chip must obviously have the same pin
layout (unless you're willing to modify the board footprint), and it
must have an I2C interface.  It will probably be necessary to add
device driver support for any new chip you select to the Pinscape
firmware, although most of the chips in this category use nearly
identical command sets, so you might find that a new chip just happens
to work with one of the existing Pinscape RTC drivers.

Crystal (ECS-.327-12.5-13X-C): This is the time-keeping crystal
for the DS1307, so you can omit it if you're omitting DS1307.
You can substitute any crystal with 32.768 kHz frequency, 12.5 pF load
capacitance, and equivalent series resistance (ESR) less than 45K Ohms.

ULN2803A: 8-channel Darlington transistor array, with internal
resistors for logic-level drive.  This is a common chip type; any
Darlington array with the same pinout and similar internal resistor
values should work as a substitute.  This chip will drive 500mA per
output channel; if you substitute a chip with a different limit,
just be aware of the change in limit to the flasher and lamp outputs.
The Flasher outputs on the main board are arranged in pairs so that
each output can drive 2X the per-channel Darlington limit, while the
single-channel limit applies to the Lamp outputs on the second board.

PC827: Dual optocoupler.  Any xx827 part with the same pin layout
should work as a substitute.

S-80833CLY: Voltage detector; this detects when the voltage on the
secondary PSU 5V line drops below 3.3V, for the TV ON power sensing
circuit.  This is a common device category, known as Voltage Detectors
and also as Power-On-Reset (POR) chips, but unfortunately the pin layouts
and electrical characteristics vary a lot between devices from different
manufacturers, so it's unlikely that you'll be able to find a drop-in
substitute.  The one easy substitution you can make is that
the S-808xxCLY family has many variations with different threshold
voltages - that's what's encoded in the 'xx' part of the name, as
the voltage times ten, so 33 means 3.3V, 30 means 3.0V, etc.
Any other S-808xxCLY family member with a threshold voltage between
3.0V and 4.0V should work find for this board.  If you can't find any
other S-808xxCLY and have to substitute a different family altogether,
the circuit will work as long as the new part has a PUSH-PULL ACTIVE-LOW
output.  PUSH-PULL means that the device actively drives the output HIGH
when the monitored voltage is on, and ACTIVE-LOW means that the device drives the
output LOW when the monitored voltage falls below the threshold voltage.
The big inconvenience with substituting a different family will probably
be that you'll have to modify the footprint on the board to match a
different pin layout.

SN74HC74NE4: Dual flip-flop with asynchronous preset and reset.  The
74xx74 series is one of those venerable basic logic chips that's been
around for decades, and there are many variations with different
letters mixed into the part name string.  Most chips with a 74xx74
name pattern should be compatible with this board; to evaluate a
substitute, check that the pin layout matches the TI SN74HC74NE4, and
that it's compatible with a 5V supply voltage.

SN74HC165N: 8-bit parallel-load shift register. This is another
long-running series with pin-compatible versions from multiple
manufacturers.  Any 74xx165 alphabet-soup variation should have the
same pin layout, and can probably be substituted in this board as
long as it's compatible with a 3.3V supply.

UCC27524P: Dual low-side MOSFET gate driver with positive logic (HIGH
on the input translates to HIGH on the output).  This is another
common chip type with pin-compatible chips made by several
manufacturers.  This particular chip has two Enable pins (ENA, ENB),
while many variations with the same pin layout show the corresponding
pins as Not Connected (NC).  This board doesn't use the Enable
feature, so a substitute with NC pins in those positions is acceptable.
However, UCC27524P has one important feature that's **not** common in
this type of chip, which is that the inputs (INA, INB) have internal
pull-down resistors that deterministically turn the output off (LOW) when
the input is floating or disconnected.  This is a critical feature in this
design because it guarantees that the outputs will remain off during startup,
when the Pico's GPIO ports (which control the gate driver inputs) are
in high-impedance state.  If you aren't able to find a gate driver
with internal pull-downs, it would be necessary to modify the board design
to add equivalent external pull-downs, by adding a 10K resistor to ground
on each INA/INB line.  I specified UCC27524P because it makes the
extra pull-downs unnecessary, which reduces the part count
and saves space on the board.  The board is already pretty packed,
but it might still be possible to get the extra resistors in there,
either as discrete resistors or as bussed network resistors similar
to the ones used to pull up the button inputs.

4609X-101-103F: 8-resistor network, bussed.  This is a common part
made by many manufacturers.  The size of the resistors isn't very
important, because all they do is pull up an open button circuit to
3.3V; anything from 1K to 100K should work.  "Bussed" means that all
of the resistors are connected internally so that one end of each
resistor is tied to a "common" pin.  Any substitute must have the same
configuration.

MOSFETs: These are the driver transistors for the high-power outputs.
The choice of parts here is very open, because this board is designed
to work with almost any common "power" MOSFET.  The main properties
that you should look for are:

- Pin layout: choose a device in a TO-220 package (that's just the
code name for the form factor, with the black plastic body, three
big metal pins/legs, and a heat fin on the back).  This is a very
common package type for power MOSFETs, and there are probably
thousands of different parts made in this style.  One detail,
though: the pin layout isn't always the same just because it's
in a TO-220 package. Make sure that the part you choose has the
same order of pins as the board uses: when looking at the TO-220
package from the front, the pins, left-to-right, must be ordered
G-D-S (Gate, Drain, Source).

- Id[max] (maximum continuous drain current): Choose a MOSFET with an
Id[max] rating well above the maximum current of the toys you intend
to attach to these outputs.  For virtual pin cabs, the largest toys
tend to be motors and solenoids, which might draw up to 5 or 6 Amps,
so I'd choose a MOSFET with an Id[max] of at least 10A.  That's
actually relatively low by power MOSFET standards - many parts can
easily handle 20A or 30A - so this isn't a very hard requirement
to satisfy.

- Rds[on] (on-state resistance): There's no hard rule here, but since
we're using the device for on/off switching, lower is better.   Look
for something below 25 milli-Ohms.

- Vds[max] (maximum drain-to-source voltage, also known as breakdown
voltage): This also depends on the toys you're using.  Choose a device
with a breakdown voltage well above the highest supply voltage you
intend to use for your feedback devices.  I'd use a 2X margin if possible.
If you're using a 50V pinball knocker coil, use a 100V MOSFET.  It's
fairly easy to find TO-220 MOSFETs with such high voltages.

- Vgs[max] (maximum gate-to-source voltage): The board drives the
MOSFET gates with +12V.  Choose a part with Vgs[max] of at least 20V.
Most power MOSFETs will qualify, but check carefully if you find
one that's labeled as a "logic" MOSFETs.  Those are specially designed
to be switched by lower gate voltages, and might have a Vgs limit
of 10V or below, which isn't suitable for this board.


## Design notes

The material below explains some of my design decisions in creating
these boards.  This won't be of any practical interest if you're just
looking to build the boards for your pin cab, but it might be useful
as background information if you're looking to create new boards of
your own, so you can weigh what went into my decisions against your
own priorities.

### Polarized and keyed connectors

All of the connectors use polarized headers and plugs, which are
designed so that you can only insert the plug in the correct
orientation.  In addition, the connectors across the two boards are
"keyed" to make it impossible (or at least difficult) to insert a plug
into the wrong connector.  Each keyed header has a non-connected pin
marked "X" on the silkscreen.  On the pin header, this pin can be
snipped, to create a blank space in the header; then, the
corresponding position can be filled with a keying pin on the plug.
The keying pin fills the socket so that you can't plug it into a
header with a pin at that position.  Since the keying pin is in a
different position for each repeated header, this prevents you from
accidentally plugging a plug into the wrong header - it will only fit
the header with the snipped pin in the matching key-pin position.

Snipping the keying pins and filling the corresponding plug positions
is optional, but if you skip that, the keying protection obviously
doesn't work.  The polarized connectors will still prevent plugging in
a connector backwards, at least.


### I2C pull-ups

The I2C bus lines (SDA, SCL) require pull-up resistors to VCC, from
about 1K ohm minimum, to a maximum that depends on the total
capacitance of all devices on the bus (300ns/C, where C is the sum of
the bus capacitances over all attached devices).  If we assume that
the minimum bus capacitance in practice is about 20pF, the high end of
the resistance range is about 15K ohms.

In an ordinary self-contained circuit board, we'd just figure the
total bus capacitance of the design and set the resistors accordingly.
In this case, though, we have a slight complication, which is that
Adafruit boards come with their own built-in pull-ups, at 10K on each
board.  When both boards are installed, these combine in parallel for
5K effective pull-up resistance on each line.  The big complication is
that we'd like to consider the modules optional, so the user might
install both, one, or neither.  We also want to allow the user to
connect at least a few boards on the QWIIC port, and we should
probably assume that each of those will come with its own 10K
resistors.  So we should choose a pull-up size that will work in any
of these case.  The best bet seems to be to use a weak pull-up
on the board, at the high end of the valid resistance range, which
gives us 15K.  With a basic configuration with both Adafruit modules
installed, this yields a total resistance of 3.75K.  And it allows
installing seven more QWIIC modules with 10K resistance each before
the total falls below the minimum 1K.


### Peripheral device power

The 3.3V power supply to the peripheral devices (ADS1115, LIS3DH) is
provided through the Pico's 3.3V regulator, switched (high-side)
through a PNP transistor.  The PNP switch is controlled by a GPIO line
on the Pico.  On the schematic, this switched 3.3V power rail is
labeled 3V3_PERIPHERAL.

The point of powering the peripherals through a switched connection to
the main Pico 3.3V regulator is to allow the main Pico to power-cycle
all of the peripherals under software control.  This is meant to
improve overall robustness by allowing the Pico software to power
cycle everything during a Pico software reset, to ensure that all of
the external hardware is restored to initial power-on conditions.
It's not unheard of for I2C chips to get into error/fault states that
can only be cleared with a power cycle.  I haven't actually run into
any such problems with any of the chips used on this board, but the
reset capability still seems worth including as a fail-safe, since
manually power cycling the whole board could be quite inconvenient in
a pin cab setup where the board is installed inside the cabinet.

The I2C pull-ups are connected to 3V3_PERIPHERAL rather than the main
Pico 3.3V power.  This is intentional: it's to ensure that we don't
feed power into any of the peripherals through their SDA/SCL pins
while their main supply power from 3V3_PERIPHERAL is at 0V, during one
of our software-controlled power cycles.  Some chips can't tolerate
voltage on logic pins above VDD, so it's safest to turn off the
SDA/SCL pull-ups whenever the peripheral VDD is cut off.


### Pico hard resets

A RESET button is positioned adjacent to each Pico, to force the Pico
to reboot.  This doesn't power-cycle the Pico; it just grounds the RUN
pin, which initiates the equivalent of a power-on reset.

Each auxiliary Pico's RUN pin is connected in parallel to an NPN
switch that grounds RUN (resetting the Pico) when the
/PERIPHERAL_POWER_ENABLE signal is HIGH.  This allows the main Pico to
reset all of the auxiliary Picos under software control, at the same
time that it resets all of its other peripherals via the peripheral
power PNP switch.  This extends the software-controlled peripheral
reset capability to the aux Picos even when they're getting power from
their USB ports.  (It was also undesirable to power them through the
3V3_PERIPHERAL network, since they have relatively high power draw
that might have required a bigger PNP switch, and because their
regulators are more efficient with 5V supplies.)


### Pico AGND connected to GND

The Pico AGND (analog ground) pin is connected directly to the main
ground plane on the board.  The Pico data sheet allows that this is
acceptable "if the ADC is not used or ADC performance is not
critical", but it's conspicuously silent on what you should do instead
if the ADC *is* used and you *do* care about its performance.  The
authors probably felt that the subject is too complex to boil down to
a one-size-fits-all wiring diagram, so they left it as an exercise.
But I'm not an analog engineer, so in the absence of any such
guidelines, I just went with the naive approach of connecting the AGND
pin straight through to the main ground plane.

Which I think will be fine for our purposes.  Pinscape does use the
ADC - for plunger input, if the plunger sensor is one of the types
with an analog output line.  But ADC performance isn't exactly
"critical" for most of these.  The one plunger type where ADC
performance is pretty important is the potentiometer-based plunger,
but for that case, we don't have to use the Pico ADC at all since we
have the superior ADS1115 instead.  That should yield better results
than the Pico's on-board ADC can produce even under ideal conditions.

If any analog engineers are reading this and want to weigh in on a
better AGND wiring plan, I'd be interested in hearing any suggestions.


### Modules for accelerometer and ADC

It's impossible today to buy an in-production accelerometer chip in a
hand-solderable package.  All of the chips currently on the market are
in tiny SMD packages meant to be handled by pick-and-place machines,
not by human hands.  And while there are a few ADC and PWM chips still
made in through-hole packages, the available options are too limiting
for our purposes; all of the good options in those categories are
SMD-only.

My solution, to satisfy the DIY imperative for these boards, was to
use "modules" for these cases.  The design has plug-in slots for
Adafruit breakout boards for an LIS3DH accelerometer and an ADS1115
ADC.  I chose the Adafruit boards because they're reasonably priced,
well designed, and well documented, and Adafruit is a reliable company
that tends to keep their boards in production for many years.

Using breakout boards instead of the raw chips raises the build cost
slightly, but not by much when you consider that the breakout boards
also include all of the supporting components (capacitors and
resistors), and that they eliminate the extra cost you'd incur if you
had to buy the tools and supplies for doing the SMD soldering
yourself.  They also eliminate the risk that the soldering job goes
wrong, which is definitely non-zero with tiny SMD parts.  I think most
people who want to assemble these boards by hand will find the small
added cost well worth it for the savings in time and effort and
uncertainty.

### Shift register button inputs

This board uses 74HC165 shift registers for the button inputs.  The
Pico doesn't have enough native GPIO ports to allow connecting buttons
directly to GPIOs, as the original Pinscape KL25Z expansion boards
did, so we need some additional hardware to let the Pico read more
digital inputs than it has GPIO ports.  The 74HC165 is a convenient
way to do this.  Each 74HC165 has four input ports that the Pico
can read serially through GPIO ports.  What's more, multiple 74HC165
chips can be strung together in a daisy chain, without requiring any
more GPIO ports on the Pico.

There are other chips that can accomplish the same thing, including
"GPIO expander" chips like PCA9555.  What makes 74HC165 particularly
good at the job is that the Pico can read its inputs at extremely high
speeds, clocking in serial bits in the megahertz range.  This allows
the Pico to scan the full set of button ports every 10 to 20 microseconds,
for essentially zero latency sampling on all button inputs.


### Picos as PWM controllers

The design calls for *four* Pico units.  This is actually just another
case of the "module" principle above, but in this case the module is
a whole Pico.

One Pico - the one marked "Main Pico" on the main board - is actually
used as a microcontroller.  This is the one that runs the Pinscape
firmware, and connects to the PC host via USB.

The other three Picos aren't acting as Pinscape units, and they're not
even really being used as full microcontrollers.  They're just serving
the role of PWM controller chips, essentially substitutes for
something like a TLC5940 or TLC59116.  PWM chips are another of the
categories where you simply can't buy a through-hole part any more for
any in-production chips that are suitable for the task.  To deal with
this, this board design uses Picos as PWM controllers.  This sounds
crazy, using a 32-bit dual-core CPU as a humble PWM chip, but it's
actually pretty cost-effective.  The Pico is only $4 per unit, and can
control 24 PWM ports (with a little software magic).  Most of the
dedicated PWM controller chips can control 16 channels and list for
about $2-$3 in retail quantities, and require a couple of supporting
components.  So using a full Pico in place of a PWM chip doesn't
actually increase the cost much.  The Pico takes up a lot of board
space compared to a fine-pitch SMD PWM chip, but that's not a fair
comparison, since fine-pitch SMD chips aren't welcome in this design.
If we compare it to the space a comparable through-hole PWM chip would
take up - *if* there were such a thing - it wouldn't be that
different.  And in terms of PWM functionality, leaving aside form
factor and cost, the Pico is a clear win over dedicated PWM chips.  It
can handle a wide range of PWM frequencies, from single-digit Hertz
into the hundreds of kHz, and it can achieve 12-bit step resolution up
to about 30 kHz, or 16-bit resolution up to 1900 Hz.  (High
frequencies are useful when controlling devices like motors and
solenoids, since inductive devices can vibrate at the PWM frequency,
causing audible acoustic noise; this can be eliminated by setting the
frequency above the human hearing range, 20 kHz or higher.  High
resolution is useful for lighting devices like LEDs.  Setting the
frequency to 20 kHz is a good compromise that should eliminate
inductive device noise while retaining excellent step resolution for
smooth brightness fades with LEDs.)  And since the Pico PWM scheme is
entirely software-based, it's hugely flexible for new features and
improvements in the future.

The three auxiliary Picos *don't* run the Pinscape firmware.  Instead,
they run a separate "PWMWorker" program designed specifically for this
task.  The PWMWorker program makes the auxiliary Picos act as I2C
slave devices, so that the main Pico (the one running Pinscape) can
communicate with them to coordinate their PWM functions.

The auxiliary Picos are powered from the main Pico's USB bus power
connection, so they don't even need to be plugged into USB for power
(although it's also safe to plug them into a powered USB connection).
And they don't need a USB data connection to the host during normal
operation.  They carry out their PWM functions by communicating
directly with the main Pico via the I2C bus connection.  USB is only
needed when you want to program them with new firmware, or if you want
to access them from the host for monitoring or troubleshooting
purposes.

### Picos - sockets vs soldered

The original board design required the Picos to be soldered directly
to the board.  This made it easier to design the board by making more
room to route traces, but it had the major disadvantage that you
couldn't replace a Pico if it ever broke.  The newer versions of the
board are modified so that the Picos are installed in 0.1" socket
headers.  The socket headers are generic parts available from several
manufacturers; one example is Wurth part number 61302011821.

Note that the socketed-Pico version of the board **requires** using
the sockets.  You shouldn't attempt to directly solder the Picos, even
though it might look like you can do that from the shape of the pads,
because the USB connectors on the Picos are set in a little too far
from the edge of the board in this version.  That's likely to get in
the way of the USB cable for a direct-soldered Pico.  Using the
sockets lifts the Pico vertically above the board enough that it
leaves room for the USB cable.