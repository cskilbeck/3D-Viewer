#pragma once

#include <filesystem>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////

extern char const *app_name;
extern char const *settings_filename;
extern char const *app_friendly_name;

//////////////////////////////////////////////////////////////////////

std::filesystem::path config_path(std::string const &application_name, std::string const &filename);

// the user's home directory (%USERPROFILE% on Windows, $HOME elsewhere)
std::filesystem::path home_path();

// ~/3DViewer.settings
std::filesystem::path settings_path();

// ImGui helpers
bool IconCheckbox(const char *label, bool *v, const char *icon_on, const char *icon_off);
bool IconCheckboxTristate(const char *label, int *v, const char *icon_on, const char *icon_off, const char *icon_mixed);
bool IconButton(const char *label, const char *icon);
void RightAlignButtons(const std::vector<const char *> &labels, bool align_to_content = false);
int MsgBox(char const *banner, char const *text, char const *yes_text = "Yes", char const *no_text = "No");

// A row of buttons, one per option, the current one highlighted. label (if not empty)
// goes after it like other widgets. Returns true if value changed
bool SegmentedControl(char const *label, int *value, std::vector<char const *> const &options, uint32_t disabled = 0);

// two choice version for a bool (options[0] = false, options[1] = true)
bool SegmentedControl(char const *label, bool *value, char const *off_option, char const *on_option);

static double get_time()
{
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}
