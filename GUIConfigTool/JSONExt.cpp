// Pinscape Pico - JSON parser extensions
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
// 
// Extensions to the simple JSON parser used in the firmware, to allow
// dynamic construction and modification of the value tree.

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <ostream>
#include "JSONExt.h"

void JSONParserExt::SetType(Value *value, Value::Type type)
{
	if (value->type != type)
	{
		value->Reset();
		value->Set(type);
	}
}

void JSONParserExt::SetBool(Value *value, bool b)
{
	value->Reset();
	value->type = (b ? Value::Type::True : Value::Type::False);
}

void JSONParserExt::SetBool(Value *value, const char *path, bool b)
{
	SetType(value, path, (b ? Value::Type::True : Value::Type::False));
}

void JSONParserExt::SetNum(Value *value, double d)
{
	// make it a number node and store the value
	SetType(value, Value::Type::Number);
	value->number = d;
}

void JSONParserExt::SetNum(Value *value, const char *path, double d)
{
	SetNum(SetType(value, path, Value::Type::Number), d);
}

void JSONParserExt::SetStr(Value *value, const char *str, size_t len)
{
	// make it a string node
	SetType(value, Value::Type::String);

	// Copy the string into the pool and set the value node to point there.
	// (We can't count on the caller's string to remain valid after the
	// call returns, so we have to make a pool copy to keep.)
	auto &pooledStr = stringPool.emplace_back(str, len);
	value->string.Set(pooledStr.c_str(), pooledStr.size());
}

void JSONParserExt::SetStr(Value *value, const char *path, const char *str, size_t len)
{
	SetStr(SetType(value, path, Value::Type::String), str, len);
}

JSONParser::Value *JSONParserExt::SetType(const char *path, Value::Type type)
{
	return SetType(&rootValue, path, type);
}

JSONParser::Value *JSONParserExt::SetType(Value *value, const char *path, Value::Type type)
{
	// Get the starting element delimiter.  If the path doesn't start with
	// an explicit '.' or '[', it's implicitly a property of the root
	// object.
	char delim = '.';
	if (*path == '.' || *path == '[')
		delim = *path++;

	// parse each path element
	for (const char *p = path ; delim != 0 ; )
	{
		// scan ahead to the next '.' or '['
		path = p;
		for (; *p != 0 && *p != '.' && *p != '[' ; ++p);

		// pull out the property name
		Value::StringWithLen ele(path, p - path);

		// traverse into the element
		if (delim == '.')
		{
			// object property dereference - make sure the node is an object
			SetType(value, Value::Type::Object);

			// get the element at this property name, or add one if it doesn't exist
			if (auto *prop = value->object->find(ele) ; prop != nullptr)
				value = &prop->val;
			else
			{
				Token tok;
				value = &value->object->emplace(this, ele, tok, Value::Type::Undefined)->val;
			}
		}
		else if (delim == '[')
		{
			// array index - make sure the node is an array
			SetType(value, Value::Type::Array);

			// evaluate the index value
			int index = atoi(ele.txt);

			// make sure the array is populated to this point
			while (static_cast<int>(value->array->size()) < index)
				value->array->emplace_back(new ArrayEleValue(Value::Type::Undefined));

			// the array element is the new value
			value = value->array->at(index).get();
		}

		// set the new delimiter
		delim = *p++;
	}

	// set the leaf value node type
	SetType(value, type);

	// return the leaf value node
	return value;
}

void JSONParserExt::Generate(std::ostream &s, const Value *value)
{
	switch (value->type)
	{
	case Value::Type::Undefined:
		s << "undefined";
		break;

	case Value::Type::Null:
		s << "null";
		break;

	case Value::Type::True:
		s << "true";
		break;

	case Value::Type::False:
		s << "false";
		break;

	case Value::Type::Number:
		// number - format it as an integer if possible, otherwise as a double
		if (double intPart = 0; modf(value->number, &intPart) == 0.0)
			s << static_cast<int64_t>(intPart);
		else
			s << value->number;
		break;

	case Value::Type::String:
		s << "\"";
		for (const char *p = value->string.txt, *endp = p + value->string.len ; p < endp ; ++p)
		{
			char c = *p;
			switch (c)
			{
			case '"':
			case '\\':
				// quote it
				{
					char buf[3]{ '\\', c, 0 };
					s << buf;
				}
				break;

			case '\n':
				s << "\\n";
				break;

			case '\t':
				s << "\\t";
				break;

			default:
				if (c < 32 || c > 126)
				{
					char buf[8];
					sprintf_s(buf, "\\%03o", c);
					s << buf;					
				}
				else
				{
					char buf[2]{ c, 0 };
					s << c;
				}
				break;
			}
		}
		s << "\"";
		break;

	case Value::Type::Object:
		s << "{";
		{
			int n = 0;
			for (auto *prop = value->object->props ; prop != nullptr ; prop = prop->nxt)
			{
				// add a separator before the second and later elements
				if (n++ > 0) s << ",";

				// write the property name in quotes
				s << '"';
				const char *p = prop->name.txt;
				for (size_t i = 0, len = prop->name.len ; i < len ; ++i)
				{
					// escape quotes and backslashes
					char c = *p++;
					if (c == '"' || c == '\\')
						s << '\\';
					s << c;
				}
				s << '"';

				// add the colon and generate the value recursively
				s << ":";
				Generate(s, &prop->val);
			}
		}
		s << "}";
		break;

	case Value::Type::Array:
		s << "[";
		for (size_t i = 0 ; i < value->array->size() ; ++i)
		{
			// add a separator before the second and later elements
			if (i > 0) s << ",";

			// generate the value recursively
			Generate(s, value->array->at(i).get());
		}
		s << "]";
		break;

	default:
		s << "$BAD_TYPE_" << static_cast<int>(value->type)   << "$";
		break;
	}
}

std::string JSONParserExt::Path::ToString()
{
	std::string s;
	for (auto &ele : path)
	{
		if (ele.prop.size() != 0)
		{
			if (s.size() != 0) s.append(".");
			s.append(ele.prop);
		}
		else
		{
			char buf[32];
			sprintf_s(buf, "[%d]", ele.index);
			s.append(buf);
		}
	}
	return s;
}

JSONParserExt::Path JSONParserExt::FindTextPtr(const char *p, Value **nodePtr)
{
	Path path;
	Value *val = &rootValue;
	for (bool done = false ; !done && val != nullptr && NodeContainsText(val, p) ; )
	{
		// if it's a composite type, find the sub-node containing the text
		Value *nextVal = nullptr;
		switch (val->type)
		{
		case Value::Type::Object:
			// scan the object property list for a property enclosing the text position
			{
				PropValue *firstFollower = nullptr;
				for (auto *ele = val->object->props ; ele != nullptr ; ele = ele->nxt)
				{
					if (NodeContainsText(&ele->val, p))
					{
						// it's in this property - add the path element
						path.path.emplace_back(val, ele->name.txt, ele->name.len);

						// traverse to the next value
						nextVal = &ele->val;
						break;
					}

					// note the closest follower
					if (p < ele->val.propTok.srcTxt)
					{
						// this item follows 'p'; see if it's the first (in source text order) so far
						if (firstFollower == nullptr || ele->val.propTok.srcTxt < firstFollower->propTok.srcTxt)
							firstFollower = &ele->val;
					}
				}

				// if we didn't find an enclosing property, but we're
				// bracketed by two properties, check if we're close to
				// the next one
				if (nextVal == nullptr && firstFollower != nullptr)
				{
					// If there's a newline between here and the follower,
					// group with the follower; otherwise, we're in open
					// space within the parent.
					const char *pp = p;
					for (int i = 0 ; i < 1024 && pp < firstFollower->propTok.srcTxt && *pp != '\n' ; ++pp, ++i);
					if (pp == firstFollower->propTok.srcTxt)
					{
						// it's on the same line as the next property - count it as 
						// part of the next item

						// append the path element
						path.path.emplace_back(firstFollower, firstFollower->propTok.txt, firstFollower->propTok.len);

						// traverse to the next value
						nextVal = firstFollower;
					}
				}
			}
			break;

		case Value::Type::Array:
			for (size_t i = 0, n = val->array->size() ; i < n ; ++i)
			{
				ArrayEleValue *ele = val->array->at(i).get();
				if (NodeContainsText(ele, p))
				{
					// it's within this element - add it to the path
					path.path.emplace_back(val, static_cast<int>(i));

					// traverse to the next value
					nextVal = ele;
					break;
				}
				else if (p < ele->startTok.srcTxt)
				{
					// It's before this element, so it's in the space
					// between the previous item's ending delimiter and
					// this item.  Count it as part of this item.

					// append the path element
					path.path.emplace_back(val, static_cast<int>(i));

					// traverse to the element
					nextVal = val->array->at(i).get();
					break;
				}
			}
			break;

		default:
			// leaf node - stop here and return the node
			done = true;
			nextVal = val;
			break;
		}

		// move on to the sub-node
		val = nextVal;
	}

	// pass back the final node if desired
	if (nodePtr != nullptr)
		*nodePtr = val;

	// return the path
	return path;
}

bool JSONParserExt::NodeContainsText(const Value *val, const char *p)
{
	return p >= val->startTok.srcTxt && p <= val->endTok.SrcEnd();
}

bool JSONParserExt::NodeContainsText(const PropValue *val, const char *p)
{
	return (p >= val->propTok.srcTxt && (p <= val->endTok.SrcEnd() || p < val->delimTok.srcTxt));
}

bool JSONParserExt::NodeContainsText(const ArrayEleValue *val, const char *p)
{
	return (p >= val->startTok.srcTxt && (p <= val->endTok.SrcEnd() || p < val->delimTok.srcTxt));
}
