#ifndef SIGN_UTILS_HPP
#define SIGN_UTILS_HPP

#include <string>

bool validateSign(const std::string &timestamp, const std::string &sign, const std::string &secret);
std::string generateSign(const std::string &timestamp, const std::string &secret);

#endif // SIGN_UTILS_HPP