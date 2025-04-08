// Pinscape Pico - Config Doc Parser
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <map>
#include <list>
#include <string>
#include <fstream>
#include <functional>
#include <regex>
#include "ConfigDocParser.h"


using PrimitiveType = ConfigDocParser::Prop::PrimitiveType;
static const std::map<std::string, PrimitiveType> propNameToTypeMap{
	{ "object", PrimitiveType::Object },
	{ "string", PrimitiveType::String },
	{ "bool", PrimitiveType::Bool },
	{ "number", PrimitiveType::Number },
	{ "gpio", PrimitiveType::GPIO },
	{ "adcgpio", PrimitiveType::ADCGPIO },
	{ "array", PrimitiveType::Array },
};

static const std::map<PrimitiveType, std::string> propTypeToNameMap{
	{ PrimitiveType::None, "none" },
	{ PrimitiveType::Object, "object" },
	{ PrimitiveType::String, "string" },
	{ PrimitiveType::Bool, "boolean" },
	{ PrimitiveType::Number, "number" },
	{ PrimitiveType::GPIO, "gpio" },
	{ PrimitiveType::ADCGPIO, "adcgpio" },
	{ PrimitiveType::Array, "array" },
};

// enumerated values comparator
bool ConfigDocParser::Prop::EnumeratedValueComparator::operator()(const std::string &a, const std::string &b) const
{
	// if both are numeric, compare them numerically
	static const std::regex numPat("\\d+");
	if (std::regex_match(a, numPat) && std::regex_match(b, numPat))
		return atoi(a.c_str()) < atoi(b.c_str());

	// otherwise, compare them lexically, ignoring case
	return _stricmp(a.c_str(), b.c_str()) < 0;
}

void ConfigDocParser::ParseFile(const char *filename)
{
	// remember the source file name
	srcfile = filename;

	// open the file
	std::fstream f(filename, std::ios_base::in);
	if (!f)
	{
		printf("Config Doc Parser: error opening input file \"%s\"\n", filename);
		exit(1);
	}

	// read the first line
	std::string l;
	std::getline(f, l);

	// keep going until EOF
	while (f)
	{
		// skip blank lines
		std::regex blank("\\s*");
		if (std::regex_match(l, blank))
		{
			std::getline(f, l);
			continue;
		}

		// check for an introduction
		if (std::regex_match(l, std::regex("\\s*INTRO:\\s*")))
		{
			// scan until we see another special directive line
			while (std::getline(f, l) && !std::regex_match(l, std::regex("\\s*[A-Z]+:\\s*")))
				intro.emplace_back(l);

			// go back to the top of the main loop to process the new directive line
			continue;
		}

		// check for a REFERENCE: line
		if (std::regex_match(l, std::regex("\\s*REFERENCE:\\s*")))
		{
			// this line is really just an ending marker for the preamble
			// sections (INTRO:), so once we find it, we can simply skip it
			std::getline(f, l);
			continue;
		}

		// parse the property name
		const char *propNameStart = l.c_str();
		const char *p = propNameStart;
		Object *obj = &rootObject;
		Prop dummyProp, *prop = &dummyProp;
		std::list<std::string> *docCollection = &obj->classText;
		std::list<std::string> *exampleCollection = &obj->classExample;
		std::string link;
		for (;; ++p)
		{
			if (*p == 0 || isspace(*p) || *p == '.' || memcmp(p, "[].", 3) == 0 || *p == '=')
			{
				// pull out the property name
				std::string propName(propNameStart, p - propNameStart);

				// append it to the link
				if (link.size() != 0) link.append(".");
				link.append(propName);

				// Check for '=' notation, which specifies a value for this
				// property that gives the enclosing object a subclass.
				std::string subclass;
				if (*p == '=')
				{
					// parse the property value
					char qu = 0;
					const char *val = ++p;
					if (*p == '"')
					{
						qu = *p++;
						val = p;
					}

					for ( ; ; ++p)
					{
						if (*p == 0)
						{
							subclass.append(val, p - val);
							break;
						}
						else if (qu != 0)
						{
							if (*p == qu)
							{
								subclass.append(val, p - val);
								val = p + 1;
								qu = 0;
							}
						}
						else
						{
							if (*p == '.' || memcmp(p, "[].", 3) == 0 || isspace(*p))
							{
								subclass.append(val, p - val);
								break;
							}
							if (*p == '"')
							{
								subclass.append(val, p - val);
								qu = *p;
								val = p + 1;
							}
						}
					}

					// This defines a subclass type
					obj->subclassSelectorPropName = propName;
					if (auto it = obj->subclassTypes.find(subclass); it != obj->subclassTypes.end())
						obj = &it->second;
					else
						obj = &obj->subclassTypes.emplace(std::piecewise_construct, std::forward_as_tuple(subclass), std::forward_as_tuple()).first->second;

					// since this is just a subclass indicator, it doesn't add a property to the path,
					// and documentation goes to the class text collection
					prop = &dummyProp;
					docCollection = &obj->classText;
					exampleCollection = &obj->classExample;
					obj->classType = subclass;

					// append it to the link
					link.append("=" + subclass);
				}
				else
				{
					// look up the property name in the current object
					if (auto it = obj->props.find(propName) ; it != obj->props.end())
						prop = &it->second;
					else
						prop = &obj->props.emplace(std::piecewise_construct, std::forward_as_tuple(propName), std::forward_as_tuple()).first->second;

					// move to this object
					obj = &prop->subObj;
					docCollection = &prop->text;
					exampleCollection = &prop->example;
				}

				// skip the extra characters for the array notation
				if (*p == '[')
					p += 2;

				// stop at space or end of line
				if (isspace(*p) || *p == 0)
					break;

				// reset for the next property
				propNameStart = p + 1;
			}
		}

		// skip spaces
		for (; isspace(*p) ; ++p) ;

		// Parse the type list
		const char *start = p;
		for (bool isArray = false ; ; ++p)
		{
			// types are separated by '|', and array types end with ']'; the 
			// whole section ends with a space or end of line
			if (*p == '|' || *p == ']' || *p == '{' || *p == 0 || isspace(*p))
			{
				// skip empty property lists
				if (p == start)
					break;

				// get the type name
				std::string typeName(start, p - start);

				// parse object cross-references
				static std::regex xrefPat("object<(.+)>");
				std::match_results<std::string::const_iterator> m;
				std::string xref;
				if (std::regex_match(typeName, m, xrefPat))
				{
					xref = m[1].str();
					typeName = "object";
				}

				// look up the type name
				auto it = propNameToTypeMap.find(typeName);
				if (it == propNameToTypeMap.end())
				{
					printf("Error: invalid type name \"%.*s\"\n", static_cast<int>(p - start), start);
					exit(1);
				}

				// if we're in an array, add it to the current array element
				if (isArray)
					prop->types.back().subTypes.emplace_back(it->second, xref);
				else
					prop->types.emplace_back(it->second, xref);

				// skip an array ender
				if (*p == ']')
				{
					++p;
					isArray = false;
				}

				// check for an enumerated value list
				std::string tok;
				if (*p == '{')
				{
					// parse the value list
					start = ++p;
					for (bool inStr = false ; ; ++p)
					{
						if (*p == 0)
						{
							break;
						}
						else if (inStr)
						{
							if (*p == '"')
							{
								inStr = false;
							}
							else if (*p == '\\')
							{
								// escape sequence
								tok.append(start, p - start);
								switch (*++p)
								{
								case 'n':
									tok.append("\n");
									start = p + 1;
									break;

								case 'r':
									tok.append("\r");
									start = p + 1;
									break;

								case 't':
									tok.append("\t");
									start = p + 1;
									break;

								case '"':
								case '\\':
									start = p;
									break;

								default:
									printf("Invalid escape sequence \\%c\n", *p);
									exit(1);
									break;
								}
							}
						}
						else
						{
							if (*p == '"')
							{
								inStr = true;
							}
							else if (*p == 0 || isspace(*p) || *p == '}')
							{
								tok.append(start, p - start);
								prop->enumeratedValues.emplace(tok);
								tok.clear();

								for (; isspace(*p); ++p) ;
								if (*p == '}')
								{
									++p;
									break;
								}
								start = p;
								--p;
							}
						}
					}
				}

				// stop at the end of the type list
				if (isspace(*p) || *p == 0)
					break;

				// start a new type
				start = p + 1;
			}
			else if (*p == '[')
			{
				isArray = true;
				prop->types.emplace_back(Prop::PrimitiveType::Array);
				start = p + 1;
			}
		}

		// parse the optional/required indicator
		for (; isspace(*p) ; ++p) ;
		const char *attrStart = p;
		for (; *p != 0 && !isspace(*p) ; ++p) ;
		std::string attr(attrStart, p - attrStart);
		if (prop != nullptr)
			prop->required = (attr == "required");

		// save the hyperlink
		prop->link = link;

		// now scan indented/blank lines and add them to the documentation text
		for (;;)
		{
			// get the next line
			if (!std::getline(f, l))
				break;

			// if it's indented or blank, it's documentation for this section
			if (std::regex_match(l, blank) || isspace(l[0]))
			{
				// skip leading spaces
				for (p = l.c_str() ; isspace(*p) ; ++p) ;

				// check for special types
				if (memcmp(p, "TOC:", 4) == 0)
				{
					// table of contents entry
					TOCEntry *tocp = &toc;
					for (p += 4 ; *p != 0 ; )
					{
						// skip spaces
						for (; isspace(*p) ; ++p);

						// find the next subsection marker ('>')
						const char *start = p;
						for (; *p != 0 && *p != '>' ; ++p) ;

						// trim ending spaces
						const char *end = p;
						for (; end > start && isspace(*(end-1)) ; --end) ;
						std::string title(start, end - start);

						// if there's more, look up the section entry
						if (*p == '>')
						{
							// it's a section entry - traverse into the sub-TOC
							tocp = &tocp->subToc[title];
							tocp->title = title;
						}
						else
						{
							// it's a leaf node - add a property entry
							tocp->subToc.emplace(std::piecewise_construct, std::forward_as_tuple(title), std::forward_as_tuple(title, prop));
						}

						// if we're at a '>', skip it
						if (*p == '>')
							++p;
					}
				}
				else if (memcmp(p, "TITLE:", 6) == 0)
				{
					// title entry
					for (p += 6 ; isspace(*p) ; ++p);
					prop->title = p;
				}
				else if (memcmp(p, "SEE:", 4) == 0)
				{
					for (p += 4 ; isspace(*p) ; ++p);
					prop->see = p;
				}
				else if (memcmp(p, "VALIDATE:", 9) == 0)
				{
					for (p += 9 ; isspace(*p) ; ++p);
					prop->validate = p;
				}
				else if (memcmp(p, "EXAMPLE:", 8) == 0)
				{
					// example text
					while (std::getline(f, l))
					{
						// stop at the first blank line
						if (std::regex_match(l, blank))
							break;

						// add the line to the example test; a "." by itself represents a blank line
						if (l == ".")
							l = "";

						// add it to the example text
						exampleCollection->emplace_back(l);
					}
				}
				else if (memcmp(p, "<pre>", 5) == 0)
				{
					// preformatted text section
					
					// note the number of leading spaces on the <pre> line
					size_t nLeadingSpaces = p - l.c_str();

					// read up to the </pre>
					std::string l2;
					while (std::getline(f, l2))
					{
						// strip the same number of leading spaces as were on the <pre> line
						const char *p2 = l2.c_str();
						for (size_t i = 0 ; i < nLeadingSpaces && isspace(*p2) ; ++p2, ++i);

						// check for the </pre>
						if (std::regex_match(l2, std::regex("\\s*</pre>\\s*")))
						{
							// add the </pre> to the end of the last line, so that we don't
							// get an extra blank line at the end
							docCollection->emplace_back(l + "</pre>");

							// done
							break;
						}

						// write out the prior line and rotate in the new line
						docCollection->emplace_back(l);
						l.assign(p2);
					}
				}
				else
				{
					// plain text - add it to the documentation collection
					docCollection->emplace_back(p);
				}
			}
			else
			{
				// this is a new property line
				break;
			}
		}

		// check for "prop={table}" syntax, which takes the enumerated value
		// list from an HTML table in the documentation text
		if (prop->enumeratedValues.size() == 1 && *prop->enumeratedValues.begin() == "table")
		{
			// delete the placeholder "table" element
			prop->enumeratedValues.clear();

			// add the table elements
			bool inTable = false;
			for (auto &t : prop->text)
			{
				if (!inTable)
				{
					if (std::regex_match(t, std::regex("\\s*<table>\\s*", std::regex_constants::icase)))
						inTable = true;
				}
				else
				{
					if (std::regex_match(t, std::regex("\\s*</table>\\s*", std::regex_constants::icase)))
						break;

					std::match_results<std::string::const_iterator> m;
					if (std::regex_match(t, m, std::regex("\\s*<tr><td>([^<]+)</td>.*")))
						prop->enumeratedValues.emplace(m[1].str());
				}
			}
		}
	}
}

void ConfigDocParser::GenerateHTML(const char *filename)
{
	// open the file
	std::fstream f(filename, std::ios_base::out);
	if (!f)
	{
		printf("Config Doc Parser: error opening output file \"%s\"\n", filename);
		exit(1);
	}

	// generate the initial boilerplate
	f << "<!DOCTYPE html>\n"
		"<!-- Pinscape Pico / Copyright 2024 Michael J Roberts / BSD-3-Clause license / NO WARRANTY -->\n"
		"<html>\n"
		"<head>\n"
		"  <title>Pinscape Pico JSON Configuration Reference</title>\n"
		"  <link rel=\"stylesheet\" href=\"Help.css\">\n"
		"</head>\n"
		"<body>\n"
		"<div id=\"TopNav\">\n"
		"  <a href=\"ConfigTool.htm\">Pinscape Pico Config Tool</a> &gt; JSON Configuration Reference\n"
		"</div>\n"
		"\n"
		"<h1>JSON Configuration Reference</h1>\n"
		"\n"
		"<p>";

	// generate the introduction
	for (auto &t : intro)
	{
		// blank lines are paragraph breaks; pass everything else as literal HTML
		if (std::regex_match(t, std::regex("\\s*")))
			f << "</p>\n<p>\n";
		else
			f << t << "\n";
	}
	f << "</p>";

	// generate the table of contents
	std::function<void(TOCEntry*, std::string, int)> GenTOC = [this, &f, &GenTOC](TOCEntry *toc, std::string indent, int level)
	{
		if (toc->prop != nullptr)
		{
			// leaf node
			f << indent << "<div class=\"jsonTOCEntry\">\n"
				<< indent << "  <a href=\"#" << toc->prop->link << "\">" << toc->title << "</a>\n"
				<< indent << "</div>\n";
		}
		else
		{
			// hierarchy node
			f << indent << "<div class=\"jsonTOCSect tocLevel" << level << "\">\n"
				<< indent << "  <div class=\"jsonTOCSectTitle\">" << toc->title << "</div>\n";

			// special items at the root level
			if (level == 0)
			{
				f << "  <div class=\"jsonTOCEntry\">\n"
					<< "    <a href=\"#RootObjectSummary\">Root object summary</a>\n"
					<< "  </div>\n";
			}

			// generate sub-items 
			for (auto &st : toc->subToc)
				GenTOC(&st.second, indent + "  ", level + 1);

			f << indent << "</div>\n";
		}
	};
	f << "<div class=\"TOC\">\n";
	GenTOC(&toc, "", 0);
	f << "</div>\n";

	// generate a property name
	static auto JsonPropName = [](const std::string &str) -> std::string
	{
		if (str.size() == 0 || !(isalpha(str[0]) && str[0] != '_' && str[0] != '$'))
			return "\"" + str + "\"";
		else
			return str;
	};

	// generate a section index
	using propType = std::pair<std::string, Prop>;
	std::function<void(propType, const std::string&)> GenIndex = [this, &f, &GenIndex](propType prop, const std::string &indent)
	{
		bool isObj = false, someArray = false, allArray = true;
		std::string *objXRef = nullptr;
		for (auto &t : prop.second.types)
		{
			if (t.primitiveType == Prop::PrimitiveType::Array)
			{
				someArray = true;
				for (auto &tt : t.subTypes)
				{
					if (tt.primitiveType == Prop::PrimitiveType::Object)
						isObj = true;
				}
			}
			else
				allArray = false;

			if (t.primitiveType == Prop::PrimitiveType::Object)
			{
				isObj = true;
				objXRef = &t.xref;
			}
		}

		std::string subIndent = indent + "   ";
		f << indent << "<span class=\"propName\">"
			<< "<a href=\"#" << prop.second.link << "\">"
			<< JsonPropName(prop.first) << "</a></span>: ";
		if (isObj)
		{
			if (allArray)
			{
				f << "[\n" << indent << "   {\n";
				subIndent += "   ";
			}
			else if (someArray)
				f << "{     // object <i>or</i> array of objects\n";
			else
				f << "{\n";

			//if (prop.second.see.size() != 0)
			//	f << subIndent << "// see <a href=\"#" << prop.second.see << "\">" << prop.second.see << "</a>\n";

			if (objXRef != nullptr && objXRef->size() != 0)
				f << subIndent << "// same as <a href=\"#" << *objXRef << "\">" << *objXRef << "</a>\n";

			for (auto &subProp : prop.second.subObj.props)
				GenIndex(subProp, subIndent);

			for (auto &subCls : prop.second.subObj.subclassTypes)
			{
				auto PropValLiteral = [&f](const std::string &val, Prop::PrimitiveType type) -> std::string
				{
					if (type == Prop::PrimitiveType::String)
						return "\"" + val + "\"";
					else
						return val;
				};
				f << "\n";
				f << subIndent << "// " + prop.second.subObj.subclassSelectorPropName << "="
					<< PropValLiteral(subCls.first, prop.second.subObj.props.find(prop.second.subObj.subclassSelectorPropName)->second.types.begin()->primitiveType) << "\n";
				for (auto &subProp : subCls.second.props)
					GenIndex(subProp, subIndent);

				if (subCls.second.props.size() == 0)
					f << subIndent << "// (No additional properties)\n";
			}

			if (allArray)
				f << indent << "   }\n"	<< indent << "],\n";
			else
				f << indent << "},\n";
		}
		else
		{
			f << "<span class=\"typeNames\">";
			std::function<void(const std::list<Prop::Type>&)> ListTypeNames = [&f, &ListTypeNames](const std::list<Prop::Type> &types)
			{
				const char *sep = "";
				for (auto &t : types)
				{
					f << sep;
					auto &name = propTypeToNameMap.find(t.primitiveType)->second;
					if (t.primitiveType == Prop::PrimitiveType::Array)
					{
						f << "[";
						ListTypeNames(t.subTypes);
						f << "]";
					}
					else
						f << name;
					sep = "|";
				}

			};
			ListTypeNames(prop.second.types);
			f << "</span>,\n";
		}
	};

	// generate the root object summary
	f << "<a name=\"RootObjectSummary\"></a>\n"
		"<h2>Root object summary</h2>\n"
		"<div class=\"jsonSectIndex\">"
		"{\n";
	for (auto &prop : rootObject.props)
		GenIndex(prop, "     ");
	f << "}\n"
		<< "</div>\n";
	
	// generate the property index
	std::function<void(Object&, std::string, const std::list<std::string>&)> Gen = 
		[this, &f, &Gen, &GenIndex](Object &obj, std::string indent, const std::list<std::string> &path)
	{
		// make the path into a string
		std::string pathStr;
		{
			Object *objEle = &rootObject;
			for (auto &p : path)
			{
				pathStr.append(p);

				const char *pathSep = ".";
				if (auto it = objEle->props.find(p); it != objEle->props.end())
				{
					objEle = &it->second.subObj;
					bool array = false;
					for (auto &t : it->second.types)
					{
						if (t.primitiveType == Prop::PrimitiveType::Array)
							pathSep = "[].";
					}
				}
				pathStr.append(pathSep);
			}
		}

		static auto QuoteHTML = [](std::string &str) -> std::string
		{
			std::string result;
			const char *start = str.c_str(), *p = start;
			for (; *p != 0 ; ++p)
			{
				switch (*p)
				{
				case '&':
				case '<':
				case '>':
					result.append(start, p - start);
					result.append(*p == '&' ? "&amp;" : *p == '<' ? "&lt;" : "&gt;");
					start = p + 1;
					break;
				}
			}
			return result.append(start, p - start);
		};
		static auto ToHTML = [](std::string &str) -> std::string
		{
			if (str == "")
				return "</p><p>";
			else
				return str;
		};
		static auto CleanPara = [](std::list<std::string> &l)
		{
			while (l.size() != 0 && l.back().size() == 0)
				l.pop_back();

			return l;
		};

		// generate properties
		for (auto &prop : obj.props)
		{
			// skip the subclass identifier property - we generate that specially at the end
			if (prop.first == obj.subclassSelectorPropName)
				continue;

			f << indent << "<a name=\"" << prop.second.link << "\"></a>\n";

			auto GenDocText = [&f](std::list<std::string> &text, const std::string &indent)
			{
				bool pre = false;
				f << indent << "<p>\n";
				for (auto &txt : CleanPara(text))
				{
					if (!pre && std::regex_match(txt, std::regex("\\s*<pre>\\s*")))
						pre = true;

					if (pre)
						f << "  " << ToHTML(txt) << "\n";
					else
						f << indent << "  " << ToHTML(txt) << "\n";

					if (pre && std::regex_match(txt, std::regex(".*</pre>")))
						pre = false;

				}
				f << indent << "</p>\n";
			};

			if (prop.second.title.size() != 0)
			{
				// generate the section title
				f << indent << "<div class=\"jsonSectTitle\">" << prop.second.title << "</div>\n";
				GenDocText(prop.second.text, indent);

				// generate the object index
				f << indent << "<div class=\"jsonSectIndexHdr\">Summary</div>\n"
					"<div class=\"jsonSectIndex\">"
					"{\n";
				GenIndex(prop, "   ");
				f << "}\n"
					<< indent << "</div>\n";
			}

			// generate the section example if present
			if (prop.second.example.size() != 0)
			{
				f << indent << "<div class=\"jsonExampleHdr\">Example</div>\n"
					<< indent << "<div class=\"jsonExample\">";
				for (auto &t : prop.second.example)
					f << t << "\n";
				f << indent << "</div>\n";
			}

			f << indent << "<div class=\"jsonProp\">\n"
				<< indent << "  <div class=\"jsonPropHdr\">\n"
				<< indent << "    <span class=\"jsonPropPath\">" << pathStr << "</span><span class=\"jsonPropName\">" << prop.first << "</span>\n"
				<< indent << "    <span class=\"jsonPropType\">";

			std::function<void(const std::list<Prop::Type>&, const char *)> ListTypeNames2 = [&f, &ListTypeNames2](
				const std::list<Prop::Type> &types, const char *sep)
			{
				const char *curSep = "";
				for (auto &t : types)
				{
					f << curSep;
					auto &ptname = propTypeToNameMap.find(t.primitiveType)->second;
					if (t.primitiveType == Prop::PrimitiveType::Array)
					{
						f << "array of ";
						ListTypeNames2(t.subTypes, " or ");
					}
					else
						f << ptname;
					curSep = sep;
				}
			};
			ListTypeNames2(prop.second.types, ", ");
			f << "</span>";

			if (prop.second.required)
				f << "<span class=\"jsonPropRequired\">Required</span>";
			else
				f << "<span class=\"jsonPropOptional\">Optional</span>";

			f << "\n"
				<< indent << "  </div>\n"
				<< indent << "  <div class=\"jsonPropDesc\">\n";

			if (prop.second.enumeratedValues.size() != 0)
			{
				f << indent << "<div class=\"jsonEnumVals\">\n"
					<< indent << "   One of: ";
				const char *sep = "";
				for (auto &v : prop.second.enumeratedValues)
				{
					f << sep << "<span class=\"jsonEnumVal\">" << v << "</span>";
					sep = ", ";
				}
				f << "\n"
					<< indent << "</div>\n";
			}

			if (prop.second.see.size() != 0)
				f << indent << "<div class=\"jsonSee\">\n"
				<< indent << "   See <a href=\"#" << prop.second.see << "\">" << prop.second.see << "</a>\n"
				<< indent << "</div>\n";

			if (prop.second.title.size() == 0)
				GenDocText(prop.second.text, indent);

			f << indent << "  </div>\n";

			std::list<std::string> subPath = path;
			subPath.emplace_back(prop.first);
			Gen(prop.second.subObj, indent + "  ", subPath);

			f << indent << "</div>\n";
		}

		// generate subclasses
		if (obj.subclassSelectorPropName.size() != 0)
		{
			auto itp = obj.props.find(obj.subclassSelectorPropName);
			if (itp == obj.props.end())
			{
				printf("Error: class type property \"%s\" not defined for containing object path \"%s\"\n", obj.subclassSelectorPropName.c_str(), pathStr.c_str());
				exit(1);
			}

			f << indent << "<a name=\"" << itp->second.link << "\"></a>\n"
				<< indent << "<div class=\"jsonProp\">\n"
				<< indent << "  <div class=\"jsonPropHdr\">\n"
				<< indent << "    <span class=\"jsonPropPath\">" << pathStr << "</span><span class=\"jsonPropName\">" << obj.subclassSelectorPropName << "</span>\n"
				<< indent << "  </div>\n"
				<< indent << "  <div class=\"jsonPropDesc\">\n";

			f << indent << "<div class=\"jsonEnumVals\">\n"
				<< indent << "   One of: ";
			const char *sep = "";
			const char *qu = itp->second.types.front().primitiveType == Prop::PrimitiveType::String ? "\"" : "";
			for (auto &s : obj.subclassTypes)
			{
				f << sep << "<span class=\"jsonEnumVal\">" << qu
					<< "<a href=\"#" << itp->second.link << "=" << s.first << "\">"
					<< s.first
					<< "</a>"
					<< qu
					<< "</span>";
				sep = ", ";
			}
			f << "\n"
				<< indent << "</div>\n";

			f << indent << "<p>\n";
			for (auto &txt : CleanPara(itp->second.text))
				f << indent << "  " << ToHTML(txt) << "\n";
			f << indent << "</p>\n";

			f << indent << "  </div>\n";


			for (auto &sc : obj.subclassTypes)
			{
				std::string typePropDesc = obj.subclassSelectorPropName + "=";
				if (itp->second.types.front().primitiveType == Prop::PrimitiveType::String)
					typePropDesc += "\"" + sc.second.classType + "\"";
				else
					typePropDesc += sc.second.classType;
				
				f << indent << "  <a name=\"" << itp->second.link << "=" << sc.first << "\"></a>\n"
					<< indent << "  <div class=\"jsonProp\">\n"
					<< indent << "    <div class=\"jsonPropHdr\">\n"
					<< indent << "      <span class=\"jsonPropPath\">" << pathStr << "</span><span class=\"jsonPropName\">" << typePropDesc << "</span>\n"
					<< indent << "    </div>\n";

				// generate the section example if present
				if (sc.second.classExample.size() != 0)
				{
					f << indent << "<div class=\"jsonExampleHdr\">Example</div>\n"
						<< indent << "<div class=\"jsonExample\">";
					for (auto &t : sc.second.classExample)
						f << t << "\n";
					f << indent << "</div>\n";
				}

				f << indent << "    <div class=\"jsonClassDesc\">\n"
					<< indent << "    <p>\n";
				for (auto &txt : CleanPara(sc.second.classText))
					f << indent << "      " << ToHTML(txt) << "\n";
				f << indent << "    </p>\n"
					<< indent << "    </div>\n";

				Gen(sc.second, indent + "  ", path);

				f << indent << "  </div>\n";
			}
			f << indent << "</div>\n";
		}
	};
	std::list<std::string> emptyPath;
	Gen(rootObject, "", emptyPath);

	// generate the ending boilerplate
	f << "</body>\n"
		"</html>\n";
}

void ConfigDocParser::GenerateSchemaStructs(const char *filename)
{
	// open the file
	std::fstream f(filename, std::ios_base::out);
	if (!f)
	{
		printf("Config Doc Parser: error opening output file \"%s\"\n", filename);
		exit(1);
	}

	// resolve cross-reference objects
	std::function<void(Object*, const std::string&)> ResolveXRefs = [this, &ResolveXRefs](Object *obj, const std::string &path)
	{
		// get the path prefix
		std::string pathPrefix = path;
		if (path.size() != 0)
			pathPrefix += ".";

		// resolve subclass objects
		for (auto &sc : obj->subclassTypes)
			ResolveXRefs(&sc.second, pathPrefix + obj->subclassSelectorPropName + "=\"" + sc.first + "\"");

		// scan the object's properties
		for (auto &pair : obj->props)
		{
			// extract the name/property items from the pair
			auto &propName = pair.first;
			auto &prop = pair.second;

			// resolve the sub-object
			ResolveXRefs(&prop.subObj, pathPrefix + propName);

			// scan the types for this property
			for (auto &type : prop.types)
			{
				// check for a cross reference
				if (type.xref.size() != 0)
				{
					// look up the cross-reference
					Object *refObj = &rootObject;
					for (const char *p = type.xref.c_str(); ; ++p)
					{
						// find the '.'
						const char *subPropName = p;
						for (; *p != 0 && *p != '.' ; ++p) ;

						// look up the property in the current object
						if (auto it = refObj->props.find(std::string(subPropName, p - subPropName)); it != refObj->props.end())
						{
							// got it - traverse into this object
							refObj = &it->second.subObj;
						}
						else
						{
							// lookup failed
							printf("%s: object cross-reference lookup \"%s\" failed\n",
								(pathPrefix + propName).c_str(), type.xref.c_str());
							break;
						}

						// stop at end of string
						if (*p == 0)
						{
							// resolved
							type.xrefObj = refObj;

							// stop searching
							break;
						}
					}
				}
			}
		}
	};
	ResolveXRefs(&rootObject, "");

	// generate a header
	f << "// Pinscape Pico Config Tool - Config file JSON schema description\n"
		<< "// Copyright 2024 Michael J Roberts / BSD-3-Clause license / NO WARRANTY\n"
		<< "// \n"
		<< "// This file was generated mechanically by ConfigDocParser from " << srcfile << ".\n"
		<< "// Hand-editing isn't recommended, since the entire file will be overwritten\n"
		<< "// by future builds.\n"
		<< "//\n"
		<< "// The file defines a series of static structs of types defined in\n"
		<< "// GUIConfigTool\\ConfigEditorWin.cpp.\n"
		<< "\n";

	const static auto Quote = [](const std::string &s)
	{
		std::string result;
		for (char c : s)
		{
			switch (c)
			{
			case '"':
				result.append("\\\"");
				break;

			case '\\':
				result.append("\\\\");
				break;

			case '\n':
				result.append("\\n");
				break;

			case '\r':
				result.append("\\r");
				break;

			default:
				result.append(&c, 1);
				break;
			}
		}
		return result;
	};

	// generate an object struct
	int objNum = 0;
	std::function<void(Object&)> GenObj = [&f, &GenObj, &objNum](Object &obj)
	{
		// do nothing if this object has already been generated
		if (obj.objNum >= 0)
			return;

		// now assign the object, so that we know not to generate recursively
		obj.objNum = objNum++;

		// pre-declare the object, in case of a circular reference
		f << "extern const JSONSchemaObj jsonSchemaObj" << obj.objNum << ";\n";

		// generate sub-objects and cross-reference objects first
		for (auto &pair : obj.props)
		{
			// generate sub-objects
			auto &subObj = pair.second.subObj;
			if (subObj.props.size() != 0)
				GenObj(subObj);

			// generate cross-reference objects
			for (auto &type : pair.second.types)
			{
				if (type.xrefObj != nullptr)
					GenObj(*type.xrefObj);
			}
		}
		for (auto &pair : obj.subclassTypes)
			GenObj(pair.second);

		f << "const JSONSchemaObj jsonSchemaObj" << obj.objNum << "{ ";
		if (obj.subclassSelectorPropName.size() != 0)
			f << "\"" << obj.subclassSelectorPropName << "\", ";
		else
			f << "nullptr, ";
		if (obj.classType.size() != 0)
			f << "\"" << obj.classType << "\", { ";
		else
			f << "nullptr, { ";

		for (auto &pair : obj.subclassTypes)
			f << "&jsonSchemaObj" << pair.second.objNum << ", ";

		f << "}, {\n";
		for (const auto &pair : obj.props)
		{
			auto &name = pair.first;
			auto &prop = pair.second;
			f << "    { \"" << name << "\", \"" << prop.link << "\", "
				<< (prop.required ? "true" : "false") << ", ";

			// build a one-line summary from the documentation text, if nay
			if (prop.text.size() != 0)
			{
				// build the text, up to a limit
				size_t maxLen = 128;
				std::string para;
				for (auto &line : prop.text)
				{
					// stop if we've reached a new paragraph
					if (line.size() == 0)
						break;

					// append a simple horizontal space between blank lines
					if (para.size() != 0) para.append(" ");

					// add the line
					para.append(line);
				}

				// strip HTML tags
				std::string plainPara;
				for (const char *p = para.c_str() ; *p != 0 ; )
				{
					// scan to the next tag
					const char *start = p;
					for (; *p != 0 && *p != '<' ; ++p);

					// append this section
					if (p != start)
						plainPara.append(start, p - start);

					// skip the HTML
					if (*p == '<')
					{
						// If it's <table>, stop here - a table is equivalent to
						// a new paragraph, so it ends the summary header.  (Tables
						// don't work very well as summary data anyway.)
						++p;
						if (_strnicmp(p, "table", 5) == 0 && (isspace(p[5]) || p[5] == '>'))
							break;

						// scan for the end of the HTML tag
						char qu = 0;
						for (; *p != 0 ; ++p)
						{
							if (qu != 0)
							{
								// in quoted text - check for end quote
								if (*p == qu)
									qu = 0;
							}
							else if (*p == '"' || *p == '\'')
							{
								// entering quoted text
								qu = *p;
							}
							else if (*p == '>')
							{
								// end of tag - skip the '>' and stop scanning
								++p;
								break;
							}
						}
					}
				}

				// truncate the result to maxLen
				f << "\"" << Quote(plainPara.substr(0, std::min(plainPara.size(), maxLen)));
				if (plainPara.size() > maxLen) f << "...";
				f << "\", ";
			}
			else
			{
				f << "nullptr, ";
			}

			if (prop.validate.length() != 0)
				f << "\"" << Quote(prop.validate) << "\", ";
			else
				f << "nullptr, ";
			f << " { ";
			const Object *objXRef = nullptr;
			for (auto &t : prop.types)
			{
				f << "{ \"" << propTypeToNameMap.find(t.primitiveType)->second << "\"";
				if (t.primitiveType == Prop::PrimitiveType::Array)
				{
					f << ", { ";
					for (auto &tt : t.subTypes)
						f << "{ \"" << propTypeToNameMap.find(tt.primitiveType)->second << "\" }, ";
					f << "}";
				}
				f << " }, ";

				if (t.primitiveType == Prop::PrimitiveType::Object)
					objXRef = t.xrefObj;
			}
			f << "}, ";

			if (prop.subObj.objNum >= 0)
				f << "&jsonSchemaObj" << prop.subObj.objNum << ", ";
			else
				f << "nullptr, ";

			if (objXRef != nullptr)
				f << "&jsonSchemaObj" << objXRef->objNum << ",";
			else
				f << "nullptr,";

			f << "{ ";
			for (auto &v : prop.enumeratedValues)
			{
				if (v._Starts_with("\""))
					f << v << ", ";
				else
					f << "\"" << v << "\", ";
			}
			f << "}, ";

			f << " }, \n";
		}
		f << "} };\n";
	};
	GenObj(rootObject);
	f << "static const JSONSchemaObj *jsonSchemaRoot = &jsonSchemaObj" << rootObject.objNum << ";\n";
}


int main(int argc, char **argv)
{
	// check arguments
	if (argc != 4)
		return printf("Usage: ConfigDocParser <in> <out-html> <out-schema>\n"), 1;

	printf("Config Doc Parser: %s -> %s, %s\n", argv[1], argv[2], argv[3]);

	// parse the input file
	ConfigDocParser parser;
	parser.ParseFile(argv[1]);

	// generate the HTML documentation file
	parser.GenerateHTML(argv[2]);

	// generate C++ structs
	parser.GenerateSchemaStructs(argv[3]);

	// done
	return 0;
}
