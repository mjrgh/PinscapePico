# Pinscape Pico Windows API

This is a Windows static-link library (.lib) for accessing Pinscape
Pico devices from a Windows application.

The API is organized into two main divisions, which correspond to
the two custom USB interfaces that Pinscape Pico devices expose:

* PinscapeVendorInterface.h defines the API for the device's
  USB Vendor Interface, which provides configuration and control
  functions.  This interface is primarily for use by programs
  like the Config Tool that need to access the device's settings,
  status, and diagnostics.

* FeedbackControllerInterface.h defines the API for the feedback 
  controller interface, which is designed for use by pinball
  simulators and other applications that generate feedback
  effects by operating attached lighting devices, motors,
  solenoids, etc.

Most applications will use one or the other of those interfaces,
according to what they're trying to accomplish.  A tool that performs
configuration functions will generally need the vendor interface, and
simulation software will usually find that the feedback controller
interface has the facilities it needs.

## Documentation and examples

The API documentation, such as it is, is contained in the main
header files listed above.

The StatusLED program (in the StatusLED/ folder off the top-level
project folder) provides a simple example of using the vendor
interface API.

The Config Tool (in the GUIConfigTool/ folder off the top-level
project folder) isn't written as a simplified example, but it
exercises just about all of the vendor interface and feedback
controller API features, so it might at least be useful as a
detailed reference that you can search through for real-world
usage examples of particular API calls.

My fork of the LWCloneU2 project at [https://github.com/mjrgh/lwcloneu2](https://github.com/mjrgh/lwcloneu2)
provides another real-world example of using the feedback controller
API.  See in particular [win32/driver/src/ledwiz.cpp](https://github.com/mjrgh/lwcloneu2/blob/master/win32/driver/src/ledwiz.cpp).


## Underlying USB protocols

The C++ API is a wrapper for the Pinscape Pico's custom USB protocols,
which are documented in the USBProtocol/ folder (under the top-level
project folder).  Everything the C++ API does can also be accomplished
with direct USB operations.  The C++ API just makes things a lot easier
by managing the low-level USB details.


## How to include in a project

The API library is designed to be linked into a C++ project as a
static-link library (.lib).  A Microsoft Visual Studio project
file (.vcxproj) is provided for building the static library, which
you can then link into your project.

You'll need the library's header files as well as the .lib, so my
recommendation is to include a copy of the whole library project in
your Visual Studio "Solution".  That lets Visual Studio build the
library from source as an automatic step in your overall project
build.  To do this, you can simply make a snapshot of the whole
Pinscape Pico source tree in a subfolder within your project's source
tree.  Or, if you want to minimize the number of files in your
snapshot, you can snapshot just the following two sub-folders (and all
of their contents):

```
PinscapePico/
  USBProtocol/
  WinAPI/
```

If it bothers you to incorporate files from another repository as a
static snapshot (for example, because you don't like the idea of
having to manually update the snapshot to keep up with changes to the
original source tree), git provides several mechanisms for including
live links to other repositories, such as subprojects, submodules,
and subtrees.  It won't matter to the build whether you use a static
snapshot or one of the git reference schemes, as long as you have
a snapshot of the library tree somewhere in your local project tree
when you run the Visual Studio build.

If your project already includes other third-party libraries, add the
PinscapePico folder as a peer of those other third-party library
folders.  Otherwise, you can add it as a subfolder of your main
source folder.

A typical project tree involving multiple third-party libraries might
look something like this:

```
project/
  src/                - contains all of my own project's C++ source files
  include/            - all of my own project's header files
  third-party/        - third-party libraries main folder
    PinscapePico/     - Pinscape Pico third-party library
```

In this case, you'd need to set your C++ compiler's include file path to 
include the third-party/ folder in every #include \<xxx\> search.

Now open your project in Visual Studio, right-click on the main Solution
item in the Solution Explorer panel, and select Add > Existing Project.
Select PinscapePico/WinAPI/PinscapePicoAPI.vcxproj.

Right click the new PinscapePicoAPI project in your Solution Explorer
panel and select Properties.  Static link libraries require a few compiler
options to be set the same way between the library and the main project
that includes it, so check the following items, and change them to match
your main project settings as needed:

* Advanced > Character Set

* C/C++ > Code Generation > Runtime Library

Close the properties dialog when done.

On the main manu, select Project > Project Dependencies.  In the dialog,
open the Projects drop list and select your main project.  Tick the 
PinscapePicoAPI box to specify that your project depends on the
PinscapePicoAPI project as a client.

In each of your project files where you'll call Pinscape Pico API
functions, add a #include for the appropriate API header:

```
#include <PinscapePico/WinAPI/PinscapeVendorInterface.h>
#include <PinscapePico/WinAPI/FeedbackControllerInterface.h>
```

Note that most projects will only use one or the other API division,
so you'll usually only need to include one of these files.

In one of your source files, also add the following #pragma:

```
#pragma comment(lib, "PinscapePicoAPI.lib")
```

That instructs the linker to include the PinscapePicoAPI.lib static
link library file.

In order to make the link work properly, you'll also have to add
the following to your project properties, in the **VC++ Directories**
page, under **General > Library Directories**:

```
$(SolutionDir)$(Platform)\$(Configuration);<existing options>
```

(The `<existing options>` part is meant to represent whatever was
already in that box.  In other words, insert the new material
before whatever's currently there.)

You should now be able to call the Pinscape Pico API functions
from your project source files.  To test it out, try adding this
snippet of code somewhere suitable:

```
std::list<PinscapePico::FeedbackControllerInterface::Desc> picos;
if (SUCCEEDED(PinscapePico::FeedbackControllerInterface::Enumerate(picos)))
   OutputDebugStringA("It worked!\n");
```

Now try building your project to make sure that the compiler and linker
options are set up to find all of the library files properly.  If that
works, you can use the Visual Studio debugger to step through the code above
to make sure that the API call works.  If you have any Pinscape Pico units 
attached to your system, the std::list should come back populated with a 
descriptor object for each connected unit.  You can look at the descriptor 
object properties in the debugger to see its various identifiers.

## License

Copyright 2024, 2025 Michael J Roberts

Released under a BSD 3-clause license - NO WARRANTY
