# Pinscape Pico Expansion Boards

This folder has EAGLE plans for several circuit board designs that go with
the Pinscape Pico software.  These plans are all open-source designs that
anyone is free to build.

* <b>DIY-Friendly:</b> A comprehensive I/O controller for a virtual pinball
cabinet, with enough button ports and feedback device ports to connect just
about everything in a high-end build.  It uses entirely through-hole parts
that are easy to solder by hand, which is what makes it DIY-friendly.  The
project consists of two boards that are meant to be built and deployed as
a set (since I couldn't fit everything onto a single board, given the amount
of space that the through-hole parts take up).

* <b>Pro:</b> Another comprehensive expansion board, with essentially the
same functionality as the DIY-Friendly board set, but this time using mostly
surface-mount (SMD) parts.  The SMD parts are much smaller than the through-hole
parts used on the DIY board, allowing everything to be packed into a single
board.  The tradeoff is that a lot of the SMD parts used in the design are
difficult to solder by hand, precisely because they're so small.  So this
board is really meant to be assembled by factory equipment.  That's why I
refer to it as the "Pro" board - it's probably only going to be cost-effective
to build if you're going to build at least 50 or 100 units, and if you're
making that many, it's probably because you plan to sell them at a profit.
The designs are open-source, so you're free to do that if you have a retail
shop set up and you're willing to back your sales with warranty and technical
support (since, as they're open source, the designs themselves don't come
with any warranty).

* <b>IR-RX:</b> A small satellite board for the DIY-Friendly or Pro boards
that holds an IR receiver chip, and connects back to the expansion board
with a ribbon cable.  This lets you position an IR receiver anywhere
inside or outside your cabinet, so that it can be in line-of-sight to the
remote controls you want to point at it.

* <b>KL25Z Adapter:</b> This is for people with an existing set of the
original Pinscape KL25Z expansion boards.  The board plugs into the KL25Z
expansion boards where the KL25Z would normally go, letting a Pico take the
place of the KL25Z.  This lets you use your existing expansion boards with
the new Pinscape Pico software, and without having to re-wire your cabinet
for a new expansion board set.  (It doesn't act as a true general-purpose
KL25Z emulator, though, since it's very specifically wired for the original
Pinscape boards.)

* <b>Power Distribution:</b> A simple board designed to help tidy up
your virtual pin cab power supply wiring.  It doesn't do much other
than route power from one connector to another, but the idea is that
it makes your wiring simpler and more structured.  It does have a
couple of simple active features: it provides main power switching for
all of the secondary supplies based on the main PC power on/off status;
and it features wiring for a coin door safety interlock switch for a high-voltage
(48V) supply, so that the high-voltage power is automatically cut off whenever
you open the coin door to do maintenance work inside the cabinet.  It also
features connectors that match the power connectors on the DIY-Friendly
and Pro expansion boards, again just to make the wiring tidier.
