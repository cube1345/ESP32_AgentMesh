#include "mesh/mesh_auth.h"

#include "espagent_config.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <string.h>

#ifndef ESPAGENT_MESH_AUTH_KEY
#define ESPAGENT_MESH_AUTH_KEY ""
#endif

#ifndef ESPAGENT_MESH_AUTH_PREVIOUS_KEY
#define ESPAGENT_MESH_AUTH_PREVIOUS_KEY ""
#endif

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    if (!out || out_size < len * 2 + 1) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static bool hex_equal(const char *a, const char *b)
{
    if (!a || !b || strlen(a) != 64 || strlen(b) != 64) {
        return false;
    }
    uint8_t diff = 0;
    for (int i = 0; i < 64; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool espagent_mesh_auth_enabled(void)
{
    return ESPAGENT_MESH_AUTH_KEY[0] != '\0' || ESPAGENT_MESH_AUTH_PREVIOUS_KEY[0] != '\0';
}

static esp_err_t build_canonical(const espagent_mesh_command_t *cmd,
                                 char *buf,
                                 size_t buf_size)
{
    if (!cmd || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(buf,
                     buf_size,
                     "%s\n%s\n%s\n%s\n%s\n%s\n%d\n%d\n%lld",
                     cmd->command_id,
                     cmd->trace_id,
                     cmd->target_node,
                     cmd->target_role,
                     cmd->action,
                     cmd->args_json,
                     cmd->ttl_ms,
                     cmd->safety_level,
                     (long long)cmd->ts_ms);
    return (n > 0 && (size_t)n < buf_size) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t sign_with_key(const espagent_mesh_command_t *cmd,
                               const char *key,
                               char *signature,
                               size_t signature_size)
{
    if (!cmd || !key || !key[0] || !signature || signature_size < ESPAGENT_MESH_SIGNATURE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    char canonical[640] = {0};
    esp_err_t err = build_canonical(cmd, canonical, sizeof(canonical));
    if (err != ESP_OK) {
        return err;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attrs, strlen(key) * 8);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    status = psa_import_key(&attrs,
                            (const uint8_t *)key,
                            strlen(key),
                            &key_id);
    psa_reset_key_attributes(&attrs);
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    uint8_t mac[32] = {0};
    size_t mac_len = 0;
    status = psa_mac_compute(key_id,
                             PSA_ALG_HMAC(PSA_ALG_SHA_256),
                             (const uint8_t *)canonical,
                             strlen(canonical),
                             mac,
                             sizeof(mac),
                             &mac_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || mac_len != sizeof(mac)) {
        return ESP_FAIL;
    }
    bytes_to_hex(mac, sizeof(mac), signature, signature_size);
    return ESP_OK;
}

esp_err_t espagent_mesh_auth_sign_command(const espagent_mesh_command_t *cmd,
                                          char *signature,
                                          size_t signature_size)
{
    if (!cmd || !signature || signature_size < ESPAGENT_MESH_SIGNATURE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ESPAGENT_MESH_AUTH_KEY[0] == '\0') {
        signature[0] = '\0';
        return ESP_OK;
    }
    return sign_with_key(cmd, ESPAGENT_MESH_AUTH_KEY, signature, signature_size);
}

esp_err_t espagent_mesh_auth_verify_command(const espagent_mesh_command_t *cmd,
                                            char *reason,
                                            size_t reason_size)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espagent_mesh_auth_enabled()) {
        if (reason && reason_size) {
            snprintf(reason, reason_size, "mesh auth disabled");
        }
        return ESP_OK;
    }
    if (cmd->signature[0] == '\0') {
        if (reason && reason_size) {
            snprintf(reason, reason_size, "missing mesh command signature");
        }
        return ESP_ERR_INVALID_STATE;
    }

    char expected[ESPAGENT_MESH_SIGNATURE_MAX] = {0};
    esp_err_t err = ESP_ERR_INVALID_CRC;
    if (ESPAGENT_MESH_AUTH_KEY[0]) {
        err = sign_with_key(cmd, ESPAGENT_MESH_AUTH_KEY, expected, sizeof(expected));
        if (err == ESP_OK && hex_equal(expected, cmd->signature)) {
            if (reason && reason_size) {
                snprintf(reason, reason_size, "mesh command signature verified current key");
            }
            return ESP_OK;
        }
    }
    if (ESPAGENT_MESH_AUTH_PREVIOUS_KEY[0]) {
        err = sign_with_key(cmd, ESPAGENT_MESH_AUTH_PREVIOUS_KEY, expected, sizeof(expected));
        if (err == ESP_OK && hex_equal(expected, cmd->signature)) {
            if (reason && reason_size) {
                snprintf(reason, reason_size, "mesh command signature verified previous key");
            }
            return ESP_OK;
        }
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_CRC) {
        if (reason && reason_size) {
            snprintf(reason, reason_size, "failed to compute mesh signature");
        }
        return err;
    }
    if (reason && reason_size) {
        snprintf(reason, reason_size, "mesh command signature mismatch");
    }
    return ESP_ERR_INVALID_CRC;
}
