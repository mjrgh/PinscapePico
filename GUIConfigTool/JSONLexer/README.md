JSON Lexer
----------

This folder contains an excerpted and slightly modified version of the JSON
Lexer from Lexilla (https://www.scintilla.org/Lexilla.html), along with the
minimum set of Lexilla library source files needed to compile it.  This was all
excerpted from the main Lexilla repository at:

https://github.com/ScintillaOrg/lexilla

The Pinscape Pico project uses the Scintilla text editor component for editing
the JSON configuration files used to program the Pico, and Scintilla uses
plug-ins called "lexers" to handle syntax colorization specially for individual
programming languages.  The official Lexilla repository contains a vast number
lexers for different languages - many more than we need for the Pinscape
project, since we only use Scintilla as an embedded component dedicated solely
to editing JSON files.  To minimize Pinscape's source code repository size, I
extracted just enough of the Lexilla code to compile the JSON lexer into this
folder.  This includes the JSON lexer itself, along with the minimum set of
Lexilla library modules that the JSON lexer depends upon.

Modifications from the original Lexilla files:

- Include paths are adjusted to reflect the new folder structure

- The JSON lexer has been tweaked to accept the non-standard syntax that
Pinscape Pico accepts (property names aren't required to be enclosed in quotes,
hex and octal integer formats are accepted)

