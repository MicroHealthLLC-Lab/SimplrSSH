/*
 * Connection Profile Menu
 * Listing, adding, editing, deleting and connecting with saved SSH profiles. Each step shows
 * a page (show_page(): a menu of rows, a form, or a question answered on the input line);
 * rows are picked with the trackball / arrow keys and Enter, by touch, or by typing their
 * number. Adding walks the fields once; the Review form then changes any one of them.
 *
 * This file also holds the wizard plumbing shared by every menu (home, WiFi, sign-in page,
 * ChatGPT, storage, security).
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <algorithm>

static const size_t MAX_NAME_LEN = 32;
static const size_t MAX_FIELD_LEN = 64;

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(' ');
    return s.substr(start, end - start + 1);
}

static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool is_number(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
}

// ---------------------------------------------------------------------------
// Shared wizard plumbing
// ---------------------------------------------------------------------------

bool SSHTerminal::wizard_input_masked() const
{
    if (wizard.step == WizardStep::PortalField) {
        return portal_field_masked();
    }
    switch (wizard.step) {
        case WizardStep::Password:
        case WizardStep::KeyPassphrase:
        case WizardStep::ConnectSecret:
        case WizardStep::SshKeyPassphrase:
        case WizardStep::WifiPassword:
        case WizardStep::VaultUnlock:
        case WizardStep::VaultOldPin:
        case WizardStep::VaultNewPin:
        case WizardStep::VaultConfirmPin:
        case WizardStep::ChatKey:
            return true;
        default:
            return false;
    }
}

std::string SSHTerminal::input_display_text() const
{
    if (wizard_input_masked()) {
        return std::string(current_input.length(), '*');
    }
    return current_input;
}

void SSHTerminal::wizard_goto(WizardStep step)
{
    wizard.step = step;
    wizard_prompt();
}

void SSHTerminal::wizard_prompt()
{
    if (profile_step_prompt() || wifi_step_prompt() || vault_step_prompt() || host_step_prompt() ||
        storage_step_prompt() || home_step_prompt() || chat_step_prompt() || portal_step_prompt()) {
        return;
    }
}

void SSHTerminal::wizard_handle_input(const std::string& raw_input)
{
    notice.clear();   // The next page shows what this answer printed
    const std::string input = trim(raw_input);

    if (!wizard_input_masked() && to_lower(input) == "cancel") {
        append_text("Cancelled.\n");
        if (wizard_holds_connection()) {
            ssh_teardown();  // Close the half-open connection waiting for trust or a passphrase
        }
        go_home();
        return;
    }

    if (profile_step_input(raw_input, input) || wifi_step_input(raw_input, input) ||
        vault_step_input(raw_input, input) || host_step_input(raw_input, input) ||
        storage_step_input(raw_input, input) || home_step_input(raw_input, input) ||
        chat_step_input(raw_input, input) || portal_step_input(raw_input, input)) {
        return;
    }
}

// Trackball left/right (Tab5: Up/Down) in a menu: the previous / next row
void SSHTerminal::wizard_cycle_choice(int direction)
{
    if (page_active) {
        page_move(direction);
    }
}

// Finish the current action: back to the menu it was started from, or home
void SSHTerminal::wizard_done()
{
    WizardStep menu = wizard.menu;
    wizard_reset();
    if (menu != WizardStep::None) {
        wizard.menu = menu;
        wizard_goto(menu);
    } else {
        go_home();
    }
}

void SSHTerminal::wizard_reset()
{
    vault::wipe(wizard.secret);
    vault::wipe(wizard.pin);
    wizard = ProfileWizard();
    portal_wipe();   // Leaving a sign-in page ends it
    hide_page();
}

// ---------------------------------------------------------------------------
// Connection profiles
// ---------------------------------------------------------------------------

// PEM keys say so when they have a passphrase ("Proc-Type: 4,ENCRYPTED", "BEGIN ENCRYPTED")
bool SSHTerminal::key_is_encrypted(const std::string& key_name)
{
    const char* key = get_loaded_key(key_name.c_str(), NULL);
    return key && strstr(key, "ENCRYPTED") != NULL;
}

// "BEGIN OPENSSH PRIVATE KEY": ssh-keygen's default format, which libssh2 can't read with mbed TLS
bool SSHTerminal::key_is_openssh_format(const std::string& key_name)
{
    const char* key = get_loaded_key(key_name.c_str(), NULL);
    return key && strstr(key, "OPENSSH PRIVATE KEY") != NULL;
}

// Steps that wait while an SSH connection is open: cancelling them closes it
bool SSHTerminal::wizard_holds_connection() const
{
    return wizard.step == WizardStep::HostTrust || wizard.step == WizardStep::SshKeyPassphrase;
}

std::string SSHTerminal::profile_summary(const ConnectionProfile& p, bool secret_saved)
{
    std::string s = p.username + "@" + p.host;
    if (p.port != 22) {
        s += ":" + std::to_string(p.port);
    }
    if (p.auth == ConnectionProfile::Auth::Key) {
        s += " [key " + p.key_name;
        if (key_is_encrypted(p.key_name) || secret_saved) {
            s += secret_saved ? ", pass saved" : ", pass: ask";
        }
        s += "]";
    } else {
        s += secret_saved ? " [pw: saved]" : " [pw: ask]";
    }
    return s;
}

void SSHTerminal::handle_profile_command(const std::string& command)
{
    // command is "profile[s] [sub] [ref...]"
    size_t first_space = command.find(' ');
    std::string rest = first_space == std::string::npos ? "" : trim(command.substr(first_space));
    size_t space = rest.find(' ');
    std::string sub = to_lower(rest.substr(0, space));
    std::string ref = space == std::string::npos ? "" : trim(rest.substr(space + 1));

    wizard_reset();

    if (sub.empty() || sub == "menu") {
        wizard.menu = WizardStep::MainMenu;
        wizard_goto(WizardStep::MainMenu);
    } else if (sub == "list" || sub == "ls") {
        list_profiles();
    } else if (sub == "add" || sub == "new") {
        begin_profile_action(WizardAction::Add, ref);
    } else if (sub == "edit") {
        begin_profile_action(WizardAction::Edit, ref);
    } else if (sub == "delete" || sub == "del" || sub == "rm") {
        begin_profile_action(WizardAction::Delete, ref);
    } else if (sub == "connect" || sub == "go") {
        begin_profile_action(WizardAction::Connect, ref);
    } else {
        append_text("Usage: profile [list|add|edit|delete|connect] [NAME|#]\n");
        append_text("  'profile' alone opens the menu\n");
    }
}

void SSHTerminal::list_profiles()
{
    if (profiles.empty()) {
        append_text("No saved profiles. Use 'profile add' to create one.\n");
        return;
    }

    append_text("Saved profiles:\n");
    for (size_t i = 0; i < profiles.size(); i++) {
        std::string line = " " + std::to_string(i + 1) + ") " + profiles[i].name +
                           "  " + profile_summary(profiles[i], profiles[i].secret_saved) + "\n";
        append_text(line.c_str());
    }
}

int SSHTerminal::find_profile(const std::string& ref)
{
    if (is_number(ref)) {
        int n = std::atoi(ref.c_str());
        return (n >= 1 && n <= (int)profiles.size()) ? n - 1 : -1;
    }

    std::string wanted = to_lower(ref);
    for (size_t i = 0; i < profiles.size(); i++) {
        if (to_lower(profiles[i].name) == wanted) {
            return i;
        }
    }
    return -1;
}

void SSHTerminal::begin_profile_action(WizardAction action, const std::string& ref)
{
    wizard.action = action;

    if (action == WizardAction::Add) {
        if (profiles.size() >= PROFILE_MAX_COUNT) {
            append_text("Profile limit reached. Delete a profile first.\n");
            wizard_done();
            return;
        }
        wizard.draft = ConnectionProfile();
        if (!loaded_keys.empty()) {
            wizard.draft.auth = ConnectionProfile::Auth::Key;
        }
        wizard.index = -1;
        wizard.editing = false;
        wizard_goto(WizardStep::Name);
        return;
    }

    if (profiles.empty()) {
        append_text("No saved profiles. Use 'profile add' to create one.\n");
        wizard_done();
        return;
    }

    if (!ref.empty()) {
        int index = find_profile(ref);
        if (index < 0) {
            append_text("Profile not found: ");
            append_text(ref.c_str());
            append_text("\n");
            list_profiles();
            wizard_done();
            return;
        }
        on_profile_picked(index);
        return;
    }

    wizard_goto(WizardStep::PickProfile);
}

void SSHTerminal::on_profile_picked(int index)
{
    wizard.index = index;
    const ConnectionProfile& p = profiles[index];

    switch (wizard.action) {
        case WizardAction::Connect:
            start_profile_connect(index);
            break;
        case WizardAction::Edit:
            wizard.draft = p;
            wizard.editing = true;
            wizard.secret_change = SecretChange::Keep;
            wizard_goto(WizardStep::Review);   // The form: pick the fields to change
            break;
        case WizardAction::Delete:
            wizard_goto(WizardStep::ConfirmDelete);
            break;
        default:
            wizard_done();
            break;
    }
}

// Title of the profile pages: the server being edited, or a new one
static std::string profile_page_title(const std::vector<ConnectionProfile>& profiles, int index)
{
    if (index >= 0 && index < (int)profiles.size()) {
        return "Edit " + profiles[index].name;
    }
    return "New server";
}

// Adding a server walks the fields once: "Step n of 5" (sign-in is one step, however many
// questions it takes). Fields changed from the Review form show no step.
static std::string add_step_info(bool adding, int step)
{
    return adding ? "Step " + std::to_string(step) + " of 5" : "";
}

bool SSHTerminal::profile_step_prompt()
{
    const ConnectionProfile& d = wizard.draft;
    const bool can_keep_secret = d.secret_saved && wizard.secret_change == SecretChange::Keep;
    const bool adding = wizard.action == WizardAction::Add && !wizard.form_return;
    const bool is_key = d.auth == ConnectionProfile::Auth::Key;
    ui::Page pg;

    switch (wizard.step) {
        case WizardStep::MainMenu:
            pg.title = "SSH servers";
            pg.info = profiles.empty() ? "No servers yet" : std::to_string(profiles.size()) + " saved";
            pg.rows = {
                ui::row("1", "Connect", LV_SYMBOL_PLAY, "", ui::Mark::More),
                ui::row("2", "List", LV_SYMBOL_LIST, "", ui::Mark::More),
                ui::row("3", "Add", LV_SYMBOL_PLUS),
                ui::row("4", "Edit", LV_SYMBOL_EDIT, "", ui::Mark::More),
                ui::row("5", "Delete", LV_SYMBOL_TRASH, "", ui::Mark::More),
                ui::back_row(),
            };
            pg.rows[4].tone = ui::Tone::Danger;
            if (profiles.empty()) {
                pg.selected = 2;   // Add
            }
            break;

        case WizardStep::PickProfile:
            // Connect, Edit, Delete; no action: the read-only list (menu 2)
            pg.title = wizard.action == WizardAction::Connect ? "Connect to"
                     : wizard.action == WizardAction::Edit ? "Edit server"
                     : wizard.action == WizardAction::Delete ? "Delete server" : "Saved servers";
            for (size_t i = 0; i < profiles.size(); i++) {
                pg.rows.push_back(ui::row(std::to_string(i + 1), profiles[i].name, LV_SYMBOL_DRIVE,
                                          profile_summary(profiles[i], profiles[i].secret_saved)));
            }
            pg.rows.push_back(ui::back_row());
            break;

        case WizardStep::Name:
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 1);
            pg.question = "Name for this server";
            pg.hint = wizard.editing ? "Enter = keep " + d.name : "Up to 32 characters, not only digits";
            break;

        case WizardStep::Host:
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 2);
            pg.question = "Host name or IP address";
            pg.hint = d.host.empty() ? "" : "Enter = keep " + d.host;
            break;

        case WizardStep::Port:
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 3);
            pg.question = "Port";
            pg.hint = "Enter = keep " + std::to_string(d.port);
            break;

        case WizardStep::User:
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 4);
            pg.question = "Username";
            pg.hint = d.username.empty() ? "" : "Enter = keep " + d.username;
            break;

        case WizardStep::AuthMethod: {
            size_t key_count = loaded_keys.size();
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 5);
            if (!pg.info.empty()) {
                pg.info += "\n";
            }
            pg.info += "Sign in with";
            pg.rows = {
                ui::row("1", "SSH key from SD card", LV_SYMBOL_SD_CARD,
                        key_count ? std::to_string(key_count) + (key_count == 1 ? " key" : " keys") : "none loaded",
                        is_key ? ui::Mark::Chosen : ui::Mark::NotChosen),
                ui::row("2", "Password", LV_SYMBOL_KEYBOARD, "",
                        is_key ? ui::Mark::NotChosen : ui::Mark::Chosen),
            };
            pg.selected = is_key ? 0 : 1;
            break;
        }

        case WizardStep::PickKey: {
            auto keys = get_loaded_key_names();
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 5);
            if (!pg.info.empty()) {
                pg.info += "\n";
            }
            pg.info += "SSH keys on the SD card";
            for (size_t i = 0; i < keys.size(); i++) {
                bool current = keys[i] == to_lower(d.key_name);
                pg.rows.push_back(ui::row(std::to_string(i + 1), keys[i], LV_SYMBOL_FILE,
                                          key_is_openssh_format(keys[i]) ? "OpenSSH format: convert to PEM"
                                          : key_is_encrypted(keys[i]) ? "passphrase" : "",
                                          current ? ui::Mark::Chosen : ui::Mark::NotChosen));
                if (current) {
                    pg.selected = i;
                }
            }
            pg.rows.push_back(ui::back_row());
            break;
        }

        case WizardStep::Password:
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 5);
            pg.question = "Password for " + d.username + "@" + d.host;
            pg.hint = can_keep_secret ? "Enter = keep saved, - = ask each connect" : "Enter = ask each connect";
            break;

        case WizardStep::KeyPassphrase:
            pg.title = profile_page_title(profiles, wizard.index);
            pg.info = add_step_info(adding, 5);
            // Offered for every key: a key that doesn't say it has one may still have one
            if (key_is_encrypted(d.key_name)) {
                pg.question = "Passphrase for key " + d.key_name;
                pg.hint = can_keep_secret ? "Enter = keep saved, - = ask each connect" : "Enter = ask each connect";
            } else {
                pg.question = "Passphrase for key " + d.key_name + ", if it has one";
                pg.hint = can_keep_secret ? "Enter = keep saved, - = none" : "Enter = none";
            }
            break;

        case WizardStep::Review: {
            // The form: one row per field (pick one to change just that), then the actions
            wizard.form_return = false;
            bool saved = wizard.secret_change == SecretChange::Set || can_keep_secret;
            std::string secret_status = saved ? "saved"
                                      : (!is_key || key_is_encrypted(d.key_name)) ? "ask each connect" : "none";
            pg.title = profile_page_title(profiles, wizard.index);
            if (wizard.secret_change == SecretChange::Set) {
                pg.info = is_key ? "Passphrase will be stored encrypted" : "Password will be stored encrypted";
            }
            pg.rows = {
                ui::row("3", "Name", LV_SYMBOL_EDIT, d.name),
                ui::row("4", "Host", LV_SYMBOL_DRIVE, d.host),
                ui::row("5", "Port", LV_SYMBOL_SETTINGS, std::to_string(d.port)),
                ui::row("6", "User", LV_SYMBOL_HOME, d.username),
                ui::row("7", "Sign-in", LV_SYMBOL_SD_CARD, is_key ? "Key " + d.key_name : "Password"),
                ui::row("8", is_key ? "Passphrase" : "Password", LV_SYMBOL_EYE_CLOSE, secret_status),
                ui::row("1", "Save", LV_SYMBOL_SAVE),
                ui::row("2", "Save and connect", LV_SYMBOL_OK),
                ui::back_row("Discard"),
            };
            if (!is_key) {
                pg.rows[4].icon = LV_SYMBOL_KEYBOARD;
            }
            pg.selected = 6;   // Save
            break;
        }

        case WizardStep::ConfirmDelete: {
            const ConnectionProfile& p = profiles[wizard.index];
            pg.title = "Delete server";
            pg.info = "Delete '" + p.name + "'?\n" + profile_summary(p, p.secret_saved);
            pg.rows = {
                ui::row("y", "Delete", LV_SYMBOL_TRASH),
                ui::row("n", "Keep it", LV_SYMBOL_CLOSE),
            };
            pg.rows[0].tone = ui::Tone::Danger;
            pg.selected = 1;
            break;
        }

        case WizardStep::ConnectSecret: {
            const ConnectionProfile& p = profiles[wizard.index];
            pg.title = "Connect";
            pg.info = p.name;
            pg.question = p.auth == ConnectionProfile::Auth::Key
                        ? "Passphrase for key " + p.key_name
                        : "Password for " + p.username + "@" + p.host;
            pg.hint = "Enter = cancel";
            break;
        }

        case WizardStep::SshKeyPassphrase:
            pg.title = "Connect";
            pg.info = pending_ssh.username + "@" + pending_ssh.host;
            pg.question = "Key passphrase";
            pg.hint = "Enter = cancel";
            break;

        default:
            return false;
    }

    show_page(pg);
    return true;
}

bool SSHTerminal::profile_step_input(const std::string& raw_input, const std::string& input)
{
    ConnectionProfile& d = wizard.draft;
    const bool can_keep_secret = d.secret_saved && wizard.secret_change == SecretChange::Keep;
    // After a field's answer: the next field, or back to the Review form it was picked from
    auto next_field = [this](WizardStep next) { return wizard.form_return ? WizardStep::Review : next; };

    switch (wizard.step) {
        case WizardStep::MainMenu:
            if (input == "1") {
                begin_profile_action(WizardAction::Connect, "");
            } else if (input == "2") {
                if (profiles.empty()) {
                    append_text("No saved profiles. Use 'profile add' to create one.\n");
                    wizard_prompt();
                } else {
                    wizard.action = WizardAction::None;   // Read-only list: picking returns here
                    wizard_goto(WizardStep::PickProfile);
                }
            } else if (input == "3") {
                begin_profile_action(WizardAction::Add, "");
            } else if (input == "4") {
                begin_profile_action(WizardAction::Edit, "");
            } else if (input == "5") {
                begin_profile_action(WizardAction::Delete, "");
            } else if (input == "0") {
                go_home();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            break;

        case WizardStep::PickProfile: {
            if (input == "0") {
                wizard_done();
                break;
            }
            int index = input.empty() ? -1 : find_profile(input);
            if (index < 0) {
                append_text("Invalid choice.\n");
                wizard_prompt();
            } else {
                on_profile_picked(index);
            }
            break;
        }

        case WizardStep::Name: {
            if (input.empty() && wizard.editing) {
                wizard_goto(next_field(WizardStep::Host));
                break;
            }
            const char* error = NULL;
            if (input.empty()) {
                error = "Name is required.\n";
            } else if (is_number(input)) {
                error = "Name can't be only digits (numbers select from lists).\n";
            } else if (input.length() > MAX_NAME_LEN) {
                error = "Name is too long (max 32).\n";
            } else {
                int existing = find_profile(input);
                if (existing >= 0 && existing != wizard.index) {
                    error = "A profile with that name already exists.\n";
                }
            }
            if (error) {
                append_text(error);
                wizard_prompt();
            } else {
                d.name = input;
                wizard_goto(next_field(WizardStep::Host));
            }
            break;
        }

        case WizardStep::Host:
            if (input.empty() && !d.host.empty()) {
                wizard_goto(next_field(WizardStep::Port));
            } else if (input.empty() || input.find(' ') != std::string::npos || input.length() > MAX_FIELD_LEN) {
                append_text("Enter a host name or IP address.\n");
                wizard_prompt();
            } else {
                d.host = input;
                wizard_goto(next_field(WizardStep::Port));
            }
            break;

        case WizardStep::Port:
            if (input.empty()) {
                wizard_goto(next_field(WizardStep::User));
            } else if (!is_number(input) || input.length() > 5 ||
                       std::atoi(input.c_str()) < 1 || std::atoi(input.c_str()) > 65535) {
                append_text("Port must be 1-65535.\n");
                wizard_prompt();
            } else {
                d.port = std::atoi(input.c_str());
                wizard_goto(next_field(WizardStep::User));
            }
            break;

        case WizardStep::User:
            if (input.empty() && !d.username.empty()) {
                wizard_goto(next_field(WizardStep::AuthMethod));
            } else if (input.empty() || input.find(' ') != std::string::npos || input.length() > MAX_FIELD_LEN) {
                append_text("Enter a username.\n");
                wizard_prompt();
            } else {
                d.username = input;
                wizard_goto(next_field(WizardStep::AuthMethod));
            }
            break;

        case WizardStep::AuthMethod: {
            std::string choice = input;
            if (choice.empty()) {
                choice = d.auth == ConnectionProfile::Auth::Key ? "1" : "2";
            }
            ConnectionProfile::Auth chosen;
            if (choice == "1") {
                if (loaded_keys.empty()) {
                    append_text("No SSH keys loaded. Put .pem files in /sdcard/ssh_keys/ and reboot.\n");
                    wizard_prompt();
                    break;
                }
                chosen = ConnectionProfile::Auth::Key;
            } else if (choice == "2") {
                chosen = ConnectionProfile::Auth::Password;
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
                break;
            }
            if (chosen != d.auth) {
                // A saved password is not a key passphrase (or vice versa)
                wizard.secret_change = SecretChange::Clear;
                vault::wipe(wizard.secret);
                d.auth = chosen;
            }
            wizard_goto(chosen == ConnectionProfile::Auth::Key ? WizardStep::PickKey : WizardStep::Password);
            break;
        }

        case WizardStep::PickKey: {
            if (input == "0") {
                wizard_goto(WizardStep::AuthMethod);
                break;
            }
            auto keys = get_loaded_key_names();
            std::string selected;
            if (input.empty()) {
                if (get_loaded_key(d.key_name.c_str(), NULL)) {
                    selected = to_lower(d.key_name);
                }
            } else if (is_number(input)) {
                int n = std::atoi(input.c_str());
                if (n >= 1 && n <= (int)keys.size()) {
                    selected = keys[n - 1];
                }
            }
            if (selected.empty()) {
                append_text("Invalid choice.\n");
                wizard_prompt();
                break;
            }
            if (selected != to_lower(d.key_name)) {
                // Different key: a saved passphrase belongs to the old one
                wizard.secret_change = SecretChange::Clear;
                vault::wipe(wizard.secret);
            }
            d.key_name = selected;
            if (key_is_openssh_format(selected)) {
                append_text("Note: OpenSSH-format keys can't be used yet. Convert it to PEM on your\n"
                            "computer: ssh-keygen -p -m PEM -f KEYFILE\n");
            }
            wizard_goto(WizardStep::KeyPassphrase);
            break;
        }

        case WizardStep::Password:
        case WizardStep::KeyPassphrase:
            // Not trimmed: spaces can be part of a password
            vault::wipe(wizard.secret);
            if (raw_input == "-") {
                wizard.secret_change = SecretChange::Clear;
            } else if (!raw_input.empty()) {
                wizard.secret_change = SecretChange::Set;
                wizard.secret = raw_input;
            } else if (!can_keep_secret) {
                wizard.secret_change = SecretChange::Clear;
            }
            wizard_goto(WizardStep::Review);
            break;

        case WizardStep::Review:
            if (input.empty() || input == "1") {
                save_profile_draft(false);
            } else if (input == "2") {
                save_profile_draft(true);
            } else if (input.length() == 1 && input[0] >= '3' && input[0] <= '8') {
                // A field row: change just that field (current values as defaults), then back here
                static const WizardStep fields[] = {WizardStep::Name, WizardStep::Host, WizardStep::Port,
                                                    WizardStep::User, WizardStep::AuthMethod, WizardStep::Password};
                WizardStep field = fields[input[0] - '3'];
                if (field == WizardStep::Password && d.auth == ConnectionProfile::Auth::Key) {
                    field = WizardStep::KeyPassphrase;
                }
                wizard.editing = true;
                wizard.form_return = true;
                wizard_goto(field);
            } else if (input == "0") {
                append_text("Discarded.\n");
                wizard_done();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            break;

        case WizardStep::ConfirmDelete:
            if (to_lower(input) == "y" || to_lower(input) == "yes") {
                ConnectionProfile removed = profiles[wizard.index];
                profiles.erase(profiles.begin() + wizard.index);
                vault::remove(removed.secret_id());
                if (profile_store::save(profiles) == ESP_OK) {
                    append_text(("Deleted '" + removed.name + "'.\n").c_str());
                } else {
                    append_text("ERROR: Failed to save profiles to flash.\n");
                }
            } else {
                append_text("Not deleted.\n");
            }
            wizard_done();
            break;

        case WizardStep::SshKeyPassphrase:
            wizard_reset();
            if (raw_input.empty()) {
                append_text("Cancelled.\n");
                ssh_teardown();
                go_home();
            } else {
                pending_ssh.passphrase = raw_input;   // Wiped once used (ssh_finish)
                ssh_finish();
            }
            break;

        case WizardStep::ConnectSecret: {
            ConnectionProfile chosen = profiles[wizard.index];
            std::string secret = raw_input;
            wizard_reset();
            if (secret.empty()) {
                append_text("Cancelled.\n");
            } else {
                connect_profile(chosen, secret);
            }
            vault::wipe(secret);
            break;
        }

        default:
            return false;
    }
    return true;
}

void SSHTerminal::save_profile_draft(bool connect_after)
{
    if (wizard.secret_change == SecretChange::Set) {
        // Storing a password needs the vault unlocked (or created)
        with_vault([this, connect_after]() { commit_profile_draft(connect_after); });
    } else {
        commit_profile_draft(connect_after);
    }
}

void SSHTerminal::commit_profile_draft(bool connect_after)
{
    ConnectionProfile p = wizard.draft;
    if (p.id.empty()) {
        p.id = profile_store::new_id();
    }

    if (wizard.secret_change == SecretChange::Set) {
        if (vault::put(p.secret_id(), wizard.secret) == ESP_OK) {
            p.secret_saved = true;
        } else {
            append_text("ERROR: Could not store the password; it will be asked each connect.\n");
            p.secret_saved = false;
        }
    } else if (wizard.secret_change == SecretChange::Clear) {
        vault::remove(p.secret_id());
        p.secret_saved = false;
    }

    int index = wizard.index;
    if (index < 0) {
        profiles.push_back(p);
        index = profiles.size() - 1;
    } else {
        profiles[index] = p;
    }

    if (profile_store::save(profiles) == ESP_OK) {
        append_text(("Profile '" + p.name + "' saved.\n").c_str());
    } else {
        append_text("ERROR: Failed to save profiles to flash.\n");
    }

    if (connect_after) {
        wizard_reset();
        start_profile_connect(index);
    } else {
        wizard_done();
    }
}

// Connects with a profile, fetching the saved secret or asking for it
void SSHTerminal::start_profile_connect(int index)
{
    const ConnectionProfile p = profiles[index];

    if (ssh_connected) {
        append_text("Already connected. Type 'exit' to disconnect first.\n");
        wizard_done();   // Back to the menu, the message in its notice
        return;
    }
    if (!wifi_connected) {
        append_text("WiFi not connected. Type 'wifi' to pick a network.\n");
        wizard_done();   // Back to the menu, the message in its notice
        return;
    }
    if (p.auth == ConnectionProfile::Auth::Key && !get_loaded_key(p.key_name.c_str(), NULL)) {
        append_text(("ERROR: Key '" + p.key_name + "' is not on the SD card.\n").c_str());
        append_text("Use 'profile edit' to pick another key.\n");
        wizard_done();   // Back to the menu, the message in its notice
        return;
    }

    // A key's passphrase: when the key says it has one, or one was saved with the profile
    bool needs_secret = p.auth == ConnectionProfile::Auth::Password || key_is_encrypted(p.key_name) ||
                        p.secret_saved;
    if (!needs_secret) {
        wizard_reset();
        connect_profile(p, "");
        return;
    }

    wizard.action = WizardAction::Connect;
    wizard.index = index;
    if (!p.secret_saved) {
        wizard_goto(WizardStep::ConnectSecret);
        return;
    }

    with_vault([this]() {
        ConnectionProfile chosen = profiles[wizard.index];
        std::string secret;
        if (vault::get(chosen.secret_id(), secret) == ESP_OK) {
            wizard_reset();
            connect_profile(chosen, secret);
            vault::wipe(secret);
        } else {
            append_text("Saved password could not be read; enter it now.\n");
            wizard_goto(WizardStep::ConnectSecret);
        }
    });
}

void SSHTerminal::connect_profile(const ConnectionProfile& profile, const std::string& secret)
{
    append_text(("Profile '" + profile.name + "'\n").c_str());
    refresh_display_now();

    if (profile.auth == ConnectionProfile::Auth::Key) {
        size_t key_len = 0;
        const char* key_data = get_loaded_key(profile.key_name.c_str(), &key_len);
        if (!key_data || key_len == 0) {
            append_text(("ERROR: Key '" + profile.key_name + "' is not on the SD card.\n").c_str());
            return;
        }
        connect_with_key(profile.host.c_str(), profile.port, profile.username.c_str(), key_data, key_len,
                         secret.empty() ? NULL : secret.c_str());
    } else {
        connect(profile.host.c_str(), profile.port, profile.username.c_str(), secret.c_str());
    }
}
