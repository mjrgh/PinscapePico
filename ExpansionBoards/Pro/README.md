# Prototype Raspberry Pi Pico-Based Pinscape Expansion Board - JLCPCB Edition

This is a set of EAGLE plans for a prototype second generation
Pinscape Expansion Board, based on the Raspberry Pi Pico
microcontroller in place of the FRDM-KL25Z used in the original
Pinscape boards.

The main motivation for this project is that the original Pinscape
boards require two components that have become extremely difficult to
source: the KL25Z microcontroller, and the TLC5940NT PWM controller
chips.  It's impossible to build the boards without those parts, and
both have been scarce for several years.  TI officially declared the
TLC5940NT to be obsolete, so those won't ever be coming back.  NXP
still lists the KL25Z as an active part, so it's possible they'll do
another production run someday, but it's not clear if or when that
will happen.

The new boards are based on the Raspberry Pi Pico.  The Pico is
similar to the KL25Z, but uses newer tech with some nice incremental
improvements (e.g., a faster CPU and a lot more RAM), but the biggest
advantage is that the Pico is widely and amply available.  And it
seems likely to remain widely and amply available, because it's become
hugely popular, and it's made by a company whose whole mission is
making hobbyist-friendly devices like this.

The Pico does have a couple of disadvantages vs the KL25Z, the biggest
being that it lacks an accelerometer (the KL25Z features an excellent
built-in accelerometer).  The Pico also has a much smaller complement
of GPIO ports (the externally exposed pins that allow the CPU to
connect to peripheral devices like lights, buttons, and
accelerometers).  These deficiencies make the Pico unsuitable for use
as a standalone pin cab button-and-nudge device, which is one of the
neat little tricks the KL25Z can do.  However, it's a great starting
point for an expansion board system, since we can use the boards to
add peripheral devices for all of the features the Pico lacks
natively.


## Factory assembly edition

This edition of the board is designed for factory assembly, rather
than soldering the parts by hand.  I started off designing the board
for hand-assembly, since that's more consistent with the DIY spirit of
the project, but I decided to investigate factory assembly for two
reasons.  The first is that there are simply no good options for the
accelerometer chip that *aren't* in DIY-hostile packages - every
option I could find was in a QFN package or something worse.  So no
matter what I chose for all of the other parts, I couldn't avoid
having that one part that would pose a big challenge for everyone
trying to build the board by hand.  I figured that this would vastly
reduce the number of people who would be willing to even attempt the
project, and create a lot of frustration for a lot of people who did
attempt it.  The second reason that I decided to look at factory
assembly is that this board simply has a lot of parts.  I tried my
best to minimize the part count by carefully choosing parts that could
do most of the work with minimal supporting components, but I still
ended up with over 120 components.  That's a lot of work just sourcing
the parts, let alone soldering them.  So given the amount and of labor
and degree of difficulty that would be involved in building the board
by hand, I figured it was worth exploring if factory assembly would be
economically feasible for DIYers.

The main difference between a "hand assembly" version of the board and
a "factory assembly" version is that the factory version is free to
use components that are difficult to solder by hand.  In particular,
the factory version uses parts with smaller physical form factors
(e.g., 0402 capacitors) and ICs with fine-pitched pins and pins hidden
under the package (e.g., QFN).  These parts are extremely difficult to
handle and solder manually, so I would very much try to avoid them in
a design intended for hand-assembly.  But if we're going to assume
that hand-assembly isn't a priority, then there are some nice benefits
to using the smaller parts.  The big "customer" benefit is that we can
pack more features into the available space, but the smaller parts
also have some engineering advantages (e.g., smaller bypass capacitors
can be placed closer to the power pins they're supplying, and circuit
trace routing can be improved because the obstacles to route around -
the SMD pads - are smaller), as well as price advantages (e.g., the
QFN version of TLC59116F is for some reason about half the price of
the SOIC version, which is actually a significant difference as this
is a fairly expensive chip - as of this writing, using QFN instead of
SOIC takes about $12 off the total component cost of each board).

Even though I'm targeting the board for factory assembly, I'm
determined to keep it a hobbyist project, not a commercial venture.
In particular, I don't want anyone to have to contract with Foxconn to
make 10,000 copies of the board and store them in a warehouse
somewhere.  I want individual pin cab builders to be able to build
boards on demand, with a small minimum order size, just like the
original Pinscape boards.  Fortunately, this is actually possible
these days!  There are a few online vendors that specialize in PCB
assembly for hobbyists.  That means they have small minimum order
sizes at reasonable prices.  I haven't found any vendors who will
assembly just one copy of a PCB, but there are several who have
minimum order sizes of only five to ten copies.  That's still more
than most of us hobbyists need, since most of us hand-craft a single
pin cab at a time and thus will only need one control board for it.
But it's a small enough minimum that it's at least plausible that most
of us can make something work with group orders.

I specifically selected one PCB assembly vendor as the target vendor
for the project, so that I could test that the design was actually
manufacturable by at least one company and so that I could get
concrete price information.  I also tailored the component selections
according the vendor's catalog, to help improve the chances that the
parts on the board would be available.  The vendor I chose is JLCPCB.
They offer small minimum order sizes (5 copies of the board) at
reasonable prices, and they have a highly automated order entry system
that requires very little manual data entry to place an order.  You
absolutely do not have to choose this same vendor if you want to build
the project!  The main point of targeting one vendor was to concretely
test that the board was feasible to manufacture.  And if it's feasible
at one vendor, it should also be feasible at most other vendors, since
all of the PCB fabricators have to maintain similar capabilities to
remain competitive.


## Status

This design is currently a prototype, on paper only.  I haven't
built a physical version of the whole board, although I've 
physically build and tested all of the main sub subsystems to
verify that the electronic designs are sound.

All of the devices on this board are supported by the Pinscape Pico
software, and the Config Tool comes with a template configuration
file with all of the necessary settings.


## Design Notes

Some notes on choices I made in the design.

### Component selection

My original goal with the board was hand-assembly, so I started off
trying to use components that are large enough to manipulate by hand
(using tweezers, certainly, but still "by hand" as opposed to "by
robot") and that use solder-pin layouts that are feasible to assemble
with solder paste and a heat gun.  For the passive components like
resistors and capacitors, I instituted a minimum size of "1206" (which
is an electronics industry jargon term meaning 0.12" by 0.6").  I've
found that 1206 is the smallest SMD package that I can comfortably
manipulate with tweezers; it's actually quite large by modern industry
standards, but everything smaller is just too hard to work with, at
least for me.  Similarly, I tried to avoid hidden-pin IC packages like
QFN, and tried to avoid super-fine-pitch ICs below 0.5mm pitch.  Even
0.5mm pitch is tricky to solder, but it's doable with some practice.

This edition of the board doesn't stick to those limits, though.  For
this edition, I specifically targeted factory assembly, which opened
the design to smaller passive parts and other IC packages.  There are
a number of 0402 and 0603 passives, and some QFN ICs.  This allowed
improving the layout, and it also reduces the component cost of the
board, since some of the QFN parts are (for whatever reason)
significantly less costly than their SOIC versions.


### Single-sided component placement

I intentionally placed all of the components on one side of the board.
With SMD components, it's common to place components on both sides of
the board to increase density, but I avoided this because it makes
hand-assembly more difficult and typically increases the cost of
factory assembly.


### Connectors

Most of the connectors are from the TE Connectivity MTA series.  These
connectors have polarized headers, ensuring that the connector is
always inserted in the correct orientation, and the wire housings use
IDC termination with individual wires (not ribbon cables).  I wanted
to give this a try because IDC connectors are a lot quicker to
assemble then crimp-pin connectors, and they're much more reliable
than screw terminals.  I think most DIYers tend to favor screw
terminals because they don't require any special tooling or expertise
to use, but the trade-off is that they're unreliable, at least in my
experience.  I want the wiring inside my pin cab to be very robust; I
don't have to constantly troubleshoot loose connections.  I think IDC
connectors provide the best combination of reliability and quick
assembly.  However, it should be possible to replace the MTA parts
with screw terminals or crimp-pin parts.  There are Molex crimp-pin
products with the same header pin pitch as the corresponding MTA
headers.

For the plunger connector and the RGB flasher output connector, I kept
the same headers, with the same pin layouts, used in the original
Pinscape expansion boards.  This allows anyone upgrading an original
Pinscape system to just plug their existing plunger and flasher cables
into the new boards.  The one change I made vs the original boards is
that I used "shrouded" headers on this board (the original boards had
plain headers without shrouds).  The shrouded header provides a
polarizing key to ensure that the connector is inserted in the correct
orientation.

I also used shrouded 0.1" pitch, two-row pin headers for the button
inputs.  These are best with mating crimp-pin wire housings; I don't
think there are any individual-wire IDC housings available in a
two-row configuration like this, and ribbon cables aren't appropriate
here, so that leaves the crimp-pin headers.  There just isn't room for
32 buttons worth of single-row headers or screw terminals.  If you
really want screw terminals, it would be fairly easy to design a
daughter-board that would plug into the pair of 16-pin button headers,
and route the pins out to screw terminals.

The secondary power supply input is a connector that mates with the
standard floppy-disk connector on an ATX power supply.  This lets you
just plug one of the unused floppy-disk power cables from your
secondary power supply directly into the board, without having to
build the adapter cabling that was necessary with the original
expansion boards.  Hopefully that's a more convenient setup for most
people.

### Button inputs

Button inputs on the original Pinscape boards were wired directly to
GPIO ports, so each button input required one GPIO port.  That was
workable on the KL25Z, which has about 50 exposed GPIO ports, but not
on the Pico, which only exposes 26 ports.  If we could use all of the
ports for button inputs, we'd have just barely enough for the standard
pin cab complement, but that's not possible because the board has to
reserve about 16 ports for its own purposes.  That leaves only 8 ports
that could be assigned as button inputs, which clearly is too few.  We
therefore need additional hardware for the button connections.

The new board uses what's known as a GPIO expander chip for this
purpose.  GPIO expanders are chips that provide additional GPIO-like
ports that a microcontroller can access through a small number of its
own ports using a serial protocol like I2C or SPI.  The chip I chose,
PCA9555, uses I2C, which only requires two ports on the RP2040.  Up to
8 of these chips can be combined onto a single I2C bus, so these chips
can be used to access 128 ports using only two RP2040 ports.  The new
board only uses two of the chips, since 32 button inputs is ample for
a virtual pin cab.  And since the board already has an I2C bus that it
uses to access other peripheral chips (the accelerometer and PWM
controllers), we effectively get all of these added button input ports
"for free".

Note that the canonical approach that most similar boards use to solve
the same problem is to use a parallel-to-serial shift register chip,
typically 74HC165.  I chose the GPIO expander instead because it
reduces the part count and consumes fewer RP2040 ports.  The selected
GPIO expander has 16 ports per chip, so we only need two of these
chips to provide 32 button inputs (the original Pinscape board only
had 24).  Furthermore, the GPIO expander can be configured to provide
its own internal pull-up resistors on the input ports, so we don't
need to add a discrete resistor per port as we would with a simple
shifter chip.  I2C only consumes two GPIO ports on the RP2040, and
since we'd need an I2C bus anyway for the other peripherals, adding
the PCA9555 to the mix effectively consumes zero additional ports.
(It actually does use one additional port in the implementation, for
an interrupt signal from the PCA9555; this isn't strictly necessary,
as the software could just poll the chips, but it gives the software
more flexibility.)  The shift-register chips typically require three
additional ports, which can't be shared on a general-purpose bus like
I2C.


### ADC(Analog-to-Digital Converter)

The board has a dedicated ADC chip for quantizing analog voltage input
from a plunger device.  The Pico has its own on-board ADC, but the
Pico's ADC isn't a very good one; it has low resolution and some known
deficiencies.  The most popular plunger sensor type right now is
probably the linear potentiometer, and those benefit greatly from
high-resolution quantization, so I thought it was worth including a
good ADC in the design.



### Unused GPIOs

The board design leaves several RP2040 GPIO ports unassigned.  I
included a header, labeled "Expansion", that provides external access
to all of the unused GPIOs and unused input ports on the ADC.  This
can be used for future add-on boards or for custom user-added
features.

The expansion port also provides access to the Pico's 3.3V power rail,
so that peripherals added via the expansion port get their power from
the same source as the Pico.  Peripherals should usually be powered
from the same rail as the MCU to avoid damage to either device, since
the data pins on both devices might be intolerant of being driven to
voltages above the respective device's current Vcc level.

This is probably an unnecessary complication, in that I don't think
anyone ever used the similar port that was provided on the original
Pinscape boards.  But it was fairly easy to include in the design, and
it only adds one fairly inexpensive part (a 2x8 0.1" pin header for
the port) to the build cost.


### PWM chips and MOSFET gate drivers

The PWM controllers on this board are TI's TLC59116F, which are
16-channel I2C chips with open-drain outputs that can sink 120mA per
channel.  Note that the "F" suffix is critical here!  TI also sells a
very similar part without the "F" suffix that has constant-current
outputs.  This board requires the open-drain version.

The TLC59116F replaces the TLC5940NT used in the original Pinscape
boards.  This chip has a couple of advantages.  One is that it has a
standard I2C bus interface for the microcontroller connection, which
only requires two GPIO ports on the Pico and is easier to work with
(from the software's perspective) than the older chip's ad hoc serial
interface.  The other is that it has a much higher PWM frequency than
the older chip, at 97kHz.  The high frequency is useful because it's
well above the human hearing range, which makes the PWM signal
inaudible if a PWM-controlled device transduces the PWM signal to
mechanical vibration.  Some devices, particular motors and large
solenoids, tend to vibrate at the PWM frequency when controlled with
PWM.  If the PWM frequency is in the human hearing range (40Hz to at
most 20kHz, depending on the individual), the vibration manifests as
audible noise.  It can be difficult or impossible to get rid of the
vibration with mechanical treatments, so the only workable solution in
many cases is to increase the PWM frequency above 20kHz, so that the
noise is too high-pitched for humans to hear.  The older TLC5940NT
chips couldn't run at high enough frequencies to get out of the
audible range, but the TLC59116F's native 97kHz frequency is safely
out of human range.  It's probably even high enough that it won't
annoy your dog.

The MOSFET outputs are connected to the PWM chips through TI's
UCC27523 gate driver chips.  The UCC27523 is an inverting gate driver
with an internal pull-up, which means that an unconnected or floating
input is pulled high (by the internal resistor) and places the MOSFET
in an OFF state, whereas an input line that's pulled to ground places
the MOSFET in an ON state.  This mates perfectly with the TLC59116F's
open-drain outputs, which are floating when the PWM signal is OFF and
pulled to ground when the PWM signal is ON.  On the MOSFET side, the
UCC27523 provides a high-current push-pull input to the MOSFET gate,
which rapidly charges or discharges the gate at each state change.
The chips are capable of driving the gate at up to 1A, which lets them
charge and discharge high-gate-capacitance MOSFETs at high switching
speeds.  The board uses 12V for the gate drive power supply, which is
compatible with most power MOSFETs in the voltage/current range we
need.  This allows for a very wide range of MOSFETs to be used, since
we don't have to be at all picky about Vgs(th) or gate capacitance.
Board builders can choose MOSFETs based largely on price; just filter
by your minimum Vds and Id needs.

The gate driver chips have the additional benefit that they're
designed for glitch-free startup, which should prevent the annoying 
random solenoid firing that sometimes happened with the original
Pinscape boards.

The original Pinscape boards used optocouplers to serve the same
function of inverting the PWM signal to drive the MOSFET gates.  The
UCC27523 chips are a far superior solution, for several reasons.  The
first is that most optocouplers can't switch fast enough to keep up
with the TLC59116F's 97kHz PWM signal, so intensity adjustments
wouldn't pass through reliably to attached devices.  The second is
that optocouplers can't deliver enough current to the MOSFET gate for
efficient switching, which means the MOSFET can heat excessively
because it's spending too much time in a high-resistance state.  Many
larger power MOSFETs have a relatively high gate capacitance that
requires high current on the gate drive to achieve fast enough
switching to avoid excess heating.  The gate driver chips are
specifically designed to deliver high current to the gate for cool
operation.  A bonus feature is that the gate driver chips take up less
space on the board than most optocouplers, which made it possible to
pack more MOSFET output ports onto the board.


### Glitch-free and lockup-free output design

The design has several elements meant to guarantee that the output
ports are free of startup and shutdown glitches (meaning that all of
the output ports consistently OFF when the board is powered on, and
turn off again immediately while the board is being powered down).  I
also tried to ensure that ports designated as solenoid ports can never
lock on due to a software fault, on either the PC or in the Pico
firmware.

Glitch-free startup and shutdown are the hardware's responsibility.
The whole issue here is that there's a brief period at each end of a
power cycle where the software isn't running, because the MCU is
deliberately not executing any software instructions while the voltage
supply is rising or falling.  During these periods, the software can't
send commands to the output controller chips to tell them to disable
the outputs, so the hardware has to be designed in such a way that
everything is deterministically in an OFF state at power-on and again
whenever the MCU is in a reset state.

The TLC59116 chips provide guaranteed glitch-free startup and shutdown
at their output ports (they use an internal power-on reset circuit
that holds the outputs in the OFF state while the supply voltage is
below a threshold).  The small LEDs are directly connected to the
TLC59116 outputs, so those get glitch-free startup by virtue of the
TLC59116's glitch-free startup.  The Darlington outputs and MOSFET
outputs are all triggered through inverters with pulled-up inputs, so
they'll remain OFF whenever their TLC59116 ports are OFF.

Guaranteeing that ports won't lock up due to a software fault is a
separate matter.  Power-on glitching is annoying but rarely does any
damage; locked-up outputs can actually break things.  Pinball coils
such as knockers, and similar large solenoids, are designed for brief
activation only, and will overheat and fail (the coil wire will act
like a fuse and simply melt) if power is applied continuously for more
than a few seconds at a time.  There are two scenarios in a virtual
pinball setup where software faults can leave ports locked on.  The
first is that the application software on the PC crashes or freezes,
or is simply programmed incorrectly, in such a way that it turns an
output port ON and then fails to turn it back OFF soon enough to avoid
damage to the connected device.  USB disconnects and glitches are in
this same class of failures, in that they could prevent the PC from
sending a required OFF command.  The second failure mode is that the
MCU firmware crashes or freezes, which makes it stop responding to
commands from the PC.  If a port is ON at the moment of a crash, the
crashed MCU firmware won't be able to obey the OFF command from the
PC, even if everything on the PC side is still working properly.

The original Pinscape boards took a very hardware-centric approach to
this problem, by providing hardware timers on certain ports (the
knocker port on the main board and all ports on the Chime board).  The
hardware timers were designed so that the port was actually
"edge-sensitive" rather than "level-sensitive": the port only turned
on at a low-to-high transition from the MCU output, and then only
turned on for a fixed time period.  So if the MCU turned a port ON and
then held it on continuously, the timed port would only stay on for
the hardware-programmed interval, and wouldn't turn on again until a
new low-to-high transition on the MCU port.  This approach inherently
addressed every possible software fault up and down the entire
software stack, because the only thing that mattered was the final
port signal coming out of the MCU.  If the final port signal got stuck
ON, for whatever reason, the port would still turn off thanks to the
hardware timer.

This design doesn't have the hardware timers.  They add a lot of
complexity and extra parts, and I decided we could get the same level
of reliability without them.  The hardware timers also weren't
configurable - a port was either a timer-protected port or it wasn't.
Doing everything in software lets you designate any port for timer
protection, and also allows for variations on the behavior, such as
adjusting the timer interval, and having the option to switch a port
to a low-power mode after the timeout (the "flipper logic" scheme in
the original Pinscape software) instead of cutting power entirely.

The new design has three layers of protection.  These are truly
layers, in that each one is an independent backup for the next.  The
first layer is the Flipper Logic/Chime Logic software control, which
applies the same principle as the original hardware timer, but
implemented in the firmware software instead of in dedicated hardware
on the board.  This is the weakest link in terms of the "guarantee",
since it depends upon the firmware correctly executing the timer
behavior as configured.  My own estimation is that the timer logic
itself is simple enough that it can be made highly reliable IN
ISOLATION - meaning that we can rely on it to function properly AS
LONG AS the firmware OVERALL is still running properly.  The main
risk, in other words, isn't that the timer logic would screw up, but
that a bug in some unrelated part of the software would cause the
whole firmware program to crash or freeze or get stuck doing some
unrelated task, preventing the part of the code that operates the
timer from executing properly or in a timely fashion.

That's where the second layer of protection comes in.  The second
layer is the Pico's "watchdog".  This is a hardware subsystem on the
Pico that runs a hardware countdown timer that reboots the Pico after
a software-configured interval expires, UNLESS the software intervenes
to prevent the reboot.  This is a widely used technique in embedded
systems, and it's highly effective, because it provides a
hardware-level guarantee that the system will reset if the software
doesn't behave "as expected", where the expectation is that the
software must, in essence, tell the watchdog "I'm still working" at
regular intervals.  For this application, the interval in question
simply has to be shorter than the time it would take for lock-up
condition to damage an attached pinball coil.  This is within entirely
reasonable bounds - it takes a couple of seconds of lock-up to burn
out most coils, and the firmware main loop is on the order of
milliseconds.  So setting the watchdog interval to something on the
order of 10 to 100 milliseconds would catch lock-ups well before they
could do any damage.  It's still important to design the software
carefully; in particular, since the most important purpose of the
watchdog by far is to prevent output port lock-up, the watchdog "I'm
still alive" signaling should be closely coupled to the code that
executes the port protection timers.

The third layer of protection is similar to the power-on glitch
protection, but in this case it applies to the MCU RESET condition
rather than power supply transitions.  This layer is necessarily at
the hardware level, since it has to deal with the aftermath of a
software fault that caused the Pico watchdog to intervene by resetting
the Pico.  The goal here is to ensure that all of the output ports are
immediately and deterministically turned OFF when the Pico enters
RESET state, and deterministically stay off until the software
restarts and takes control of the ports again.  We can accomplish this
fairly easily by taking advantage of a hardware feature of the Pico,
and a hardware feature of the TLC59116 chips.  When the Pico is in the
RESET state, it configures all of its GPIO ports as "inputs", which
places them in a high-impedance state (essentially an open-drain
connection).  The input/high-impedance configuration remains in effect
until the software explicitly changes the GPIO to some other mode, so
this is the state from the moment a RESET occurs until the software is
fully in control again.  So let's start by designating one Pico GPIO
as the /OUTPUT_RESET signal, and tying this GPIO to GND through a
pull-down resistor.  The pull-down assures that this signal will be at
GND through a Pico reset, and remain at GND until the software
explicitly programs the port as an output and drives it high.  We can
then tie this /OUTPUT_RESET signal to the /RESET input pin on each of
the TLC59116 chips.  Holding this line low on a TLC59116 forces all of
the chip's output ports to their OFF state, exactly as during the
power-on/power-off supply voltage transitions.  This ensures that the
connected small LED, Darlington, and MOSFET ports are all off, for the
reasons discussed above.  When the CPU completes its RESET cycling, it
will restart the firmware, which will in turn initialize its
connections to the TLC59116 chips.  Part of this initialization will
be to drive the /OUTPUT_RESET line high to allow the TLC59116 chips to
come out of their reset state.  When they do, they're designed to hold
all of the outputs OFF until the host explicitly sets them to another
state, so we have a smooth transition back into normal operation, with
all ports turned OFF and held OFF until the software is properly
running again.


### Darlington outputs and small LED outputs

Like the original Pinscape boards, this board has a mix of MOSFET
outputs, Darlington outputs, and direct PWM-chip outputs.  All of the
ports work the same way from the user's perspective: they're all
low-side switches that serve as the "ground" connection for one device
each.  The only difference between the port types is the amount of
power that each port can handle.  Ideally, every port would be a
MOSFET port, because those are the most general-purpose port, capable
of handling any type of device from low power to high power.  Cab
builders wouldn't have to worry about using special port types per
device because every port would work with any device.  The reason we
*didn't* use all MOSFET ports is that it would take up too much space
on the board.  A MOSFET port takes up about twice the amount of space
of a Darlington port, which in turn takes up perhaps 4X the space of a
small LED port.  The mixture of port types makes it possible to
include enough ports to connect all of the common virtual pinball
devices without making the circuit board gigantic (and prohibitively
expensive).

The theory of operation of the MOSFET ports was described earlier.
The Darlington ports work similarly, but Darlingtons don't need the
gate driver chips, since they trigger on a small current on the
transistor base.  Instead, we use a small pre-biased PNP transistor on
each port to act as a logic inverter to convert the TLC59116F's
open-drain output logic to a positive voltage source on the Darlington
base input.  The Pinscape boards used optocouplers to serve the same
purpose for their Darlington outputs; the PNP inverters in this design
take up less space and switch faster than typical optocouplers, which
(as mentioned in the MOSFET section above) is necessary to handle the
TLC59116F's high PWM frequency.  The pre-biased PNP transistors are
available in "array" packages that have two transistor (and the
associated bias transistors) per package, which further improves board
density and assembly workload.

The direct PWM output ports for small LEDs work almost exactly the
same way as in the original Pinscape board.  These output ports on the
board are simply wired directly to the corresponding TLC59116F ports,
without any booster circuitry, which means they can sink up to the
maximum current that the TLC59116F chip itself can handle, which is
120mA per port (which is ample for virtually any small LED).  The only
difference from the original Pinscape boards is that these chips are
simple open-drain current sinks, whereas the original boards had
constant-current sinks.  On this board, therefore, the external LED
circuit must include a current-limiting resistor.  I think this is an
improvement over the constant-current ports on the original Pinscape
boards, since it's much easier to use with a heterogeneous set of
LEDs.  The constant-current sink on the original boards was convenient
in that you didn't need to include current-limiting resistors, but the
requirement to choose a fixed current level was too inflexible. 

### IR Remote

Like the original Pinscape boards, this board has support for an IR
remote input and output.  There are some slight changes.

First, the output port is designed for one TSAL6400 IRED, with the
driver transistor and current-limiting resistor sized to run the
device at the device's short-pulse current limit.  The original
Pinscape board was only designed to run the device at its continuous
current limit, which made the IR signal a lot weaker than it should
be.  The IRED can be safely operated at the higher short-pulse limit
because the signal is inherently modulated into short pulses.  The
higher current increases the signal strength to allow for longer
transmission distances.

Second, as with the original boards, the board has space to install
the TSOP38438 receiver directly on the board.  But the new board also
has a header for installing the TSOP38438 on a separate satellite
board.  A satellite board installation makes it possible to position
the receiver in an opening in the cabinet so that you can send signals
to it with a remote control from outside the cabinet.  The original
boards only really allowed for an on-board receiver, because my
original conception of the feature was that you'd only need to use the
receiver to "teach" the software commands that you wanted it to be
able to send on the emitter.  But the Pinscape firmware has an
additional use case that allows the firmware to receive and act on IR
commands received, as an additional kind of "button" input; if you
want to use this feature, you'd want to position the receiver to
receive signals from outside the cabinet, so you'd need the off-board
receiver installation.  The addition of the header makes this easier.
The wiring only allows for one receiver, so if you install the
on-board receiver, you can't use an external one, and vice versa.
When building the board, then, **only one** of the the on-board
receiver block **or** the off-board header should be populated.  Note
that the off-board receiver should be built on a small PCB that
includes the capacitor-and-resistor power supply filter recommended in
TSOP38438 the data sheet; those parts are meant to be physically
located close to the receiver, since the whole point is to filter
electrical noise on the power supply wiring coming into the receiver,
so they can't be located on the main board when the off-board receiver
is to be used.

### Power sensing circuit

This board uses a redesigned power-sensing circuit.  It serves the
same function as the power-sensing circuit on the original Pinscape
boards, and has a similar theory of operation, but I revamped the
electronic design using a couple of special-purpose IC chips rather
than the discrete component design of the original boards.  

The original circuit is essentially a flip-flop that resets to a logic
'0' state each time the secondary power supply (PSU2) powers up.  The
microcontroller (KL25Z on the old board, Pico on the new board) reads
the flip-flop state through an optocoupler, which isolates the MCU's
power supply from the secondary supply.  The isolation is critical
because the whole point of the circuit is to let the MCU detect when
power has been cycled on the secondary supply even if the MCU itself
is continuously powered at all times through the USB port, which is
usually the case due to the PC being on a "soft power" control rather
than actually having its AC power cord unplugged between sessions.
The MCU can also attempt to *set* the flip-flop state through a second
optocoupler.  When the flip-reads as logic '0', it means that either
PSU2 is off (so it can't power the optocoupler, causing it to read as
a zero voltage and thus logic '0') *or* that the flip-flop has been
reset (cleared to logic '0') by a power cycle on PSU2 (recall that the
circuit is designed so that the flip-flop is always cleared after a
power cycle).  The MCU can distinguish the two cases by attempting to
set the flip-flop, and then reading back the result; if the flip-flop
reads as logic '1' after the set operation, the flip-flop must have
power, so the fact that it read as '0' previously means that PSU2 has
been turned back on after a period of being off.  This state change is
what triggers the TV-ON timer because it means that the cab has just
been powered up and is in the process of initializing.  If the
flip-flop still reads as '0' after a set operation, it means that PSU2
is currently off, so the MCU should do nothing for now and simply
check back later.

The old design used a couple of transistors to implement the
flip-flop.  The new design uses a flip-flop IC instead.  Since
flip-flop ICs power on in an indeterminate state, we also use a "POR"
(Power-On Reset) IC to force the flip-flop into a logic '0' state when
PSU2 powers on.  The POR chip is a special-purpose chip designed to
assert a RESET signal for a predictable interval after the power
supply stabilizes above a threshold voltage after being below the
threshold (typically meaning that power was off for a time).  The
combination of the flip-flop and POR chip (plus the pair of
optocouplers from the original design) implements exactly the behavior
we need.  The reason for the redesign is that it uses fewer
components, reducing cost and assembly workload, and accomplishes the
design goals more robustly.  The flip-flop and POR chips are designed
for these specific tasks, and are engineered to much higher standards
than my ad hoc design.

### 6.3VDC power supply

An early version of the board included a 6.3V regulator to supply
power for commonly used small pinball lamps (#555, #44, etc), used in
coin doors (for the coin chute lamps) and Suzo-Happ lighted
push-buttons.  This was eliminated in later versions (due to space
constraints).  However, there's a companion Power Distribution Board
that includes the same 6.3V regulator.
