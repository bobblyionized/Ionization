#pragma once
#include <string>

// Plain HTTPS GET (WinHTTP, system proxy). Used for weather and place search (Open-Meteo, no key).
bool HttpGetText(const std::wstring& url, std::string& body, std::string& err);
std::wstring UrlEncode(const std::string& utf8);
