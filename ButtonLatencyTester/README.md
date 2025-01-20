# Button Latency Tester

This Pinscape Pico sub-project is a Windows program program that
measures the latency of button presses on the Pico.  It's not part of
the official distribution, since I only developed it for my own
testing purposes: hence it's not documented, and it only covers the
narrow use cases I wanted to test.  But the source is here in case
anyone wants to attempt running their own tests with it, or wants to
use it as a starting point for similar tooling.

This program has been largely superseded by **Button Latency Tester
II**, which you can find in its own subfolder under the main project
folder.  BLT-II has many improvements on this one, including the
ability to measure latency to any sort of device (Pinscape or
otherwise, Pico or otherwise), and better precision at making the
measurements.  The BLT-II project has its own Pico firmware component
that turns a spare Pico into a testing tool, plus a Windows host
program works in conjunction with a BLT-II Pico to perform the tests.
