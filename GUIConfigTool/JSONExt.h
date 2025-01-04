// Pinscape Pico - JSON parser extensions
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
// 
// Extensions to the simple JSON parser used in the firmware, to allow
// dynamic construction and modification of the value tree.

#pragma once
#include <string.h>
#include <ostream>
#include "../Firmware/JSON.h"

class JSONParserExt : public JSONParser
{
public:
	// Generate to a stream
	void Generate(std::ostream &s) { Generate(s, &rootValue); }
	void Generate(std::ostream &s, const Value *value);

	// Set a path value/type
	void SetBool(const char *path, bool b) { SetBool(&rootValue, path, b); }
	void SetNum(const char *path, double d) { SetNum(&rootValue, path, d); }
	void SetStr(const char *path, const char *str) { SetStr(&rootValue, path, str); }
	void SetStr(const char *path, const char *str, size_t len) { SetStr(&rootValue, path, str, len); }

	// set a node's value and/or type
	void SetType(Value *value, Value::Type type);
	void SetBool(Value *value, bool b);
	void SetNum(Value *value, double d);
	void SetStr(Value *value, const char *str) { SetStr(value, str, strlen(str)); }
	void SetStr(Value *value, const char *str, size_t len);

	// set a sub-path value/type
	void SetBool(Value *value, const char *path, bool b);
	void SetNum(Value *value, const char *path, double d);
	void SetStr(Value *value, const char *path, const std::string &str) { SetStr(value, path, str.c_str(), str.length()); }
	void SetStr(Value *value, const char *path, const char *str) { SetStr(value, path, str, strlen(str)); }
	void SetStr(Value *value, const char *path, const char *str, size_t len);

	// Set the element at the given path to object type.  If it's
	// already an object, nothing changes; if it's not already an
	// object, a new empty object replaces any existing value.
	Value *SetObject(const char *path) { return SetType(path, Value::Type::Object); }
	Value *SetObject(Value *value, const char *path) { return SetType(value, path, Value::Type::Object); }

	// Set the element at the given path to array type.
	Value *SetArray(const char *path) { return SetType(path, Value::Type::Array); }

	// Set the element at the given path to the given type; returns
	// the value node.
	Value *SetType(const char *path, Value::Type type);
	Value *SetType(Value *value, const char *path, Value::Type type);

	// Parse path, for FindTextPtr
	struct Path
	{
		struct PathEle
		{
			PathEle(Value *value, const char *str, size_t len) : value(value), prop(str, len) { }
			PathEle(Value *value, const char *str) : value(value), prop(str) { }
			PathEle(Value *value, const std::string &str) : value(value), prop(str) { }
			PathEle(Value *value, int index) : value(value), index(index) { }

			Value *value;      // value node
			std::string prop;  // property name, if this is a property dereference, else blank
			int index = -1;    // array index; ignored if the property name is present
		};
		std::list<PathEle> path;

		// convert to a string representation
		std::string ToString();
	};

	// Find the node whose token text contains the given text pointer.
	// Returns the selection expression from the root node, and fills
	// in 'node' with the value node.  node can be nullptr if the value
	// node isn't required.
	Path FindTextPtr(const char *p, Value **node);

	// does the given value node contain the given text?
	bool NodeContainsText(const Value *val, const char *p);
	bool NodeContainsText(const PropValue *val, const char *p);
	bool NodeContainsText(const ArrayEleValue *val, const char *p);

protected:
	// String pool.  String value nodes contain pointers to strings that
	// must have storage duration at least as long as the parser object.
	// When parsing source text, this allows strings to point directly
	// into the source text without the need for extra copies.  For dynamic
	// construction and modification, though, we can't count on external
	// strings having sufficient storage duration, so we have to explicitly
	// make copies.  We do that by making copies in this pool.  Note that
	// this is highly inefficient for use cases where the tree will have
	// long lifetime and will be extensively modified, since we don't
	// garbage-collect strings if they later become unreferenced from the
	// tree.  This is designed only for simple use cases where the tree
	// is constructed once and then stays fairly static, or is discarded
	// entirely.  (The pooled strings ARE freed when the whole parser is
	// deleted, so they don't "leak", but they do accumulate as long as
	// the parse tree itself is alive.)
	std::list<std::string> stringPool;
};

