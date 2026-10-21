#pragma once
#include "domain_proxy_runtime.h"
#include "social_network_rpc_type.h"

// Extra provenance is exclusive to the new request types. No legacy ABI edit.
struct DomainUserRequest {
  sn_domain::Origin origin;
  UserTimeLineReq request;
};
struct DomainUserResponse {
  sn_domain::Origin origin;
  PostStorageReadCXLResp response;
};
struct DomainHomeRequest {
  sn_domain::Origin origin;
  HomeTimeLineReq request;
};
struct DomainHomeResponse {
  sn_domain::Origin origin;
  PostStorageReadCXLResp response;
};
static_assert(sizeof(DomainUserRequest) == 48, "domain request wire layout");
static_assert(sizeof(DomainUserResponse) == 64, "domain response wire layout");
static_assert(sizeof(DomainHomeRequest) == sizeof(DomainUserRequest), "home request wire layout");
static_assert(sizeof(DomainHomeResponse) == sizeof(DomainUserResponse), "home response wire layout");
static_assert(static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_REQ) == 21, "domain request type");
static_assert(static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_RESP) == 22, "domain response type");
static_assert(static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_READY) == 23, "domain readiness type");
