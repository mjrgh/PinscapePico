// Pinscape Pico - Config Doc Parser
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//
// This parses a file prepared in a custom source format for documenting
// the Pinscape Pico's JSON configuration file entries.  The format is
// designed to index the documentation by JSON element, so that other
// tools can mechanically cross-reference JSON elements with their entries
// in the documentation.  The source also contains some amount of schema
// information that can be used to at least partially validate a JSON file,
// such as checking datatypes and checking for missing required elements.
// The generator outputs a human-readable HTML documentation file, and a
// file of C++ structs representing the schema tree.  The C++ structs are
// designed for consumption by the Config Tool editor, so that it can
// provide context-sensitive auto-complete and help links.

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <map>
#include <set>
#include <list>
#include <string>

class ConfigDocParser
{
public:
	// source file name
	std::string srcfile;

	// property map
	struct Prop;
	using PropMap = std::map<std::string, Prop>;

	// object type
	struct Object
	{
		// property map
		PropMap props;

		// Class selector property value and class description text.  These apply
		// to nodes that end with a "type=xxx" subclass designation, where the
		// text is for the class rather than for one of its properties.
		std::string classType;
		std::list<std::string> classText;
		std::list<std::string> classExample;

		// Objects can have subclasses, specified by a property (often
		// named "type") that selects the subclass type.  When there's
		// a subclass, each concrete subclass can provide its own additional
		// properties that are used only when that subclass type is selected.
		std::string subclassSelectorPropName;
		std::map<std::string, Object> subclassTypes;

		// object number in generated schema
		int objNum = -1;
	};

	// JSON object property
	struct Prop
	{
		// primitive types
		enum class PrimitiveType
		{
			None,       // invalid/undefined
			Object,		// sub-object
			String,		// string
			Number,		// number
			Bool,       // boolean
			GPIO,		// a number referring to a GPIO port
			ADCGPIO,	// a number referring to an ADC-capable GPIO port
			Array,      // array of subtypes
		};

		// mappings from primitive type to string and back
		std::map<std::string, PrimitiveType> nameToTypeMap;
		std::map<PrimitiveType, std::string> typeToNameMap;

		// Valid type or types for the property
		struct Type
		{
			Type(PrimitiveType t) : primitiveType(t) { }
			Type(PrimitiveType t, std::string &xref) : primitiveType(t), xref(xref) { }

			// primitive type
			PrimitiveType primitiveType;

			// Object type cross-reference.  Some object types refer back
			// to types defined elsewhere, rather than defining a new type
			// inline.  This contains the internal HREF style ID of the
			// defining property, such as "outputs.device".
			std::string xref;

			// resolved object cross-reference
			Object *xrefObj = nullptr;

			// for arrays, the types that can populate the array
			std::list<Type> subTypes;
		};
		std::list<Type> types;

		// enumerated values comparator
		struct EnumeratedValueComparator {
			bool operator()(const std::string &a, const std::string &b) const;
		};

		// Enumerated values allowed.  When present, these are the only
		// valid values for the property.  The values are stored as strings
		// but are interpreted according to the property type; when a property
		// has enumerated values, it can have only one type, so the type is
		// necessarily always the first (and only) in the type list.
		std::set<std::string, EnumeratedValueComparator> enumeratedValues;

		// is the property required?
		bool required;

		// link - we use this to generate <a href> cross-references
		std::string link;

		// section title text (TITLE: xxx in the main text)
		std::string title;

		// documentation text for this property node
		std::list<std::string> text;

		// example text
		std::list<std::string> example;

		// SEE reference
		std::string see;

		// VALIDATE pattern - a regex that a validation tool can use to
		// check validity of the value in this property
		std::string validate;

		// If the property contains an object or an array of objects,
		// we store a sub-object for its property list.
		Object subObj;
	};

	// root object 
	Object rootObject;

	// introductory text
	std::list<std::string> intro;

	// table of contents
	struct TOCEntry
	{
		TOCEntry() { }
		TOCEntry(const char *title) : title(title) { }
		TOCEntry(std::string &title, Prop *prop) : title(title), prop(prop) { }

		// section title
		std::string title;

		// for leaf nodes, the property link
		Prop *prop = nullptr;

		// for container nodes, the sub-TOC
		std::map<std::string, TOCEntry> subToc;
	};

	// root contents
	TOCEntry toc{ "Table of Contents" };

	// Parse a file, populating the root object property list
	void ParseFile(const char *filename);

	// Generate HTML documentation
	void GenerateHTML(const char *filename);

	// Generate schema C++ structs
	void GenerateSchemaStructs(const char *filename);
};
