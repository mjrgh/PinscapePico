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

* A switched 120VAC output port is also provided, for connecting to
a plain power strip as its AC power input, turning the plain power
strip into a smart switched power strip that turns on and off
according to the main PC power status.  You can use this to control
power to other equipment in the pin cab, such as TVs/monitors and
audio amplifiers, so that everything turns on and off automatically
according to the main PC power status.  This eliminates the need to
buy a separate "smart power strip" for other equipment.

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
wires, and makes the wiring a little neater.  The 120VAC power
connectors to these headers use the switched "smart" power lines, so
all of your SMPS units wired through these connectors will
automatically power on and off according to the main PC power status.

* Filtering: This board includes some large capacitors to help filter
voltage spikes on the high-voltage 48V supply, to help provide rapid
delivery to high-current devices like solenoids.  This should also
help reduce electrical interference from those devices.

* Coin-door interlock: The board includes a coin-door interlock
circuit for the 48VDC supply, so that power on the 48VDC rail is cut
off when the coin door is open.  This is a protective measure that
real pinball machines started including in the 1990s to protect
operators from accidentally touching a live high-voltage wire while
they have the machine opened up for servicing, and it's a good feature
to include in a virtual pin cab for the same reason.  The wiring to
the coin door switch itself is a low-voltage (5V) logic circuit, for
added safety.

* 6.3V lamp supply: The board includes a regulator that provides a
6.3VDC supply for #555 lamps, which are the lamps found in
arcade-style pushbuttons.  Those pinball bulbs *can* run on 5V, but
they'll look at bit dim at 5V, so it's nice to have a 6.3 supply.  But
it's hard to find a 6.3V SMPS, so this board provides its own 6.3V
output via a voltage regulator sourced from the 12V supply.  The
regulator can supply up to 5 Amps on the 6.3V output, which is enough
for about 20 incandescent #555 bulbs, or over 100 LED equivalents.

* OEM power supply ports: The board has ports designed to connect
to the screw terminals on typical 5V/12VDC, 24VDC, and 48VDC OEM
power supplies of the sort you can find on Amazon and eBay.  These
supplies almost all come with a set of screw terminals for 120VAC
line/neutral/ground inputs, and +/- DC outputs.  The power distribution
board has matching terminals for each type of power supply, so that
you can easily wire each supply to the distro board, keeping the
wiring tidy.  The 120VAC connections to the OEM supplies are all
switched through the "smart" relay, so that all power supplies
automatically switch on and off with the main PC.

* Secondary PC ATX power supply port: A standard 24-pin motherboard
connector is provided for a secondary PC ATX power supply, which can
be used **instead of** a standalone 5V/12V OEM power supply.  Many pin
cab builders use PC ATX power supplies as their 5V/12V supplies simply
because they're so widely available, and they're often cheaper than
standalone SMPS units with similar amperage capacity.  This board
makes it extremely easy to use a PC ATX supply, since it has a
standard motherboard connector, **and** it automatically controls the
"soft" power to the supply through the smart power switching feature.
Plug your secondary ATX power supply's AC cord directly into a regular
outlet, and plug the main 24-pin connector into the power distribution
board's mating 24-pin header.  The smart power switching will then
automatically turn the secondary power supply on and off according to
the main PC power supply status.

* Power hub: The board includes an ample set of output ports for 5V,
6.3V, 12V, 24V, 48V, and GND connections, for running wiring to the
various feeedback devices.  This is a convenience to help tidy up your
wiring by providing enough ports that you won't have to improvise by
tying multiple wires together.

* Indicator LEDs: all of the output DC power rails have indicator LEDs
that light when power to that voltage level is on.  This is helpful
for safety (since you can tell at a glance that power is on) as well
as for troubleshooting.


## Operation Mode Options

When you build the board, you can select one of two operational modes:

* Smart power switching, based on the main PC 5V power supply status

* Always-On mode

### Smart power switching

In this mode, the 120V power sent to all of the power supplies is
switched based on the status of the 5V power rail from the main PC,
via the MAIN PC POWER header connection.  This lets the main PC control
power to all of the secondary power supplies, so that all of the other
devices automatically power on and off along with the main PC.

You MUST plug a floppy disk power connector from your main PC into the
MAIN PC POWER header when operating in this mode, so that the board
can tell when the main PC is powered on.

<b>DO NOT</b> install anything in the slots marked **W1** and **W2**.
Those are only used in ALWAYS-ON mode.


### Always-On

If ALWAYS-ON mode is selected when you build the board, all of the power
supplies are connected to 120VAC mains power ALWAYS.  There is no "smart"
switching based on the main PC power supply status.  Use this mode if
you **already** have your own smart power switching mechanism, such as
a smart power strip that controls its outlets based on whether the main
PC is on or off.  You can also use ALWAYS-ON mode if you use a hard On/Off
switch to controls the main 120V power to the entire system.

To build the board in ALWAYS-ON mode, **DO NOT POPULATE** the following parts:

* K1 (Relay)
* OK1 (Optocoupler)
* R4 (resistor)

Instead, **install jumper wires** at the slots marked **W1** and **W2**.
(A jumper wire is simply a short piece of wire inserted into the pads
as marked - effectively a zero-ohm resistor.)

In this mode, there's no need to plug anything into the MAIN PC POWER header.


## Connectors

This board uses three connector types:

* <b>Molex MicroFit+: </b> These are used for most of the wiring to
the external power supplies.  I chose these because they can handle
high currents, the pins are all enclosed in plastic (which protects
against accidental short circuits if you drop a screwdriver or
anything like that), and each connector is designed so that you can't
plug in the wrong cable, and you can't plug in the right cable in the
wrong direction.  This makes maintenance work easier, since you don't
have to remember which cable goes where - each cable only fits in one
header.
 
  These connectors use crimp-pin housings, which makes them a little bit
  of work to assemble, so I only used them for the connections to the
  power supplies.

* <b>TE Connectivity MTA-156:</b> These are used for the power
distribution side, to carry power and ground to your devices and other
circuit boards.  These are large connectors designed for high
currents, and they have "ramps" that prevent plugging in the connector
in the wrong direction.  I used different pin counts for each header
of this type, which should make it fairly obvious which plug goes with
which header.

  I chose these because the wire housings use IDC termination, which is
  faster and easier to assemble than the main alternative, crimp pins.
  The only thing to watch out for is that each housing type only works
  with one wire gauge, so you have to be sure to use the right wire type
  with each plug.

* <b>Mate-N-Lok:</b> This is the special connector that mates with the
standard floppy disk power connectors found on virtually all ATX power
supplies.  This is used for the connection to the primary PC power supply.
It makes it extremely easy to connect the PC power supply, since you just
need to plug in a free floppy disk connector.

* <b>Molex Mini-Fit Jr:</b> This is another PC-compatible connector,
in this case, the ATX 24-pin motherboard connector.  This is provided
in case you want to use an off-the-shelf ATX power supply as your 5V
and 12V source.  If so, you can just plug its motherboard power cable
directly into the 24-pin header on the power distribution board. (If
your power supply comes with a 20-pin connector, that's also
compatible with this header.)  You can omit this header if you're not
using an ATX supply for your 5V/12V source.


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


## Coin door interlock

The coin door interlock circuit cuts off power to the 48VDC output
pins whenever the two pins on the **COIN DOOR INTERLOCK** header are
not shorted together.  You should connect these two pins to the
NORMALLY OPEN and COMMON terminals (respectively) of your coin door
switch.  The switch should then be positioned behind the coin door so
that the coin door CLOSES the switch when the door is closed.  This is
the standard setup that real pinball machines have used for many
years, so it's easy to replicate in a virtual pin cab if you're using
standard parts.  See [Coin Door](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php?sid=coinDoor)
in the [Pinscape Build Guide](http://mjrnet.org/pinscape/BuildGuideV2/BuildGuide.php)
for the recommended parts and physical installation details.

If you're not using a 48V power supply, there's no need to connect
any of this.

If don't wish to use the interlock feature, you can simply create a
dummy connector for the COIN DOOR INTERLOCK header that shorts the two
pins together.  I don't recommend doing this, since it bypasses what I
consider an important safety feature, but it might be necessary if
your cabinet is constructed in such a way that there's no good way to
install a switch that detects when the cabinet is opened up for
service.  If you omit the safety interlock, you should always
completely power down your system manually before working inside the
cab.

Note that the coin door interlock **only** cuts power to the 48V rail.
It **doesn't** cut power to 5V, 6.3V, 12V, or 24V, or to the Pinscape
Pico boards.  This is because the interlock is designed primarily for **human**
safety, and the lower voltages are less likely to cause harm to a human.
However, be aware that ALL of the voltages are nonetheless quite hazardous
to the **equipment** in the pin cab, and all of them are quite capable
of starting fires.  Always completely power off the system if you're
going to be moving wires around or doing anything where you might
cause a short circuit.  It's all too easy to destroy expensive
equipment with a misplaced screwdriver when the power is hot.  People
are posting to the forums all the time about boards they destroyed
by accidentally touching a live 12V wire to the wrong terminal.



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
