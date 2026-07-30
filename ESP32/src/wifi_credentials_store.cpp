#include <Preferences.h>

#include "wifi_credentials_store.h"

namespace {
constexpr const char * kNamespace = "wifi_cfg";
constexpr const char * kKeySsid = "ssid";
constexpr const char * kKeyPassword = "password";
}

bool wifi_credentials_load(String & ssid, String & password)
{
    Preferences prefs;
    prefs.begin(kNamespace, true);
    ssid = prefs.getString(kKeySsid, "");
    password = prefs.getString(kKeyPassword, "");
    prefs.end();
    return ssid.length() > 0;
}

void wifi_credentials_save(const String & ssid, const String & password)
{
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putString(kKeySsid, ssid);
    prefs.putString(kKeyPassword, password);
    prefs.end();
}

void wifi_credentials_clear()
{
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.clear();
    prefs.end();
}
