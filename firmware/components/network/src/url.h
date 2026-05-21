// SPDX-License-Identifier: Apache-2.0
//
// URL guard helpers used by the HTTPS client. Split out so host tests
// can validate the policy without standing up esp_http_client.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns true iff `url` starts with the literal prefix "https://" and
// the host portion is non-empty. We refuse plain `http://` everywhere
// in PagerOS: SPEC §9.3 / §6.3 require TLS to App Servers, the bundled
// CA roots are only useful for HTTPS, and per docs/spec/threat-model.md
// §S6 a downgrade to cleartext would compromise the request-signing
// guarantee.
bool pageros_url_is_https(const char *url);

#ifdef __cplusplus
}
#endif
