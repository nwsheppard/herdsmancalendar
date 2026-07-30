#include <Preferences.h>

#include "calendar_server_store.h"

namespace {
constexpr const char * kNamespace = "server_cfg";
constexpr const char * kKeyBaseUrl = "base_url";
}

bool calendar_server_url_load(String & out)
{
    Preferences prefs;
    prefs.begin(kNamespace, true);
    out = prefs.getString(kKeyBaseUrl, "");
    prefs.end();
    return out.length() > 0;
}

void calendar_server_url_save(const String & url)
{
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putString(kKeyBaseUrl, url);
    prefs.end();
}
