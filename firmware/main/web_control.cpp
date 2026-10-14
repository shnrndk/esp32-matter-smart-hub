#include "web_control.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <esp_matter_controller_client.h>
#include <app/BufferedReadCallback.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <controller/InvokeInteraction.h>
#include <platform/CHIPDeviceLayer.h>
#include <app-common/zap-generated/cluster-objects.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
namespace {
constexpr const char *TAG = "web_control";
struct Bulb {
    uint64_t node;
    uint16_t endpoint = 0;
    int power = -1, brightness = -1, kelvin = -1, hue = -1, saturation = -1, mode = -1;
    int min_mired = 0, max_mired = 0, color_caps = 0;
    bool level = false, color = false, online = false;
    char error[128] = "Not checked yet";
};
Bulb bulbs[2] = {{1}, {2}};
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
bool busy = false;
uint32_t operation = 0;
char operation_error[128] = "";
extern const uint8_t page_start[] asm("_binary_index_html_start");
extern const uint8_t page_end[] asm("_binary_index_html_end");

// Each request owns the asynchronous CASE callbacks and read paths until done.
// One outstanding request bounds heap use and keeps slider requests ordered.
class Request final : public ReadClient::Callback {
public:
    Request(int bulb, int action, int value, int saturation)
      : index(bulb), action(action), value(value), saturation(saturation), buffered(*this),
        connected(on_connected, this), failed(on_failed, this) {
        portENTER_CRITICAL(&mux); result = bulbs[index]; portEXIT_CRITICAL(&mux);
    }
    static void start(intptr_t arg) {
        auto *r = reinterpret_cast<Request *>(arg);
        auto *controller = esp_matter::controller::matter_controller_client::get_instance().get_commissioner();
        auto err = controller->GetConnectedDevice(r->result.node, &r->connected, &r->failed);
        if (err != CHIP_NO_ERROR) r->finish(err);
    }
private:
    int index, action, value, saturation;
    Bulb result;
    BufferedReadCallback buffered;
    chip::Callback::Callback<OnDeviceConnected> connected;
    chip::Callback::Callback<OnDeviceConnectionFailure> failed;
    ReadClient *client = nullptr;
    AttributePathParams paths[10];
    SessionHolder session;
    Messaging::ExchangeManager *exchange = nullptr;
    bool discovery = false, got_power = false;
    CHIP_ERROR read_error = CHIP_NO_ERROR;
    int64_t settle_until = 0;

    bool at_target() const {
        if (action == 1) return result.power == value;
        if (action == 2) return result.power == 1 && result.brightness > 0 && std::abs(result.brightness - value) <= 1;
        if (action == 3) return result.mode == 2 && std::abs(result.kelvin - value) <= 15;
        if (action == 4) {
            if (result.hue < 0 || result.saturation < 0) return false;
            int difference = std::abs(result.hue - value);
            difference = std::min(difference, 360 - difference);
            return (result.mode == 0 || result.mode == 1) && difference <= 2 && std::abs(result.saturation - saturation) <= 1;
        }
        return true;
    }

    void finish(CHIP_ERROR error) {
        result.online = error == CHIP_NO_ERROR;
        snprintf(result.error, sizeof(result.error), "%s", error == CHIP_NO_ERROR ? "" : ErrorStr(error));
        portENTER_CRITICAL(&mux);
        bulbs[index] = result;
        snprintf(operation_error, sizeof(operation_error), "%s", result.error);
        busy = false;
        portEXIT_CRITICAL(&mux);
        Platform::Delete(this);
    }
    static void on_failed(void *arg, const ScopedNodeId &, CHIP_ERROR error) {
        static_cast<Request *>(arg)->finish(error);
    }
    static void on_connected(void *arg, Messaging::ExchangeManager &mgr, const SessionHandle &handle) {
        auto *r = static_cast<Request *>(arg);
        r->exchange = &mgr; r->session.Grab(handle);
        if (!r->result.endpoint) r->read(true);
        else if (r->action == 0) r->read(false);
        else r->invoke();
    }
    void read(bool discover) {
        if (!session) { finish(CHIP_ERROR_INCORRECT_STATE); return; }
        discovery = discover; got_power = false; read_error = CHIP_NO_ERROR;
        size_t count = 0;
        if (discover) paths[count++] = AttributePathParams(kInvalidEndpointId, Descriptor::Id, 1);
        else {
            auto ep = result.endpoint;
            paths[count++] = AttributePathParams(ep, OnOff::Id, 0);
            if (result.level) paths[count++] = AttributePathParams(ep, LevelControl::Id, 0);
            if (result.color) for (auto id : {0u, 1u, 7u, 8u, 0x400au, 0x400bu, 0x400cu})
                paths[count++] = AttributePathParams(ep, ColorControl::Id, id);
        }
        client = Platform::New<ReadClient>(InteractionModelEngine::GetInstance(), exchange, buffered, ReadClient::InteractionType::Read);
        if (!client) { finish(CHIP_ERROR_NO_MEMORY); return; }
        ReadPrepareParams params(session.Get().Value());
        params.mpAttributePathParamsList = paths; params.mAttributePathParamsListSize = count;
        auto err = client->SendRequest(params);
        if (err != CHIP_NO_ERROR) { Platform::Delete(client); client = nullptr; finish(err); }
    }
    void OnAttributeData(const ConcreteDataAttributePath &path, TLV::TLVReader *data, const StatusIB &status) override {
        if (status.ToChipError() != CHIP_NO_ERROR || !data) {
            if (path.mClusterId == OnOff::Id) read_error = status.ToChipError();
            return;
        }
        if (discovery) {
            if (path.mEndpointId == 0 || (result.endpoint && result.endpoint <= path.mEndpointId)) return;
            TLV::TLVType container;
            if (data->EnterContainer(container) != CHIP_NO_ERROR) return;
            bool on = false, level = false, color = false;
            while (data->Next() == CHIP_NO_ERROR) {
                uint32_t cluster = 0;
                if (data->Get(cluster) != CHIP_NO_ERROR) continue;
                on |= cluster == OnOff::Id; level |= cluster == LevelControl::Id; color |= cluster == ColorControl::Id;
            }
            data->ExitContainer(container);
            if (on) { result.endpoint = path.mEndpointId; result.level = level; result.color = color; }
            return;
        }
        if (path.mClusterId == OnOff::Id) {
            bool power;
            if (data->Get(power) == CHIP_NO_ERROR) { result.power = power; got_power = true; }
            return;
        }
        uint32_t number = 0;
        if (data->Get(number) != CHIP_NO_ERROR) return; // nullable values stay unknown
        if (path.mClusterId == LevelControl::Id) result.brightness = lround(number * 100.0 / 254.0);
        else switch (path.mAttributeId) {
            case 0: result.hue = lround(number * 360.0 / 254.0); break;
            case 1: result.saturation = lround(number * 100.0 / 254.0); break;
            case 7: result.kelvin = number ? lround(1000000.0 / number) : -1; break;
            case 8: result.mode = number; break;
            case 0x400a: result.color_caps = number; break;
            // Reserved values (e.g. Sengled's 0xFFFF) are unknown limits.
            case 0x400b: result.min_mired = number && number < 0xff00 ? number : 0; break;
            case 0x400c: result.max_mired = number && number < 0xff00 ? number : 0; break;
        }
    }
    void OnError(CHIP_ERROR error) override { read_error = error; }
    void OnDone(ReadClient *readClient) override {
        Platform::Delete(readClient); client = nullptr;
        if (read_error != CHIP_NO_ERROR) { finish(read_error); return; }
        if (discovery) {
            if (!result.endpoint) { finish(CHIP_ERROR_NOT_FOUND); return; }
            if (action) invoke(); else read(false);
        } else if (!got_power) finish(CHIP_ERROR_NOT_FOUND);
        else if (action && !at_target()) {
            if (esp_timer_get_time() >= settle_until) { finish(CHIP_ERROR_TIMEOUT); return; }
            // Some bulbs fade even with transitionTime=0. Confirm the final
            // requested attributes, rather than presenting an intermediate read.
            auto err = DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(200),
                [](System::Layer *, void *context) { static_cast<Request *>(context)->read(false); }, this);
            if (err != CHIP_NO_ERROR) finish(err);
        } else finish(CHIP_NO_ERROR);
    }
    template<typename T> void send(const T &command) {
        if (!session) { finish(CHIP_ERROR_INCORRECT_STATE); return; }
        auto err = Controller::InvokeCommandRequest(exchange, session.Get().Value(), result.endpoint, command,
            [this](const ConcreteCommandPath &, const StatusIB &, const typename T::ResponseType &) {
                settle_until = esp_timer_get_time() + 15000000; read(false);
            },
            [this](CHIP_ERROR error) { finish(error); }, MakeOptional(System::Clock::Seconds16(10)));
        if (err != CHIP_NO_ERROR) finish(err);
    }
    void invoke() {
        if (action == 1) {
            if (value) send(OnOff::Commands::On::Type{}); else send(OnOff::Commands::Off::Type{});
        } else if (action == 2 && result.level) {
            LevelControl::Commands::MoveToLevelWithOnOff::Type cmd;
            cmd.level = std::max(1, static_cast<int>(lround(value * 254.0 / 100.0))); cmd.transitionTime = 0;
            send(cmd);
        } else if (action == 3 && result.color) {
            ColorControl::Commands::MoveToColorTemperature::Type cmd;
            int mired = lround(1000000.0 / value);
            if (result.min_mired) mired = std::max(mired, result.min_mired);
            if (result.max_mired) mired = std::min(mired, result.max_mired);
            value = lround(1000000.0 / mired);
            cmd.colorTemperatureMireds = mired; cmd.transitionTime = 0;
            cmd.optionsMask = ColorControl::OptionsBitmap::kExecuteIfOff;
            cmd.optionsOverride = ColorControl::OptionsBitmap::kExecuteIfOff;
            send(cmd);
        } else if (action == 4 && result.color) {
            ColorControl::Commands::MoveToHueAndSaturation::Type cmd;
            cmd.hue = lround(value * 254.0 / 360.0); cmd.saturation = lround(saturation * 254.0 / 100.0); cmd.transitionTime = 0;
            cmd.optionsMask = ColorControl::OptionsBitmap::kExecuteIfOff;
            cmd.optionsOverride = ColorControl::OptionsBitmap::kExecuteIfOff;
            send(cmd);
        } else finish(CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
    }
};

esp_err_t json_reply(httpd_req_t *req, cJSON *json, const char *status = "200 OK") {
    char *body = cJSON_PrintUnformatted(json); cJSON_Delete(json);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json"); httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    auto err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN); cJSON_free(body); return err;
}
esp_err_t fail(httpd_req_t *req, const char *message, const char *status = "400 Bad Request") {
    auto *json = cJSON_CreateObject(); cJSON_AddStringToObject(json, "error", message); return json_reply(req, json, status);
}
esp_err_t page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, reinterpret_cast<const char *>(page_start), page_end - page_start);
}
esp_err_t status(httpd_req_t *req) {
    Bulb copy[2]; bool active; uint32_t id; char error[128];
    portENTER_CRITICAL(&mux); memcpy(copy, bulbs, sizeof(copy)); active = busy; id = operation; memcpy(error, operation_error, sizeof(error)); portEXIT_CRITICAL(&mux);
    auto *json = cJSON_CreateObject(); cJSON_AddBoolToObject(json, "busy", active); cJSON_AddNumberToObject(json, "operation", id);
    cJSON_AddStringToObject(json, "error", error); cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
    auto *array = cJSON_AddArrayToObject(json, "bulbs");
    for (auto &b : copy) {
        auto *item = cJSON_CreateObject(); cJSON_AddItemToArray(array, item);
        cJSON_AddNumberToObject(item, "node", b.node); cJSON_AddNumberToObject(item, "endpoint", b.endpoint);
        cJSON_AddBoolToObject(item, "online", b.online); cJSON_AddStringToObject(item, "error", b.error);
        cJSON_AddNumberToObject(item, "power", b.power); cJSON_AddNumberToObject(item, "brightness", b.brightness);
        cJSON_AddNumberToObject(item, "kelvin", b.kelvin); cJSON_AddNumberToObject(item, "hue", b.hue);
        cJSON_AddNumberToObject(item, "saturation", b.saturation); cJSON_AddNumberToObject(item, "mode", b.mode);
        cJSON_AddBoolToObject(item, "level", b.level); cJSON_AddBoolToObject(item, "color", b.color);
        cJSON_AddNumberToObject(item, "min_kelvin", b.max_mired ? std::max(1800, 1000000 / b.max_mired) : 1800);
        cJSON_AddNumberToObject(item, "max_kelvin", b.min_mired ? std::min(6500, 1000000 / b.min_mired) : 6500);
    }
    return json_reply(req, json);
}
bool integer(cJSON *json, const char *key, int min, int max, int &out) {
    auto *value = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble != value->valueint || value->valueint < min || value->valueint > max) return false;
    out = value->valueint; return true;
}
esp_err_t command(httpd_req_t *req) {
    // JSON plus a same-origin-only custom header prevents cross-site form requests.
    char marker[24];
    if (httpd_req_get_hdr_value_str(req, "X-SmartHub", marker, sizeof(marker)) != ESP_OK || strcmp(marker, "control")) return fail(req, "Use the SmartHub page", "403 Forbidden");
    if (req->content_len <= 0 || req->content_len > 255) return fail(req, "Invalid request size");
    char body[256]; int received = 0;
    while (received < req->content_len) {
        int bytes = httpd_req_recv(req, body + received, req->content_len - received);
        if (bytes <= 0) return fail(req, "Incomplete request");
        received += bytes;
    }
    body[received] = 0; auto *json = cJSON_Parse(body);
    if (!json) return fail(req, "Invalid JSON");
    int node, action = -1, value = 0, sat = 100;
    auto *name = cJSON_GetObjectItemCaseSensitive(json, "action");
    if (integer(json, "node", 1, 2, node) && cJSON_IsString(name)) {
        if (!strcmp(name->valuestring, "refresh")) action = 0;
        if (!strcmp(name->valuestring, "power") && integer(json, "value", 0, 1, value)) action = 1;
        if (!strcmp(name->valuestring, "brightness") && integer(json, "value", 1, 100, value)) action = 2;
        if (!strcmp(name->valuestring, "temperature") && integer(json, "value", 1800, 6500, value)) action = 3;
        if (!strcmp(name->valuestring, "color") && integer(json, "value", 0, 360, value) && integer(json, "saturation", 0, 100, sat)) action = 4;
    }
    cJSON_Delete(json);
    if (action < 0) return fail(req, "Invalid bulb, action or value");
    portENTER_CRITICAL(&mux);
    bool active = busy;
    if (!active) { busy = true; ++operation; operation_error[0] = 0; }
    portEXIT_CRITICAL(&mux);
    if (active) return fail(req, "A bulb command is still running", "409 Conflict");
    auto *request = Platform::New<Request>(node - 1, action, value, sat);
    auto err = request ? DeviceLayer::PlatformMgr().ScheduleWork(Request::start, reinterpret_cast<intptr_t>(request)) : CHIP_ERROR_NO_MEMORY;
    if (err != CHIP_NO_ERROR) {
        if (request) Platform::Delete(request);
        portENTER_CRITICAL(&mux); busy = false; portEXIT_CRITICAL(&mux);
        return fail(req, "Unable to queue command", "503 Service Unavailable");
    }
    auto *reply = cJSON_CreateObject(); cJSON_AddNumberToObject(reply, "operation", operation);
    return json_reply(req, reply, "202 Accepted");
}
}

esp_err_t web_control_start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.stack_size = 6144; config.max_open_sockets = 3; config.lru_purge_enable = true;
    httpd_handle_t server;
    auto err = httpd_start(&server, &config); if (err != ESP_OK) return err;
    httpd_uri_t root = {}; root.uri = "/"; root.method = HTTP_GET; root.handler = page;
    httpd_uri_t info = {}; info.uri = "/api/status"; info.method = HTTP_GET; info.handler = status;
    httpd_uri_t control = {}; control.uri = "/api/control"; control.method = HTTP_POST; control.handler = command;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root)); ESP_ERROR_CHECK(httpd_register_uri_handler(server, &info)); ESP_ERROR_CHECK(httpd_register_uri_handler(server, &control));
    ESP_LOGI(TAG, "Phone controls ready on HTTP port 80");
    return ESP_OK;
}
