# Pinscape Pico - KL25Z adapter boards

These boards let you plug a Pico into your original-edition Pinscape
Expansion Boards for the KL25Z, with the Pico taking the place of the
KL25Z.  If you have a set of KL25Z expansion boards, this lets you
upgrade to the new Pinscape Pico software without changing all of your
other hardware.  It also lets you continue using the KL25Z expansion
boards even if your KL25Z is broken and you can't find a replacement.

The board works by routing pins on the KL25Z-style connectors to the
corresponding pins on the Pico - GPIO ports, power, ground, etc.
It also adds some extra hardware elements to fill in gaps in the
Pico's capabilities:

* A LI3DH accelerometer, to take the place of the KL25Z's built-in accelerometer

* GPIO expanders (PCA9555 or 74HC165, depending on the board version) to make
up the difference between the KL25Z's 50+ GPIO ports and the Pico's 26

* A high-quality 16-bit ADC (ADS1115), to supplement the Pico's low-resolution
on-board ADC (for smoother animation and better precision when using a
potentiometer-based plunger)



## Versions

The 74HC165-based version of the board is the official release.

The older PCA9555 version is also included for reference, but I'm not
planning to maintain it as an official release.  See notes on the
differences below.


## Credits

Thanks to Tengri (https://github.com/jueank) for the conversion to
through-hole sockets for the Pico.  The original design required
soldering the Pico directly to the board, which would make it nearly
impossible to replace the Pico if it ever failed.  Tengri's improved
design lets you use standard 20-position 0.1" socket headers, such as
Wurth 61302011821, that mate with 0.1" pin headers soldered to the
bottom of the Pico.  But you can also still solder the Pico directly
to the board if you prefer.


## Caveats

A few warnings about using the adapter boards:

* The adapter board **only** works with the original Pinscape KL25Z
expansion boards - and I'm talking very specifically about **my original three-board set**,
the one that's documented in the Pinscape Build Guide, with the 10x10cm
Main, Power, and Chime modules.  The adapter **doesn't**
turn the Pico into a general-purpose KL25Z emulator, and it probably
won't work with any third-party Pinscape boards (such as those from Oak
Micros or Arnoz), **unless** they use identical KL25Z pin assignments.
The obstacle to using the adapter with any other expansion boards or
add-on boards is that the adapter is completely inflexible about the KL25Z
pin assignments.  You can only use the adapter with an expansion board
that has exactly the same pin assignments as my original three-board set.
With a **real** KL25Z, it's possible to reconfigure the GPIO ports in
many different ways, which allowed people to design different expansion
board layouts.  The adapter board doesn't have that flexibility, so it
only works in the unique expansion board environment it was designed for.

* If you don't already have a set of the Pinscape KL25Z boards, you
should look at the new Pinscape Pico expansion boards instead.  Those
are designed from the ground up around the Pico, and they have some
improvements over the original KL25Z boards.

* The adapter **doesn't** make the Pico capable of running KL25Z
software.  You have to switch all of your software to the new Pinscape
Pico system.

* Pinscape Pico doesn't work with the Pinscape-for-KL25Z Config Tool,
so you'll also have to switch to the new Pinscape Pico version on the
Windows side.  You'll also have to tweak your DOF configuration to let
DOF know you're using the new system, and make sure you have the
latest DOF (which has support for Pinscape Pico built in), but
otherwise you shouldn't have to change anything in DOF or most other
client software.

* **DIYers beware!** These boards are designed with small-pitch SMD
(surface-mount) parts, which are difficult to solder by hand.  They're
really meant to be assembled by robots.  I didn't really have any
choice with this board because of the constraint that it has to fit
into the same physical space as an actual KL25Z, so that it can fit
into the space allowed in the expansion boards.  The equivalent
easy-to-solder "through-hole" parts are just too big to fit into such
a small space.  One thing you might look into is "PCBA" (PCB Assembly)
services, which are companies that both fabricate the circuit boards
and solder the parts for you.  This is more costly than doing it
yourself, and the ordering process can be intimidatingly complex,
but it might end up being easier than trying to do the soldering
by hand (and possibly cheaper, if you take into account the cost
of extra parts you might need to order for practice or re-dos.)


## Pinscape Pico Configuration

The project folder includes a JSON file for each adapter board layout
with the basic Pinscape Pico configuration for that board.  The two
boards require individual JSON configurations because of the different
hardware.  Import the JSON file matching your chosen board design into
the Config Editor in the Config Tool.

You'll have to do some additional hand-editing on the file because of
the modular design of the KL25Z expansion boards.  The KL25Z expansion
board system allows you to add zero or more "Power" and "Chime" boards
to the basic setup, so you have to set up the Pico configuration to
match the number of boards of each kind you have.  The JSON file has
comments explaining which sections will need your attention, so browse
through the file and follow the instructions you find.

In addition to customizing the configuration for your expansion board
hardware, you'll also want to go through the `button:` section to set
up your preferred button mappings, and arrange the `output:` section
so that the ports are arranged in the same order as your KL25Z setup.
Both systems let you assign DOF port numbers to the physical output
ports in any order.  It will make it easier to move your DOF configuration
to the new system if you replicate the same output port numbering that
you used in the KL25Z setup.

## DOF Setup

Even though Pinscape Pico shares a "brand name" with the original
KL25Z Pinscape, they're completely different systems.  That includes

at the DOF level - DOF thinks about them as unrelated boards, requiring
different communications protocols.  So you'll have to update your
DOF Config Tool settings to select the new boards.

Otherwise, the DOF setup should be the same as with the KL25Z boards.
Both systems let you map your physical output ports to any arrangement
of DOF port numbers, so the first thing you should probably do in the
Pinscape Pico Config Tool is go through the `outputs:` section and
make sure that the ports are all listed in the same order as in your
KL25Z setup.

## 74HC165 vs PCA9555 versions

The repository has two versions of the board: one based on the 74HC165
shift register chip, and one based on the PCA9555 I2C GPIO expander chip.

<b>The 74HC165 board is the official version.</b>  I don't plan to
maintain the PCA9555 version, but it's included for reference.

The difference between the two boards is the choice of chip used to
physically connect the button inputs to the Pico.  The older version
was based on the PCA9555 GPIO expander, which is designed to add
digital in/out ports equivalent to a microcontroller's native GPIO
ports, for applications such as this one where the microcontroller
doesn't have enough native ports to meet the application's
requirements.  In this case, we have a lot more button inputs than
the Pico has GPIO ports.  The newer version of the board accomplishes
the same thing using 74HC165 shift register chips.

The main advantage of the PCA9555 over the 74HC165 is that the former
has built-in pull-up resistors.  Each button port requires a pull-up
resistor so that the port reads a deterministic logic level when the
button **isn't** being pressed (so the button switch is open).  The
74HC165 doesn't have its own built-in pull-ups, so a board based on
this chip requires additional parts (one resistor per port).  That
increases the parts cost and assembly work to build the board, so I
started with the PCA9555 for the sake of economy.

But the PCA9555 has a major disadvantage, which is that it's
relatively slow: it can only sample the button inputs about once per
millisecond.  For most applications, that would be fine, but it's a
negative for a gaming device like this one, where users desire
extremely low latency for control inputs.  The 74HC165 solves this; it
can clock samples into the host microcontroller at megahertz speeds,
allowing buttons to be sampled every 10 to 20 microseconds.  That's
effectively zero latency.

I'm keeping the PCA9555 board in the repository just in case anyone
deems the reduced parts cost more important than the reduced latency
of the 74HC165 version.  However, I haven't brought it up to date
with the changes for the through-hole pin sockets for the Pico, and
I don't plan to maintain it, since I think the 74HC165 version is
the better choice for most people.


