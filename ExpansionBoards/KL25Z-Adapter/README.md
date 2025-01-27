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


## Versions

The repository has two versions of the board, both of which are
"official" - the difference isn't newer vs older, but rather how they
implement the button input hardware.  The Pico doesn't have enough
GPIO ports to cover all of the button inputs in the original KL25Z
boards, so the adapter requires some extra hardware to read more
buttons than the Pico has GPIO ports.  The two board versions use
different hardware designs to accomplish this:

* <b>PCA9555 version:</b> The first version of the board I designed
uses a GPIO expander chip called PCA9555 (actually, two of these
chips) for the button inputs.  In addition, four special button ports
(the ones labeled #1, #2, #3, and #4) are connected directly to Pico
GPIO ports.  The reason for the four special GPIO-direct ports is that
the Pico can scan them almost instantaneously, whereas the PCA9555
chips are relatively slow at communicating with the Pico, allowing
scans only about once per millisecond.  The 1ms scan cycle adds a
small amount of latency reading the PCA9555 ports.  So I reserved a
small set of GPIO-direct ports for super-low-latency button inputs.
In a pin cab, which is how these boards are intended to be used, you
should use the special fast input ports for the flipper buttons, since
those are so crucial for game play.  It might seem terribly limiting
to only have four of these fancy high-speed inputs, but you really
only need them for the flippers; none of the other buttons in a pin
cab particularly benefit from super low latency input.

* <b>74HC165 version:</b> This version uses a shift register chip,
74HC165, for all of the button inputs, replacing the PCA9555 in the
original design.  (This board still has one PCA9555 as well, but it's
not used for button inputs; it's just for internal functions.)  The
reason for making this change is that the Pico can scan the shift
register ports almost as fast as its own native GPIO ports, allowing a
full scan every 10 to 20 microseconds.  This makes *all* of the ports
essentially zero-latency, whereas the PCA9555 version of the board
only has the four dedicated GPIO ports for high-speed button input.
This makes it a little easier to wire the cabinet buttons, since you
don't have to worry about which ports have the special high-speed
feature (they all do!).

My original plan was to make the 74HC165 version the final, official
version of the board, replacing the PCA9555 version.  However, I
changed my mind after finishing the 74HC165 design, and decided to
keep both versions in the project.  The reason to keep both is
that each has some advantages.  The 74HC165 version is technically
better, I think, because of the uniform high-speed button ports.  The
trade-off is that it has more parts than the PCA9555 version, so it'll
be more expensive and require more assembly work.  I'm not sure it'll
be worth the extra cost to most people to have the high-speed
capability on all of the ports, when you really only need it on enough
ports to cover the flipper buttons, which the PCA9555 version of the
board accomplishes via its small set of GPIO-direct ports.

## Pinscape Configuration

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

