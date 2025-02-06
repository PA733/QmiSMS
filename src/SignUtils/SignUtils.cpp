#include "SignUtils.hpp"
#include <cctype>
#include <cppcodec/base64_default_rfc4648.hpp>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sstream>

namespace {
std::string url_encode(const std::string &value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped << c;
    } else {
      escaped << '%' << std::setw(2) << std::uppercase << int(c);
    }
  }
  return escaped.str();
}
} // namespace

std::string generateSign(const std::string &timestamp,
                         const std::string &secret) {
  std::string signStr = timestamp + "\n" + secret;

  unsigned int len = 0;
  unsigned char *hmac_result = HMAC(
      EVP_sha256(), reinterpret_cast<const unsigned char *>(secret.data()),
      secret.size(), reinterpret_cast<const unsigned char *>(signStr.data()),
      signStr.size(), nullptr, &len);
  if (!hmac_result)
    return "";

  std::string b64Sign = cppcodec::base64_rfc4648::encode(hmac_result, len);
  return url_encode(b64Sign);
}

bool validateSign(const std::string &timestamp, const std::string &sign,
                  const std::string &secret) {
  std::string expected = generateSign(timestamp, secret);
  return (expected == sign);
}