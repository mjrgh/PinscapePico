# Pinscape Pico Power Distribution Board

This is a prototype design for a power distribution board for virtual
pinball cabinets.  It's nominally designed to be used with the
Pinscape Pico expansion boards, but it could really be used with just
about any pin cab setup, with or without an I/O board like Pinscape.

## Danger - high voltage

This board provides connections to the 120VAC mains inputs to the
various DC power supplies.  It also has a section for 48VDC power.
Line voltage and 48VDC are both hazardous, capable of causing electric
shock and fires.  The board must be installed in the cabinet so that
it's shielded from accidental contact with the operator or with loose
articles, in a grounded metal enclosure (grounded to Earth ground
through the AC outlet) or an insulated enclosure.


## Features

This board has several functions special to pin cabs:

* "Smart" power switching: Virtual pin cabs typically have two or
more DC power supplies: the ATX power supply that runs the main PC,
and one or more auxiliary supplies for running feedback devices
(lights, motors, solenoids, etc).  There are often several auxiliary
supplies to meet the varying voltage requirements for the feedback
devices, such as a 24V supply for contactors and solenoids, a 48V
supply for pinball coils, and a 5V/12V dual supply for LEDs, motors,
and smaller solenoids.  Most people like to use a single "soft power"
button to control power to the PC, and would like all of the auxiliary
devices to switch on and off automatically with the PC.  This board
takes care of that for the auxiliary power supplies, by providing a
relay that switches the 120VAC mains power to all of the secondary
supplies according to the 12V rail from the main PC power supply.
Just plug an unused floppy-disk power connector from your PC ATX
power supply into the matching port on this board, and it'll control
the relay, which in turn switches AC power to your auxiliary power
supplies.

* DC power supply wire routing: Most of the switch-mode power supplies
(SMPS) that people use in pin cab projects have exposed screw
terminals for all of their wiring, including the 120VAC power input.
The power distribution board has a corresponding shrouded header for
each power supply ("shrouded" means that the pins are enclosed in a
plastic housing to protect against accidental contact).  This lets you
create a single pluggable cable for each SMPS, so that you don't have
to mess with the screw terminals as often - just wire the screw
terminal once, and then you can safely plug and unplug it via the
power distribution board connector.  This is simpler when doing
maintenance work, reduces the chances of errors when reconnecting
wires, and makes the wiring a little neater.

* Filtering: This board includes some large capacitors to help filter
voltage spikes on the secondary supplies and ensure quick power
delivery to high-current devices like solenoids, reducing brown-outs
and sudden voltage drops on the other supplies.

* Coin-door interlock: The board includes a coin-door interlock
connection for a 48VDC supply, so that power on the 48V network is
cut when the coin door is open.  This is a protective measure that
real pinball machines started including in the 1990s to protect
operators from high voltage when servicing the machine, and it's
a good feature to include in a virtual pin cab as well when you
include a high-voltage supply in the design.

* 6.3V lamp supply: The board includes a regulator that provides a
6.3VDC supply for #555 lamps, which are the lamps found in
arcade-style pushbuttons.  It's not easy to find a dedicated 6.3V
supply, so the board provides one by down-converting from the 12V
supply, which you'll probably already have in your system, since you
need that to run many motors, LED strips, and other devices.  The
pinball bulbs *can* run on 5V, but they'll look at bit dim at 5V,
so it's nice to have the actual voltage level they need available.

* OEM power supply ports: The board has ports designed to connect
to the screw terminals on typical 5V/12VDC, 24VDC, and 48VDC OEM
power supplies of the sort you can find on Amazon and eBay.  These
supplies almost all come with a set of screw terminals for 120VAC
line/neutral/ground inputs, and +/- DC outputs.  The power distribution
board has matching terminals for each type of power supply, so that
you can easily wire each supply to the distro board, keeping the
wiring tidy.  The 120VAC connections to the OEM supplies are all
switched through the "smart" relay, too.

* Power hub: The board includes an ample set of output ports for 5V,
6.3V, 12V, 24V, 48V, and GND connections, for running wiring to the
various feeedback devices.  This is a convenience to help tidy up your
wiring by providing enough ports that you won't have to improvise by
tying multiple wires together.


## Ground interconnects

This board connects all of the power supply grounds together, and
connects them all to the Earth ground from the 120VAC line power
input.  This is generally considered the safest configuration, because
it ensures that all voltages in the cabinet have the same 0V reference
point.

Note that all PC ATX power supplies connect the DC ground to the AC
Earth ground internally, since this is required by the ATX
specifications.  Power bricks and bare-metal-case power supply units
might or might not connect the grounds internally, but connecting them
to the board will interconnect the grounds whether or not they're
connected internally in the power supply units.


## Power capacity

The Molex Micro-Fit connectors used for the SMPS inputs are rated for
13A per pin, so this is the upper limit for each voltage level in the
system.  If you plan to use anywhere close to the 13A current limit,
use 18 AWG wire for the SMPS connections.

The different voltage supplies are all independent of one another, so
you can be using 13A at 12V and 13A at 24V at the same time without
overloading any of the connector pins.

If the power supplies you're using have lower individual current
limits, the lower limits will apply, again on an individual basis for
each voltage-level circuit.

The 13A limit on each voltage level should be adequate for most
virtual pinball cabinets.  The one likely exception is if you're using
very long LED light strips.  Those can draw 3A or more per meter, so
you'll reach the 13A limit at about 4 meters.  The best solution would
be to power those from a separate power supply connected directly to
the light strips, without going through the power distribution board.
