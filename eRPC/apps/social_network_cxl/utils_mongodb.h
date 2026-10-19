#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <bson/bson.h>
#include <mongoc/mongoc.h>
#include <nlohmann/json.hpp>

#define SERVER_SELECTION_TIMEOUT_MS 300

// Setup failures must not publish readiness with an empty/partially loaded map.
// Do not log a URI, credentials or query/document contents.
[[noreturn]] static inline void mongodb_load_failure(
        const char *service, const char *operation, uint32_t domain = 0,
        uint32_t code = 0) {
    std::fprintf(stderr, "SN_MONGODB_ERROR service=%s op=%s domain=%u code=%u\n",
                 service, operation, domain, code);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

static inline void require_mongodb_query_success(
        mongoc_cursor_t *cursor, const char *service) {
    if (!cursor) mongodb_load_failure(service, "cursor-create");
    bson_error_t error{};
    if (mongoc_cursor_error(cursor, &error))
        mongodb_load_failure(service, "query", error.domain, error.code);
}

static inline mongoc_client_pool_t* init_mongodb_client_pool(
        const nlohmann::json &config_json,
        const std::string &service_name,
        uint32_t max_size) {
    std::string addr = config_json[service_name + "_mongodb"]["addr"];
    std::string port = config_json[service_name + "_mongodb"]["port"];
    std::string uri_str = "mongodb://" + addr + ":" +
        port + "/?appname=" + service_name + "-service";

    mongoc_init();
    bson_error_t error;
    mongoc_uri_t *mongodb_uri =
            mongoc_uri_new_with_error(uri_str.c_str(), &error);

    if (!mongodb_uri) {
        mongodb_load_failure(service_name.c_str(), "uri-parse", error.domain, error.code);
    }
    mongoc_client_pool_t *client_pool = mongoc_client_pool_new(mongodb_uri);
    mongoc_uri_destroy(mongodb_uri);
    if (!client_pool) mongodb_load_failure(service_name.c_str(), "pool-create");
    mongoc_client_pool_max_size(client_pool, max_size);
    return client_pool;
}
