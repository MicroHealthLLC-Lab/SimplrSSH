/*
 * Storage Menu
 * Everything is saved on the device's internal flash and persists without an
 * SD card. This menu shows what is saved and copies it between the device and
 * the SD card (backup / restore), loads SSH keys from the card, or erases the
 * device's settings.
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "settings_backup.hpp"
#include "sd_card.hpp"
#include <algorithm>
#include <sys/stat.h>

// Backups are written to /simplrssh; call only while the card is mounted
static std::string backup_dir()
{
    return std::string(sdcard::MOUNT_POINT) + "/simplrssh";
}

// Where to read a backup from: /simplrssh, or /pocketssh (made before the rename) if only
// that one has a backup. Call only while the card is mounted.
static std::string restore_dir()
{
    struct stat st;
    std::string legacy = std::string(sdcard::MOUNT_POINT) + "/pocketssh";
    if (stat((backup_dir() + "/settings.dat").c_str(), &st) != 0 &&
        stat((legacy + "/settings.dat").c_str(), &st) == 0) {
        return legacy;
    }
    return backup_dir();
}

static std::string plural(int n, const char* word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

static std::string describe(const settings_backup::Counts& c)
{
    return plural(c.profiles, "profile") + ", " + plural(c.networks, "WiFi network") + ", " +
           plural(c.hosts, "trusted server");
}

static bool yes(const std::string& input, bool default_yes)
{
    std::string a = input;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    return a.empty() ? default_yes : (a == "y" || a == "yes");
}

void SSHTerminal::handle_storage_command(const std::string& command)
{
    size_t space = command.find(' ');
    std::string sub = space == std::string::npos ? "" : command.substr(space + 1);
    sub.erase(0, sub.find_first_not_of(' '));

    wizard_reset();
    if (sub.empty() || sub == "menu") {
        wizard.menu = WizardStep::StorageMenu;
        wizard_goto(WizardStep::StorageMenu);
    } else if (sub == "backup") {
        wizard_goto(WizardStep::StorageConfirmBackup);
    } else if (sub == "restore") {
        wizard_goto(WizardStep::StorageConfirmRestore);
    } else if (sub == "keys") {
        storage_load_keys();
    } else {
        append_text("Usage: storage [backup|restore|keys]\n  'storage' alone opens the menu\n");
    }
}

void SSHTerminal::storage_backup()
{
    append_text("Backing up to SD card...\n");
    refresh_display_now();
    if (sdcard::mount(true) != ESP_OK) {
        append_text("No SD card found.\n");
        return;
    }
    settings_backup::Counts c;
    esp_err_t err = settings_backup::backup(backup_dir(), c);
    sdcard::unmount();

    if (err == ESP_OK) {
        append_text(("Backed up " + describe(c) + " and " + plural(c.vault_files, "saved password") +
                     " (encrypted) to /simplrssh on the SD card.\n").c_str());
    } else {
        append_text("ERROR: Backup failed (card full or write-protected?). The old backup is unchanged.\n");
    }
}

void SSHTerminal::storage_restore()
{
    if (ssh_connected) {
        append_text("Disconnect SSH first ('exit').\n");
        return;
    }
    append_text("Restoring from SD card...\n");
    refresh_display_now();
    if (sdcard::mount() != ESP_OK) {
        append_text("No SD card found.\n");
        return;
    }
    settings_backup::Counts c;
    esp_err_t err = settings_backup::restore(restore_dir(), c);
    sdcard::unmount();

    if (err == ESP_ERR_INVALID_STATE) {
        append_text("No usable backup on the SD card - nothing was changed.\n");
        return;
    }
    reload_saved_settings();
    if (err == ESP_OK) {
        append_text(("Restored " + describe(c) + ".\n").c_str());
    } else {
        append_text("WARNING: Restore was incomplete; check 'profile list' and 'wifi list'.\n");
    }
    if (vault_was_reset) {
        append_text("The backup's saved passwords belong to another device; they'll be asked\n"
                    "for once and saved again.\n");
    } else if (vault::has_pin()) {
        append_text("Saved passwords use the PIN that was set when the backup was made.\n");
    }
}

void SSHTerminal::storage_load_keys()
{
    append_text("Reading SSH keys from SD card...\n");
    refresh_display_now();
    if (sdcard::mount() != ESP_OK) {
        append_text("No SD card found.\n");
        return;
    }
    int loaded = sdcard::load_ssh_keys(this);
    sdcard::unmount();
    append_text(("Loaded " + std::to_string(loaded) + " key(s) from /ssh_keys. " +
                 std::to_string(loaded_keys.size()) + " available.\n").c_str());
}

// Re-reads everything stored in NVS / the vault after a restore or erase
void SSHTerminal::reload_saved_settings()
{
    vault_was_reset = vault::init();
    profiles = profile_store::load();
    saved_networks = network_store::load();
    if (vault_was_reset) {
        clear_all_saved_secret_flags();  // Those passwords are gone; ask for them again
    }
    command_history.clear();
    history_index = -1;
    history_needs_save = false;
    load_history_from_nvs();
}

bool SSHTerminal::storage_step_prompt()
{
    ui::Page p;
    p.title = "Storage";
    switch (wizard.step) {
        case WizardStep::StorageMenu: {
            settings_backup::Counts device = settings_backup::count_device();
            p.info = "This device: " + describe(device) + "\nSaved passwords: " + std::to_string(device.vault_files) +
                     (vault::has_pin() ? " (PIN)" : "") + ", SSH keys: " + std::to_string(loaded_keys.size()) + "\n";

            refresh_display_now();
            if (sdcard::mount() == ESP_OK) {
                settings_backup::Counts sd;
                bool has_backup = settings_backup::read_backup_counts(restore_dir(), sd);
                sdcard::unmount();
                p.info += has_backup ? "SD card backup: " + describe(sd) : std::string("SD card: no backup yet");
            } else {
                p.info += "SD card: not inserted";
            }

            p.rows = {
                ui::row("1", "Back up to SD card", LV_SYMBOL_UPLOAD),
                ui::row("2", "Restore from SD card", LV_SYMBOL_DOWNLOAD),
                ui::row("3", "Load SSH keys from SD card", LV_SYMBOL_SD_CARD),
                ui::row("4", "Erase all settings", LV_SYMBOL_TRASH),
                ui::back_row(),
            };
            p.rows[3].tone = ui::Tone::Danger;
            break;
        }
        case WizardStep::StorageConfirmBackup:
            p.info = "Copy this device's settings to the SD card?\nAn older backup is replaced.";
            p.rows = { ui::row("y", "Back up", LV_SYMBOL_UPLOAD), ui::row("n", "Cancel", LV_SYMBOL_CLOSE) };
            p.selected = 0;   // "y"
            break;
        case WizardStep::StorageConfirmRestore:
            p.info = "Replace this device's settings with the SD card backup?";
            p.rows = { ui::row("y", "Restore", LV_SYMBOL_DOWNLOAD), ui::row("n", "Keep them", LV_SYMBOL_CLOSE) };
            p.rows[0].tone = ui::Tone::Danger;
            p.selected = 1;   // "n"
            break;
        case WizardStep::StorageConfirmErase:
            p.info = "Erase ALL profiles, networks, trusted servers, history and saved passwords?";
            p.rows = { ui::row("y", "Erase everything", LV_SYMBOL_TRASH), ui::row("n", "Keep them", LV_SYMBOL_CLOSE) };
            p.rows[0].tone = ui::Tone::Danger;
            p.selected = 1;   // "n"
            break;
        default:
            return false;
    }
    show_page(p);
    return true;
}

bool SSHTerminal::storage_step_input(const std::string& raw_input, const std::string& input)
{
    (void)raw_input;
    switch (wizard.step) {
        case WizardStep::StorageMenu:
            if (input == "1") {
                wizard_goto(WizardStep::StorageConfirmBackup);
            } else if (input == "2") {
                wizard_goto(WizardStep::StorageConfirmRestore);
            } else if (input == "3") {
                storage_load_keys();
                wizard_prompt();
            } else if (input == "4") {
                wizard_goto(WizardStep::StorageConfirmErase);
            } else if (input == "0") {
                go_home();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;

        case WizardStep::StorageConfirmBackup:
            if (yes(input, true)) {
                storage_backup();
            } else {
                append_text("Not backed up.\n");
            }
            wizard_done();
            return true;

        case WizardStep::StorageConfirmRestore:
            if (yes(input, false)) {
                storage_restore();
            } else {
                append_text("Nothing changed.\n");
            }
            wizard_done();
            return true;

        case WizardStep::StorageConfirmErase:
            if (yes(input, false)) {
                settings_backup::erase_device();
                reload_saved_settings();
                append_text("All settings on this device were erased.\n");
            } else {
                append_text("Nothing erased.\n");
            }
            wizard_done();
            return true;

        default:
            return false;
    }
}
