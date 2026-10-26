/*
 * Home Menu
 * The start screen, and where 'menu' / 'exit' / every "0) Back" lead. Numbers
 * open an app (7: the console); anything else typed is run as a command. Also the Security
 * menu and the Power menu (sleep, turn off; carried out by keypad_task, deck_base.cpp).
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "board.hpp"
#include "portal.hpp"
#include "esp_timer.h"

void SSHTerminal::go_home()
{
    if (chat_active) {
        close_chat();
    }
    wizard_reset();
    if (ssh_connected) {
        append_text("SSH is connected - type 'exit' to close it and go to the menu.\n");
        return;
    }
    wizard.menu = WizardStep::HomeMenu;
    wizard_goto(WizardStep::HomeMenu);
}

void SSHTerminal::leave_to_home()
{
    vault::wipe(current_input);   // A half-typed line may be a password
    cursor_pos = 0;
    history_index = -1;
    if (chat_active) {
        close_chat();
    }
    if (session || ssh_socket >= 0) {
        disconnect();             // Also a connection waiting for trust or a passphrase
    }
    append_text("\nBack to the menu.\n");
    go_home();
    update_input_display();
}

// After a command started from a menu: return to that menu when it finishes
void SSHTerminal::show_menu_after(WizardStep menu)
{
    wizard.menu = menu;
    if (!wizard_active()) {
        wizard_goto(menu);
    }
}

void SSHTerminal::background_tick()
{
    if (!ssh_connected) {
        show_term_view(false);   // The session ended (by the server or the side panel)
    }
    if (home_pending) {
        home_pending = false;
        go_home();
    }
    wifi_maintain();
    dictate_tick();   // Words spoken in an SSH session go to the cursor
    sync_view();   // Ends a progress page left by a blocking step
}


bool SSHTerminal::home_step_prompt()
{
    switch (wizard.step) {
        case WizardStep::HomeMenu: {
            ui::Page p;
            p.title = "SimplrSSH";
            p.rows = {
                ui::row("1", "WiFi", LV_SYMBOL_WIFI, wifi_connected ? portal::printable(wifi_ssid, 32) : std::string("Off"), ui::Mark::More),
                ui::row("2", "SSH servers", LV_SYMBOL_DRIVE, std::to_string(profiles.size()) + " saved",
                        ui::Mark::More),
                ui::row("3", "ChatGPT", LV_SYMBOL_ENVELOPE, chat_has_key() ? chat_model : std::string("No key"),
                        ui::Mark::More),
                ui::row("4", "Storage", LV_SYMBOL_SD_CARD, "", ui::Mark::More),
                ui::row("5", "Security", LV_SYMBOL_EYE_CLOSE, vault::has_pin() ? "PIN" : "No PIN", ui::Mark::More),
                ui::row("6", "Power", LV_SYMBOL_POWER, "", ui::Mark::More),
                ui::row("7", "Console", LV_SYMBOL_KEYBOARD, "type commands"),
            };
            show_page(p);
            return true;
        }
        case WizardStep::SecurityMenu: {
            ui::Page p;
            p.title = "Security";
            p.info = "Saved passwords: " + std::to_string(vault::count()) +
                     (vault::has_pin() ? " (PIN protected)" : " (no PIN)");
            if (vault::has_pin()) {
                p.rows = {
                    ui::row("1", "Change PIN", LV_SYMBOL_EDIT),
                    ui::row("2", "Remove PIN", LV_SYMBOL_EYE_OPEN),
                };
            } else {
                p.rows = { ui::row("1", "Set a PIN", LV_SYMBOL_EYE_CLOSE) };
            }
            p.rows.push_back(ui::row("3", "Trusted servers", LV_SYMBOL_LIST));
            p.rows.push_back(ui::row("4", "Erase saved passwords", LV_SYMBOL_TRASH));
            p.rows.back().tone = ui::Tone::Danger;
            p.rows.push_back(ui::back_row());
            show_page(p);
            return true;
        }
        case WizardStep::PowerMenu: {
            ui::Page p;
            p.title = "Power";
            p.rows = {
                ui::row("1", "Sleep", LV_SYMBOL_PAUSE, std::string("wake: ") + board::HINT_WAKE),
                ui::row("2", "Turn off", LV_SYMBOL_POWER),
                ui::back_row(),
            };
            show_page(p);
            return true;
        }
        default:
            return false;
    }
}

bool SSHTerminal::home_step_input(const std::string& raw_input, const std::string& input)
{
    switch (wizard.step) {
        case WizardStep::HomeMenu:
            if (input == "1") {
                handle_wifi_command("wifi");
            } else if (input == "2") {
                handle_profile_command("profile");
            } else if (input == "3") {
                handle_chat_command("chat settings");
            } else if (input == "4") {
                handle_storage_command("storage");
            } else if (input == "5") {
                wizard_reset();
                wizard.menu = WizardStep::SecurityMenu;
                wizard_goto(WizardStep::SecurityMenu);
            } else if (input == "6") {
                wizard_reset();
                wizard.menu = WizardStep::PowerMenu;
                wizard_goto(WizardStep::PowerMenu);
            } else if (input == "7") {
                wizard_reset();   // No page: the console (log) shows
                append_text("Console: type a command ('help' lists them); 'menu' returns.\n");
            } else if (input.empty()) {
                wizard_prompt();
            } else {
                wizard_reset();
                execute_command(raw_input);
            }
            return true;

        case WizardStep::SecurityMenu:
            if (input == "1") {
                handle_vault_command("vault pin");
                show_menu_after(WizardStep::SecurityMenu);
            } else if (input == "2" && vault::has_pin()) {
                handle_vault_command("vault nopin");
                show_menu_after(WizardStep::SecurityMenu);
            } else if (input == "3") {
                wizard_reset();
                wizard.menu = WizardStep::HostList;   // Back here after forgetting one
                wizard_goto(WizardStep::HostList);
            } else if (input == "4") {
                handle_vault_command("vault reset");
                show_menu_after(WizardStep::SecurityMenu);
            } else if (input == "0") {
                go_home();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;

        case WizardStep::PowerMenu:
            if (input == "1") {
                request_power(PowerRequest::Sleep);
            } else if (input == "2") {
                request_power(PowerRequest::Off);
            } else if (input == "0") {
                go_home();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;

        default:
            return false;
    }
}

// Sleep / power off: recorded here, carried out by keypad_task once this input is handled
void SSHTerminal::request_power(PowerRequest request)
{
    wizard_reset();
    if (ssh_connected || session) {
        append_text("SSH is connected - type 'exit' to close it first.\n");
        return;
    }
    power_request = request;
}

SSHTerminal::PowerRequest SSHTerminal::take_power_request()
{
    PowerRequest request = power_request;
    power_request = PowerRequest::None;
    return request;
}

// Before sleeping or turning off: nothing left running, unsaved history saved, WiFi off.
// With a PIN, the vault locks (like a computer going to sleep); without one nothing is asked.
void SSHTerminal::power_prepare(PowerRequest request)
{
    if (chat_active) {
        close_chat();
    }
    wizard_reset();
    if (history_needs_save) {
        history_needs_save = false;
        save_history_to_nvs();
    }
    wifi_sleep();
    if (vault::has_pin()) {
        vault::lock();
    }
    if (request == PowerRequest::Off) {
        append_text((std::string("\nTurning off. ") + board::HINT_POWER_ON + "\n").c_str());
    } else {
        append_text((std::string("\nSleeping. Wake it with ") + board::HINT_WAKE + ".\n").c_str());
    }
    refresh_display_now();
}

// Awake again: WiFi reconnects as at startup (unless the user had turned it off), then the menu
void SSHTerminal::power_resume()
{
    last_input_ms = esp_timer_get_time() / 1000;
    append_text("Awake.\n");
    if (wifi_auto_enabled) {
        wifi_retry_delay_s = 0;
        wifi_next_retry_ms = last_input_ms + 30000;
        wifi_auto_connect(false);
    }
    if (!wizard_active()) {   // Not waiting for a PIN
        go_home();
    }
}

// The device stayed on after 'shutdown' (Tab5 on USB power): it sleeps instead
void SSHTerminal::power_off_failed()
{
    append_text((std::string("Still powered (USB keeps it on?) - sleeping instead. Wake it with ") +
                 board::HINT_WAKE + ".\n").c_str());
    refresh_display_now();
}
