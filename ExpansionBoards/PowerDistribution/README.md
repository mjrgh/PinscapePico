# Pinscape Pico Power Distribution Board

This is a prototype design for a power distribution board for virtual
pinball cabinets.  It's nominally designed to be used with the
Pinscape Pico expansion boards, but it could really be used with just
about any pin cab setup, with or without an I/O board like Pinscape.

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

* Power hub: The board includes a fairly ample set of output ports for
5V, 6.3V, 12V, 24V, 48V, and GND connections, for running wiring to
the various feeedback devices.  This is just a convenience to help
tidy up your wiring by providing enough ports that you won't have to
improvise by tying multiple wires together.
