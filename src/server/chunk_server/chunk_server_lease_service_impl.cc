#include "src/server/chunk_server/chunk_server_lease_service_impl.h"

#include "absl/time/time.h"
#include "grpcpp/grpcpp.h"
#include "src/common/system_logger.h"
#include "src/common/utils.h"
#include "src/protos/grpc/chunk_server_lease_service.grpc.pb.h"

using gfs::common::utils::ConvertProtobufStatusToGrpcStatus;
using google::protobuf::util::IsNotFound;
using google::protobuf::util::Status;
using google::protobuf::util::StatusOr;
using grpc::ServerContext;
using protos::grpc::GrantLeaseReply;
using protos::grpc::GrantLeaseRequest;
using protos::grpc::RevokeLeaseReply;
using protos::grpc::RevokeLeaseRequest;

namespace gfs {
namespace service {

grpc::Status ChunkServerLeaseServiceImpl::GrantLease(
    ServerContext* context, const GrantLeaseRequest* request,
    GrantLeaseReply* reply) {
  LOG(INFO) << "Received GrantLeaseRequest:" << (*request).DebugString();
  *reply->mutable_request() = *request;

  // No redo/undo logs needed. If the chunk server crashes at any point of
  // handling a GrantLease request, it will fail to reply to the master and the
  // master is safe to assume it has not accepted lease successfully. Since we
  // do not store lease information on disk, upon restart of the chunk server,
  // it will start off holding no leases neither, consistent with master's
  // assumption. If a chunk sever crashes *after* accepting a lease and
  // replying to the master, it's also safe for master to wait until the lease
  // expires before reassign someone else the lease. If the chunk server
  // recovers before the lease expires, then the chunk server's startup
  // ReportToMaster RPC call will also effectively let master know it crashed
  // and restarted, and the master can then determine how to re-issue the lease.

  LOG(INFO) << "Validating GrantLeaseRequest for: " << request->chunk_handle();
  StatusOr<uint32_t> owned_version_or =
      chunk_server_impl_->GetChunkVersion(request->chunk_handle());
  if (!owned_version_or.ok()) {
    if (IsNotFound(owned_version_or.status())) {
      LOG(INFO) << "Cannot accept lease because " << request->chunk_handle()
                << " doesn't exit on this chunk server";
      reply->set_status(GrantLeaseReply::REJECTED_NOT_FOUND);
      return grpc::Status::OK;
    } else {
      LOG(ERROR) << "Unexpected error when receiving lease: "
                 << owned_version_or.status();
      return ConvertProtobufStatusToGrpcStatus(owned_version_or.status());
    }
  } else if (owned_version_or.value() < request->chunk_version()) {
    LOG(INFO) << "Cannot accept lease because " << request->chunk_handle()
              << " is stale on this chunk server";
    reply->set_status(GrantLeaseReply::REJECTED_STALE_VERSION);
    return grpc::Status::OK;
  } else if (owned_version_or.value() > request->chunk_version()) {
    LOG(INFO) << "Cannot accept lease because " << request->chunk_handle()
              << " has a newer version on this chunk server";
    reply->set_status(GrantLeaseReply::UNKNOWN);
    return grpc::Status::OK;
  } else if (absl::Now() > absl::FromUnixSeconds(
                               request->lease_expiration_time().seconds())) {
    LOG(INFO) << "Cannot accept lease for " << request->chunk_handle()
              << " because the given lease is already expired";
    reply->set_status(GrantLeaseReply::IGNORED_EXPIRED_LEASE);
    return grpc::Status::OK;
  } else {
    chunk_server_impl_->AddOrUpdateLease(
        request->chunk_handle(), request->lease_expiration_time().seconds());
    LOG(INFO) << "New lease accepted for " << request->chunk_handle();
    reply->set_status(GrantLeaseReply::ACCEPTED);
    return grpc::Status::OK;
  }
}

grpc::Status ChunkServerLeaseServiceImpl::RevokeLease(
    ServerContext* context, const RevokeLeaseRequest* request,
    RevokeLeaseReply* reply) {
  LOG(INFO) << "Received RevokeLeaseRequest:" << (*request).DebugString();
  *reply->mutable_request() = *request;

  // No redo/undo logs needed. If the chunk server crashes at any point of
  // handling a RevokeLease request, it will fail to reply to the master and the
  // master is safe to assume it has not revoked the lease and either retry or
  // wait until the lease expires.  If a chunk sever crashes *after* revoking
  // a lease and replying to the master, since we do not store lease information
  // on disk, upon restart of the chunk server, it will start off holding no
  // leases neither, consistent with the outcome of "revoking" a lease.

  LOG(INFO) << "Validating RevokeLeaseRequest for: " << request->chunk_handle();
  StatusOr<absl::Time> current_lease_expiration_time_or =
      chunk_server_impl_->GetLeaseExpirationTime(request->chunk_handle());
  if (!current_lease_expiration_time_or.ok()) {
    if (IsNotFound(current_lease_expiration_time_or.status())) {
      LOG(INFO) << "No lease to revoke for " << request->chunk_handle();
      reply->set_status(RevokeLeaseReply::REJECTED_NOT_FOUND);
      return grpc::Status::OK;
    } else {
      LOG(ERROR) << "Unexpected error when revoking lease: "
                 << current_lease_expiration_time_or.status();
      return ConvertProtobufStatusToGrpcStatus(
          current_lease_expiration_time_or.status());
    }
  }
  absl::Time original_lease_expiration_time = absl::FromUnixSeconds(
      request->original_lease_expiration_time().seconds());
  if (original_lease_expiration_time <
      current_lease_expiration_time_or.value()) {
    LOG(INFO) << "Server already holds a newer lease that expires in future: "
              << request->chunk_handle();
    reply->set_status(RevokeLeaseReply::IGNORED_HAS_NEWER_LEASE);
  } else {
    chunk_server_impl_->RemoveLease(request->chunk_handle());
    LOG(INFO) << "Lease successfully revoked for " << request->chunk_handle();
    reply->set_status(RevokeLeaseReply::REVOKED);
  }
  return grpc::Status::OK;
}

}  // namespace service
}  // namespace gfs
