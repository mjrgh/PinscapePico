// Pinscape Pico - Utilities
// Copyright 2024, 2025 Michael J Roberts / BSD-3-Clause license / NO WARRANTY
//

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <functional>
#include <string>
#include <regex>
#include <stdarg.h>
#include <wchar.h>

// -------------------------------------------------------------------------
//
// sprintf-style format to a std::string
//
std::string StrPrintf(const char *fmt, ...);
std::string StrPrintfV(const char *fmt, va_list va);

std::basic_string<wchar_t> StrPrintf(const wchar_t *fmt, ...);
std::basic_string<wchar_t> StrPrintfV(const wchar_t *fmt, va_list va);

// printf format string for TCHAR
#ifdef UNICODE
#define TCHAR_FMT "%ws"
#else
#define TCHAR_FMT "%hs"
#endif

// -------------------------------------------------------------------------
//
// Regex replace with callback
//
// The callback function has this form:
//
//  [captures](const std::match_results<StringType::const_iterator> &m) -> StringType
//
// where StringType is the suitable TSTRING, CSTRING, or WSTRING type
// matching the source string type.
//

template<class BidirIt, class Traits, class CharT, class UnaryFunction>
std::basic_string<CharT> regex_replace(BidirIt first, BidirIt last,
	const std::basic_regex<CharT, Traits>& re, UnaryFunction f)
{
	std::basic_string<CharT> s;

	typename std::match_results<BidirIt>::difference_type
		positionOfLastMatch = 0;
	auto endOfLastMatch = first;

	auto callback = [&](const std::match_results<BidirIt>& match)
	{
		auto positionOfThisMatch = match.position(0);
		auto diff = positionOfThisMatch - positionOfLastMatch;

		auto startOfThisMatch = endOfLastMatch;
		std::advance(startOfThisMatch, diff);

		s.append(endOfLastMatch, startOfThisMatch);
		s.append(f(match));

		auto lengthOfMatch = match.length(0);

		positionOfLastMatch = positionOfThisMatch + lengthOfMatch;

		endOfLastMatch = startOfThisMatch;
		std::advance(endOfLastMatch, lengthOfMatch);
	};

	std::regex_iterator<BidirIt> begin(first, last, re), end;
	std::for_each(begin, end, callback);

	s.append(endOfLastMatch, last);

	return s;
}

template<class Traits, class CharT, class UnaryFunction>
std::basic_string<CharT> regex_replace(
	const std::basic_string<CharT>& s,
	const std::basic_regex<CharT, Traits>& re,
	UnaryFunction f)
{
	return regex_replace(s.cbegin(), s.cend(), re, f);
}
