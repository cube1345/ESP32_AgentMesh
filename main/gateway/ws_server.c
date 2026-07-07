#include "ws_server.h"
#include "espagent_config.h"
#include "bus/message_bus.h"
#include "skills/skill_runtime.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "ws";
static const int MAX_SKILL_API_BODY_BYTES = (12 * 1024) + 512;

static httpd_handle_t s_server = NULL;

/* Simple client tracking */
typedef struct {
    int fd;
    char chat_id[32];
    bool active;
} ws_client_t;


static ws_client_t s_clients[ESPAGENT_WS_MAX_CLIENTS];

static ws_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < ESPAGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (int i = 0; i < ESPAGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *add_client(int fd)
{
    ws_client_t *existing = find_client_by_fd(fd);
    if (existing) {
        return existing;
    }

    for (int i = 0; i < ESPAGENT_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            s_clients[i].active = true;
            ESP_LOGI(TAG, "Client connected: %s (fd=%d)", s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    ESP_LOGW(TAG, "Max clients reached, rejecting fd=%d", fd);
    return NULL;
}

static void remove_client(int fd)
{
    for (int i = 0; i < ESPAGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGI(TAG, "Client disconnected: %s", s_clients[i].chat_id);
            s_clients[i].active = false;
            return;
        }
    }
}

static void set_api_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
}

static esp_err_t send_api_json(httpd_req_t *req, const char *status, const char *json)
{
    set_api_headers(req);
    if (status && status[0]) {
        httpd_resp_set_status(req, status);
    }
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_api_error(httpd_req_t *req, const char *status, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error ? error : "unknown error");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    esp_err_t err = send_api_json(req, status, json);
    cJSON_free(json);
    return err;
}

static esp_err_t read_request_body(httpd_req_t *req, char **body_out)
{
    if (!req || !body_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *body_out = NULL;
    if (req->content_len <= 0 || req->content_len > MAX_SKILL_API_BODY_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = calloc(1, (size_t)req->content_len + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';
    *body_out = body;
    return ESP_OK;
}

static esp_err_t http_get_skills(httpd_req_t *req)
{
    char json[4096];
    esp_err_t err = skill_runtime_list_json(json, sizeof(json));
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return send_api_error(req, "500 Internal Server Error", "failed to enumerate skills");
    }
    return send_api_json(req, "200 OK", json);
}

static esp_err_t http_post_skills(httpd_req_t *req)
{
    char *body = NULL;
    if (read_request_body(req, &body) != ESP_OK) {
        return send_api_error(req, "400 Bad Request", "invalid request body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_api_error(req, "400 Bad Request", "invalid JSON body");
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    bool confirmed = cJSON_IsTrue(cJSON_GetObjectItem(root, "confirmed"));

    char message[256];
    esp_err_t err = skill_runtime_upsert(name, content, confirmed, message, sizeof(message));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_api_error(req,
                              err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG
                                  ? "400 Bad Request"
                                  : "500 Internal Server Error",
                              message);
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "name", name ? name : "");
    cJSON_AddStringToObject(resp, "message", message);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    esp_err_t send_err = send_api_json(req, "200 OK", json);
    cJSON_free(json);
    return send_err;
}

static esp_err_t http_delete_skills(httpd_req_t *req)
{
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_api_error(req, "400 Bad Request", "missing query string");
    }

    char name[64] = {0};
    char confirmed_text[16] = {0};
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return send_api_error(req, "400 Bad Request", "missing skill name");
    }
    bool confirmed =
        httpd_query_key_value(query, "confirmed", confirmed_text, sizeof(confirmed_text)) == ESP_OK &&
        (strcmp(confirmed_text, "true") == 0 || strcmp(confirmed_text, "1") == 0);

    char message[256];
    esp_err_t err = skill_runtime_delete(name, confirmed, message, sizeof(message));
    if (err != ESP_OK) {
        return send_api_error(req,
                              err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_FOUND
                                  ? "400 Bad Request"
                                  : "500 Internal Server Error",
                              message);
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "name", name);
    cJSON_AddStringToObject(resp, "message", message);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    esp_err_t send_err = send_api_json(req, "200 OK", json);
    cJSON_free(json);
    return send_err;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — register client */
        int fd = httpd_req_to_sockfd(req);
        add_client(fd);
        return ESP_OK;
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len == 0) return ESP_OK;

    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = find_client_by_fd(fd);
    if (!client) {
        client = add_client(fd);
        if (!client) {
            free(ws_pkt.payload);
            ESP_LOGW(TAG, "Failed to track websocket client fd=%d", fd);
            return ESP_ERR_NO_MEM;
        }
    }

    /* Parse JSON message */
    cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
    free(ws_pkt.payload);

    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON from fd=%d", fd);
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *content = cJSON_GetObjectItem(root, "content");

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0
        && content && cJSON_IsString(content)) {

        /* Determine chat_id */
        const char *chat_id = client ? client->chat_id : "ws_unknown";
        cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
        if (cid && cJSON_IsString(cid)) {
            chat_id = cid->valuestring;
            /* Update client's chat_id if provided */
            if (client) {
                snprintf(client->chat_id, sizeof(client->chat_id), "%s", chat_id);
            }
        }

        ESP_LOGI(TAG, "WS message from %s: %.40s...", chat_id, content->valuestring);

        /* Push to inbound bus */
        espagent_msg_t msg = {0};
        strncpy(msg.channel, ESPAGENT_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(content->valuestring);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = ESPAGENT_WS_PORT;
    config.ctrl_port = ESPAGENT_WS_PORT + 1;
    config.max_open_sockets = ESPAGENT_WS_MAX_CLIENTS;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register WebSocket URI */
    httpd_uri_t ws_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    httpd_uri_t skills_get_uri = {
        .uri = "/api/skills",
        .method = HTTP_GET,
        .handler = http_get_skills,
    };
    httpd_register_uri_handler(s_server, &skills_get_uri);

    httpd_uri_t skills_post_uri = {
        .uri = "/api/skills",
        .method = HTTP_POST,
        .handler = http_post_skills,
    };
    httpd_register_uri_handler(s_server, &skills_post_uri);

    httpd_uri_t skills_delete_uri = {
        .uri = "/api/skills",
        .method = HTTP_DELETE,
        .handler = http_delete_skills,
    };
    httpd_register_uri_handler(s_server, &skills_delete_uri);

    ESP_LOGI(TAG, "WebSocket server started on port %d", ESPAGENT_WS_PORT);
    return ESP_OK;
}

esp_err_t ws_server_send(const char *chat_id, const char *text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    ws_client_t *client = find_client_by_chat_id(chat_id);
    if (!client) {
        ESP_LOGW(TAG, "No WS client with chat_id=%s", chat_id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build response JSON */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len = strlen(json_str),
    };

    esp_err_t ret = httpd_ws_send_frame_async(s_server, client->fd, &ws_pkt);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send to %s: %s", chat_id, esp_err_to_name(ret));
        remove_client(client->fd);
    }

    return ret;
}

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
    return ESP_OK;
}
