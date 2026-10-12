/*
 * WiFi Sign-in Page (captive portal)
 * Cafes, hotels and airports often let a new device online only after it accepts terms or
 * fills in a form on a web page. 'portal' (WiFi menu: Sign in) loads that page and shows it
 * as a form: tick checkboxes, fill in fields, pick from lists, press its buttons or
 * follow its links. No scripts run, so pages that need them won't work; a phone hotspot does.
 *
 * Runs only when the user asks. Pages load in a short-lived worker task (portal_client.cpp);
 * the page, what was typed into it and its cookies stay in RAM and are wiped when it ends.
 */

#include "ssh_terminal.hpp"
#include "portal_client.hpp"
#include "secret_vault.hpp"
#include "esp_log.h"
#include "esp_netif.h"
#include "bsp/esp-bsp.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

static const char *TAG = "PORTAL_MENU";

// Row highlighted when the page is shown again (the item just used), -1 = the first
static int portal_selected = -1;

struct SSHTerminal::PortalJob {
    SSHTerminal* terminal;
    uint32_t generation;
    bool has_request;          // false: probe (start, or check the connection)
    portal::Request request;
    std::string gateway;       // The network's router, asked if the probe's name doesn't resolve
    portal::Cookies cookies;   // A copy: the worker never touches the menu's state
    portal::Result result;
    esp_err_t err = ESP_FAIL;
};

static bool is_number(const std::string& s)
{
    return !s.empty() && s.size() <= 3 && std::all_of(s.begin(), s.end(), ::isdigit);
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool is_https(const std::string& url)
{
    return url.compare(0, 6, "https:") == 0;
}

// Text cut to `max` characters for a title or a row's detail
static std::string shorten(const std::string& s, size_t max)
{
    return s.size() <= max ? s : s.substr(0, max - 3) + "...";
}

// One thing on the page (field, button, link) as a form row
static ui::Row item_row(const portal::Page& page, const portal::Item& item, const std::string& key)
{
    switch (item.kind) {
        case portal::Item::SubmitForm:
            return ui::row(key, page.forms.size() > 1 ? "Send form " + std::to_string(item.index + 1) : "Send",
                           LV_SYMBOL_OK);
        case portal::Item::Redirect: {
            portal::Url u;
            return ui::row(key, "Continue to " + (portal::parse_url(page.redirect, u) ? u.host : "the next page"),
                           LV_SYMBOL_RIGHT);
        }
        case portal::Item::LinkItem: {
            const std::string& text = page.links[item.index].label;
            return ui::row(key, text.empty() ? "Link" : text, LV_SYMBOL_RIGHT, "link");
        }
        case portal::Item::FieldItem:
            break;
    }
    const portal::Field& f = page.fields[item.index];
    std::string label = f.label + (f.required ? " *" : "");
    switch (f.type) {
        case portal::FieldType::Checkbox:
            return ui::row(key, label, nullptr, "", f.checked ? ui::Mark::Checked : ui::Mark::Unchecked);
        case portal::FieldType::Radio:
            return ui::row(key, label, nullptr, "", f.checked ? ui::Mark::Chosen : ui::Mark::NotChosen);
        case portal::FieldType::Select:
            return ui::row(key, label, LV_SYMBOL_LIST,
                           f.options.empty() ? "(no choices)" : shorten(f.options[f.selected].label, 30),
                           ui::Mark::More);
        case portal::FieldType::Submit:
        case portal::FieldType::Image:
            return ui::row(key, f.label, LV_SYMBOL_OK);
        case portal::FieldType::Password:
            return ui::row(key, label, LV_SYMBOL_EYE_CLOSE,
                           f.value.empty() ? "(empty)" : std::string(std::min<size_t>(f.value.size(), 12), '*'));
        default: {
            std::string value = shorten(portal::printable(f.value, 40), 30);
            return ui::row(key, label, LV_SYMBOL_EDIT, value.empty() ? "(empty)" : value);
        }
    }
}

// http://<gateway>/ - many portals answer there when their DNS gives nothing before sign-in
static std::string gateway_url()
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.gw.addr == 0) {
        return "";
    }
    char ip[16];
    esp_ip4addr_ntoa(&info.gw, ip, sizeof(ip));
    portal::Url url;
    url.host = ip;
    url.path = "/";
    return url.str();
}

void SSHTerminal::portal_wipe()
{
    portal::wipe(portal_page);
    portal_cookies.wipe();
    portal_field = -1;
    portal_selected = -1;
    portal_generation = portal_generation + 1;   // A page still loading is dropped when it arrives
}

bool SSHTerminal::portal_field_masked() const
{
    return portal_field >= 0 && (size_t)portal_field < portal_page.fields.size() &&
           portal_page.fields[portal_field].type == portal::FieldType::Password;
}

void SSHTerminal::handle_portal_command(const std::string& command)
{
    (void)command;
    wizard_reset();
    if (ssh_connected || session) {
        append_text("SSH is connected - type 'exit' to close it first.\n");
        return;
    }
    if (!wifi_connected) {
        append_text("Connect to the network first (WiFi menu), then open its sign-in page.\n");
        return;
    }
    if (portal_busy || chat_busy || chat_models_busy) {
        append_text("Still finishing the last request - try again in a moment.\n");
        return;
    }
    append_text("Opening the network's sign-in page (scripts don't run; forms, checkboxes\n"
                "and links do).\n");
    portal_load(NULL);
}

// Starts the worker: the probe (request NULL) or a page/form request
void SSHTerminal::portal_load(const portal::Request* request)
{
    if (portal_busy) {
        append_text("Still loading - one moment.\n");
        wizard_prompt();
        return;
    }
    PortalJob* job = new PortalJob();
    job->terminal = this;
    job->generation = portal_generation;
    job->has_request = request != NULL;
    if (request) {
        job->request = *request;
    } else {
        job->gateway = gateway_url();
    }
    job->cookies = portal_cookies;
    portal_busy = true;
    wizard_goto(WizardStep::PortalWait);
    refresh_display_now();
    if (xTaskCreate(portal_worker, "portal", 10240, job, 4, NULL) != pdPASS) {
        portal::wipe(job->request);
        job->cookies.wipe();
        delete job;
        portal_busy = false;
        append_text("Not enough memory right now - try again.\n");
        if (portal_page.url.empty()) {
            wizard_done();
        } else {
            wizard_goto(WizardStep::PortalPage);
        }
    }
}

void SSHTerminal::portal_worker(void* param)
{
    PortalJob* job = (PortalJob*)param;
    SSHTerminal* t = job->terminal;
    if (!job->has_request) {
        job->err = portal::probe(job->cookies, job->result);
        if (job->err != ESP_OK && !job->gateway.empty()) {
            portal::Request router;
            router.url = job->gateway;
            portal::Result page;
            if (portal::fetch(router, job->cookies, page) == ESP_OK) {
                portal::wipe(job->result.page);
                job->result = page;
                job->err = ESP_OK;
            }
            portal::wipe(page.page);
        }
    } else {
        job->err = portal::fetch(job->request, job->cookies, job->result);
        if (job->err == ESP_OK) {
            // Sent: is the internet reachable now? (The answer page is shown if not)
            portal::Result check;
            if (portal::probe(job->cookies, check) == ESP_OK && check.online) {
                job->result.online = true;
            }
            portal::wipe(check.page);
        }
    }
    if (bsp_display_lock(5000)) {
        t->portal_done(*job);
        bsp_display_unlock();
    }
    portal::wipe(job->request);
    portal::wipe(job->result.page);
    job->cookies.wipe();
    delete job;
    t->portal_busy = false;
    vTaskDelete(NULL);
}

// Display lock held (worker task)
void SSHTerminal::portal_done(PortalJob& job)
{
    if (job.generation != portal_generation || wizard.step != WizardStep::PortalWait) {
        return;   // The sign-in was left while loading
    }
    std::swap(portal_cookies, job.cookies);
    if (job.result.online) {
        append_text(job.has_request ? "Signed in: the internet is reachable now.\n"
                                    : "No sign-in needed: the internet is reachable.\n");
        wizard_done();
        return;
    }
    if (job.err != ESP_OK) {
        append_text(("Couldn't load the page: " + job.result.error + "\n").c_str());
        if (portal_page.url.empty()) {
            append_text("If the network needs a sign-in, try again with 'portal', or sign in on a phone\n"
                        "and share its connection (hotspot).\n");
            wizard_done();
        } else {
            wizard_goto(WizardStep::PortalPage);
        }
        return;
    }
    portal::wipe(portal_page);
    std::swap(portal_page, job.result.page);
    portal_selected = -1;
    portal_field = -1;
    ESP_LOGI(TAG, "Sign-in page shown");
    wizard_goto(WizardStep::PortalPage);
}

// Sends form `form`, pressed with button `submitter` (-1: the form has no button)
void SSHTerminal::portal_send(int form, int submitter)
{
    std::string missing = portal::missing_required(portal_page, form);
    if (!missing.empty()) {
        append_text(("Fill in first: " + missing + "\n").c_str());
        wizard_prompt();
        return;
    }
    portal::Request request = portal::submit(portal_page, form, submitter);
    if (request.url.empty()) {
        append_text("This form can't be sent from here.\n");
        wizard_prompt();
        return;
    }
    append_text("Sending...\n");
    portal_load(&request);
    portal::wipe(request);
}

void SSHTerminal::portal_activate(const portal::Item& item)
{
    portal::Request request;
    switch (item.kind) {
        case portal::Item::SubmitForm:
            portal_send(item.index, -1);
            return;
        case portal::Item::Redirect:
            request.url = portal_page.redirect;
            break;
        case portal::Item::LinkItem:
            request.url = portal_page.links[item.index].url;
            break;
        case portal::Item::FieldItem: {
            portal::Field& f = portal_page.fields[item.index];
            switch (f.type) {
                case portal::FieldType::Checkbox:
                case portal::FieldType::Radio:
                    portal::toggle(portal_page, item.index);
                    wizard_prompt();
                    return;
                case portal::FieldType::Select:
                    portal_field = item.index;
                    wizard_goto(WizardStep::PortalOption);
                    return;
                case portal::FieldType::Submit:
                case portal::FieldType::Image:
                    portal_send(f.form, item.index);
                    return;
                default:
                    portal_field = item.index;
                    wizard_goto(WizardStep::PortalField);
                    return;
            }
        }
    }
    request.referer = portal_page.url;
    append_text("Loading...\n");
    portal_load(&request);
}

bool SSHTerminal::portal_step_prompt()
{
    ui::Page p;
    p.title = "Sign in";

    switch (wizard.step) {
        case WizardStep::PortalWait:
            p.info = "Loading the page...";
            p.hint = "Esc cancels";
            break;

        case WizardStep::PortalPage: {
            const portal::Page& page = portal_page;
            portal::Url url;
            portal::parse_url(page.url, url);
            if (!page.title.empty()) {
                p.title = shorten(page.title, 20);
            }
            p.info = "From " + url.host + (url.https ? "" : " (not encrypted)");
            if (page.status >= 400 && page.status != 511) {
                p.info += "\nThe page answered with error " + std::to_string(page.status) + ".";
            }
            if (!page.text.empty()) {
                p.info += "\n\n" + page.text;
            }
            std::vector<portal::Item> items = portal::items(page);
            if (items.empty()) {
                p.info += "\n\nNothing to fill in or press here: the page probably needs scripts.\n"
                          "Sign in on a phone and share its connection (hotspot) instead.";
            }
            for (size_t i = 0; i < items.size(); i++) {
                p.rows.push_back(item_row(page, items[i], std::to_string(i + 1)));
            }
            p.rows.push_back(ui::row("r", "Start again / check connection", LV_SYMBOL_REFRESH));
            p.rows.push_back(ui::back_row());
            if (portal_selected >= 0 && (size_t)portal_selected < items.size()) {
                p.selected = portal_selected;   // Stay on the box just ticked or field just filled
            }
            break;
        }

        case WizardStep::PortalField: {
            const portal::Field& f = portal_page.fields[portal_field];
            bool secret = f.type == portal::FieldType::Password;
            if (secret && !is_https(portal_page.url)) {
                p.info = "This page isn't encrypted - others on this network could see what you type here.";
            }
            p.question = f.label + (f.required ? " *" : "");
            std::string current = secret ? "" : shorten(portal::printable(f.value, 40), 30);
            p.hint = (current.empty() ? "" : "Now: " + current + ". ") + "Enter = keep, - = clear";
            break;
        }

        case WizardStep::PortalOption: {
            const portal::Field& f = portal_page.fields[portal_field];
            p.title = shorten(f.label, 20);
            for (size_t i = 0; i < f.options.size(); i++) {
                p.rows.push_back(ui::row(std::to_string(i + 1), f.options[i].label, nullptr, "",
                                         i == f.selected ? ui::Mark::Chosen : ui::Mark::NotChosen));
            }
            p.rows.push_back(ui::back_row());
            p.selected = f.selected < f.options.size() ? (int)f.selected : 0;
            break;
        }

        default:
            return false;
    }

    show_page(p);
    return true;
}

bool SSHTerminal::portal_step_input(const std::string& raw_input, const std::string& input)
{
    switch (wizard.step) {
        case WizardStep::PortalWait:
            append_text("Still loading - one moment (Esc cancels).\n");
            return true;

        case WizardStep::PortalPage: {
            std::string choice = lower(input);
            std::vector<portal::Item> items = portal::items(portal_page);
            int n = is_number(choice) ? atoi(choice.c_str()) : -1;
            if (choice == "0") {
                wizard_done();
            } else if (choice == "r") {
                portal_load(NULL);
            } else if (n >= 1 && (size_t)n <= items.size()) {
                portal_selected = n - 1;
                portal_activate(items[n - 1]);
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;
        }

        case WizardStep::PortalField: {
            portal::Field& f = portal_page.fields[portal_field];
            if (input == "-") {
                portal::wipe(f.value);
            } else if (raw_input.size() > portal::MAX_VALUE) {
                append_text(("At most " + std::to_string(portal::MAX_VALUE) + " characters.\n").c_str());
                wizard_prompt();
                return true;
            } else if (!raw_input.empty()) {
                portal::wipe(f.value);
                f.value = f.type == portal::FieldType::Password ? raw_input : input;
            }
            portal_field = -1;
            wizard_goto(WizardStep::PortalPage);
            return true;
        }

        case WizardStep::PortalOption: {
            portal::Field& f = portal_page.fields[portal_field];
            int n = is_number(input) ? atoi(input.c_str()) : -1;
            if (n >= 1 && (size_t)n <= f.options.size()) {
                f.selected = n - 1;
            } else if (n != 0) {
                append_text("Invalid choice.\n");
                wizard_prompt();
                return true;
            }
            portal_field = -1;
            wizard_goto(WizardStep::PortalPage);
            return true;
        }

        default:
            return false;
    }
}
