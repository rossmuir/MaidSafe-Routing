/*******************************************************************************
 *  Copyright 2012 maidsafe.net limited                                        *
 *                                                                             *
 *  The following source code is property of maidsafe.net limited and is not   *
 *  meant for external use.  The use of this code is governed by the licence   *
 *  file licence.txt found in the root of this directory and also on           *
 *  www.maidsafe.net.                                                          *
 *                                                                             *
 *  You are not free to copy, amend or otherwise use this source code without  *
 *  the explicit written permission of the board of directors of maidsafe.net. *
 ******************************************************************************/

#include "maidsafe/routing/routing_impl.h"

#include <cstdint>

#include "maidsafe/common/log.h"

#include "maidsafe/rudp/managed_connections.h"
#include "maidsafe/rudp/return_codes.h"

#include "maidsafe/routing/bootstrap_file_handler.h"
#include "maidsafe/routing/message_handler.h"
#include "maidsafe/routing/node_info.h"
#include "maidsafe/routing/parameters.h"
#include "maidsafe/routing/return_codes.h"
#include "maidsafe/routing/routing_pb.h"
#include "maidsafe/routing/rpcs.h"
#include "maidsafe/routing/utils.h"


namespace fs = boost::filesystem;

namespace maidsafe {

namespace routing {

namespace {

typedef boost::asio::ip::udp::endpoint Endpoint;

}  // unnamed namespace

Routing::Impl::Impl(const Fob& fob, bool client_mode)
    : kFob_([&fob]()->Fob {
          if (fob.identity.IsInitialised())
            return fob;
          Fob fob_temp;
          fob_temp.keys = asymm::GenerateKeyPair();
          fob_temp.identity = Identity(RandomString(64));
          return fob_temp;
      }()),
      kNodeId_(NodeId(kFob_.identity)),
      kAnonymousNode_(!fob.identity.IsInitialised()),
      running_(true),
      running_mutex_(),
      functors_(),
      random_node_helper_(),
      routing_table_(kFob_, client_mode),
      non_routing_table_(kFob_),  // TODO(Prakash) : don't create NRT for client nodes (wrap both)
      message_handler_(),
      asio_service_(2),
      network_(routing_table_, non_routing_table_),
      timer_(asio_service_),
      re_bootstrap_timer_(asio_service_.service()),
      recovery_timer_(asio_service_.service()),
      setup_timer_(asio_service_.service()) {
  asio_service_.Start();
  message_handler_.reset(new MessageHandler(routing_table_, non_routing_table_, network_, timer_));

  assert((client_mode || fob.identity.IsInitialised()) &&
         "Server Nodes cannot be created without valid keys");
  if (kAnonymousNode_) {
    LOG(kInfo) << "Anonymous node id: " << DebugId(kNodeId_)
               << ", connection id" << DebugId(routing_table_.kConnectionId());
  }
}

Routing::Impl::~Impl() {
  LOG(kVerbose) << "~Impl " << DebugId(kNodeId_) << ", connection id "
                << DebugId(routing_table_.kConnectionId());
  std::lock_guard<std::mutex> lock(running_mutex_);
  running_ = false;
}

void Routing::Impl::Join(const Functors& functors, const std::vector<Endpoint>& peer_endpoints) {
  ConnectFunctors(functors);
  if (!peer_endpoints.empty()) {
    BootstrapFromTheseEndpoints(peer_endpoints);
  } else {
    LOG(kInfo) << "Doing a default join";
    DoJoin(peer_endpoints);
  }
}

void Routing::Impl::ConnectFunctors(const Functors& functors) {
  routing_table_.set_remove_node_functor([this](const NodeInfo& node, bool internal_rudp_only) {
                                             RemoveNode(node, internal_rudp_only);
                                           });
  routing_table_.set_network_status_functor(functors.network_status);
  routing_table_.set_close_node_replaced_functor(functors.close_node_replaced);
  message_handler_->set_message_received_functor(functors.message_received);
  message_handler_->set_request_public_key_functor(functors.request_public_key);
  network_.set_new_bootstrap_endpoint_functor(functors.new_bootstrap_endpoint);
  functors_ = functors;
}

void Routing::Impl::DisconnectFunctors() {
  routing_table_.set_remove_node_functor(nullptr);
  routing_table_.set_network_status_functor(nullptr);
  routing_table_.set_close_node_replaced_functor(nullptr);
  message_handler_->set_message_received_functor(nullptr);
  message_handler_->set_request_public_key_functor(nullptr);
  functors_ = Functors();
}

void Routing::Impl::BootstrapFromTheseEndpoints(const std::vector<Endpoint>& endpoints) {
  LOG(kInfo) << "Doing a BootstrapFromTheseEndpoints Join.  Entered first bootstrap endpoint: "
             << endpoints[0] << ", this node's ID: " << DebugId(kNodeId_)
             << (routing_table_.client_mode() ? " Client" : "");
  if (routing_table_.Size() > 0) {
    for (uint16_t i = 0; i < routing_table_.Size(); ++i) {
      NodeInfo remove_node = routing_table_.GetClosestNode(kNodeId_);
      network_.Remove(remove_node.connection_id);
      routing_table_.DropNode(remove_node.node_id, true);
    }
    NotifyNetworkStatus(static_cast<int>(routing_table_.Size()));
  }
  DoJoin(endpoints);
}

void Routing::Impl::DoJoin(const std::vector<Endpoint>& endpoints) {
  int return_value(DoBootstrap(endpoints));
  if (kSuccess != return_value)
    return NotifyNetworkStatus(return_value);

  assert(!network_.bootstrap_connection_id().IsZero() &&
         "Bootstrap connection id must be populated by now.");
  if (kAnonymousNode_)  // No need to do find node for anonymous node
    return NotifyNetworkStatus(return_value);

  assert(!kAnonymousNode_ && "Not allowed for anonymous nodes");
  FindClosestNode(boost::system::error_code(), 0);
  NotifyNetworkStatus(return_value);
}

int Routing::Impl::DoBootstrap(const std::vector<Endpoint>& endpoints) {
  // FIXME race condition if a new connection appears at rudp -- rudp should handle this
  assert(routing_table_.Size() == 0);
  recovery_timer_.cancel();
  setup_timer_.cancel();
  std::lock_guard<std::mutex> lock(running_mutex_);
  if (!running_)
    return kNetworkShuttingDown;
  if (!network_.bootstrap_connection_id().IsZero()) {
    LOG(kInfo) << "Removing bootstrap connection to rebootstrap. Connection id : "
               << DebugId(network_.bootstrap_connection_id());
    network_.Remove(network_.bootstrap_connection_id());
    network_.clear_bootstrap_connection_info();
  }

  return network_.Bootstrap(
      endpoints,
      [=](const std::string& message) { OnMessageReceived(message); },
      [=](const NodeId& lost_connection_id) { OnConnectionLost(lost_connection_id);});  // NOLINT
}

void Routing::Impl::FindClosestNode(const boost::system::error_code& error_code, int attempts) {
  {
    std::lock_guard<std::mutex> lock(running_mutex_);
    if (!running_)
      return;
  }
  if (error_code == boost::asio::error::operation_aborted)
    return;

  assert(!kAnonymousNode_ && "Not allowed for anonymous nodes");
  if (attempts == 0) {
    assert(!network_.bootstrap_connection_id().IsZero() && "Only after bootstrapping succeeds");
    assert(!network_.this_node_relay_connection_id().IsZero() &&
            "Relay connection id should be set after bootstrapping succeeds");
  } else {
    if (routing_table_.Size() > 0) {
      std::lock_guard<std::mutex> lock(running_mutex_);
      if (!running_)
        return;
      // Exit the loop & start recovery loop
      LOG(kVerbose) << "Added a node in routing table."
                    << " Terminating setup loop & Scheduling recovery loop.";
      recovery_timer_.expires_from_now(Parameters::find_node_interval);
      recovery_timer_.async_wait([=](const boost::system::error_code& error_code) {
                                      if (error_code != boost::asio::error::operation_aborted)
                                        ReSendFindNodeRequest(error_code, false);
                                    });
      return;
    }

    if (attempts >= Parameters::maximum_find_close_node_failures) {
      LOG(kError) << "[" << HexSubstr(kFob_.identity)
                  << "] failed to get closest node."
                  << " ReBootstrapping ...";
      // TODO(Prakash) : Remove the bootstrap node from the list
      ReBootstrap();
    }
  }

  protobuf::Message find_node_rpc(
      rpcs::FindNodes(kNodeId_, kNodeId_, 1, true, network_.this_node_relay_connection_id()));

  rudp::MessageSentFunctor message_sent_functor(
      [=](int message_sent) {
        if (message_sent == kSuccess)
            LOG(kInfo) << "   [" << DebugId(kNodeId_) << "] sent : "
                      << MessageTypeString(find_node_rpc) << " to   "
                      << DebugId(network_.bootstrap_connection_id())
                      << "   (id: " << find_node_rpc.id() << ")";
        else
          LOG(kError) << "Failed to send FindNodes RPC to bootstrap connection id : "
                      << DebugId(network_.bootstrap_connection_id());
      });

  ++attempts;
  network_.SendToDirect(find_node_rpc, network_.bootstrap_connection_id(), message_sent_functor);

  std::lock_guard<std::mutex> lock(running_mutex_);
  if (!running_)
    return;
  setup_timer_.expires_from_now(Parameters::find_close_node_interval);
  setup_timer_.async_wait([=](boost::system::error_code error_code_local) {
                              if (error_code_local != boost::asio::error::operation_aborted)
                                FindClosestNode(error_code_local, attempts);
                            });
}

int Routing::Impl::ZeroStateJoin(const Functors& functors,
                                 const Endpoint& local_endpoint,
                                 const Endpoint& peer_endpoint,
                                 const NodeInfo& peer_info) {
  assert((!routing_table_.client_mode()) && "no client nodes allowed in zero state network");
  assert((!kAnonymousNode_) && "not allowed on anonymous node");
  ConnectFunctors(functors);
  int result(network_.Bootstrap(
      std::vector<Endpoint>(1, peer_endpoint),
      [=](const std::string& message) { OnMessageReceived(message); },
      [=](const NodeId& lost_connection_id) { OnConnectionLost(lost_connection_id); },
      local_endpoint));

  if (result != kSuccess) {
    LOG(kError) << "Could not bootstrap zero state node from local endpoint : "
                << local_endpoint << " with peer endpoint : " << peer_endpoint;
    return result;
  }

  LOG(kInfo) << "[" << DebugId(kNodeId_) << "]'s bootstrap connection id : "
             << DebugId(network_.bootstrap_connection_id());

  assert(!peer_info.node_id.IsZero() && "Zero NodeId passed");
  assert((network_.bootstrap_connection_id() == peer_info.node_id) &&
         "Should bootstrap only with known peer for zero state network");
  LOG(kVerbose) << local_endpoint << " Bootstrapped with remote endpoint " << peer_endpoint;
  rudp::NatType nat_type(rudp::NatType::kUnknown);
  rudp::EndpointPair peer_endpoint_pair;  // zero state nodes must be directly connected endpoint
  rudp::EndpointPair this_endpoint_pair;
  peer_endpoint_pair.external = peer_endpoint_pair.local = peer_endpoint;
  this_endpoint_pair.external = this_endpoint_pair.local = local_endpoint;
  Sleep(boost::posix_time::milliseconds(100));  // FIXME avoiding assert in rudp
  result = network_.GetAvailableEndpoint(peer_info.node_id, peer_endpoint_pair,
                                         this_endpoint_pair, nat_type);
  if (result != rudp::kBootstrapConnectionAlreadyExists) {
    LOG(kError) << "Failed to get available endpoint to add zero state node : " << peer_endpoint;
    return result;
  }

  result = network_.Add(peer_info.node_id, peer_endpoint_pair, "invalid");
  if (result != kSuccess) {
    LOG(kError) << "Failed to add zero state node : " << peer_endpoint;
    return result;
  }

  ValidateAndAddToRoutingTable(network_,
                               routing_table_,
                               non_routing_table_,
                               peer_info.node_id,
                               peer_info.node_id,
                               peer_info.public_key,
                               false);
  // Now poll for routing table size to have other zero state peer.
  uint8_t poll_count(0);
  do {
    Sleep(boost::posix_time::milliseconds(100));
  } while ((routing_table_.Size() == 0) && (++poll_count < 50));
  if (routing_table_.Size() != 0) {
    LOG(kInfo) << "Node Successfully joined zero state network, with "
               << DebugId(network_.bootstrap_connection_id())
               << ", Routing table size - " << routing_table_.Size()
               << ", Node id : " << DebugId(kNodeId_);

    std::lock_guard<std::mutex> lock(running_mutex_);
    if (!running_)
      return kNetworkShuttingDown;
    recovery_timer_.expires_from_now(Parameters::find_node_interval);
    recovery_timer_.async_wait([=](const boost::system::error_code& error_code) {
                                   if (error_code != boost::asio::error::operation_aborted)
                                     ReSendFindNodeRequest(error_code, false);
                                 });
    return kSuccess;
  } else {
    LOG(kError) << "Failed to join zero state network, with bootstrap_endpoint "
                << peer_endpoint;
    return kNotJoined;
  }
}

void Routing::Impl::Send(const NodeId& destination_id,
                         const NodeId& group_claim,
                         const std::string& data,
                         const ResponseFunctor& response_functor,
                         const boost::posix_time::time_duration& timeout,
                         bool direct,
                         bool cacheable) {
  if (destination_id.IsZero()) {
    LOG(kError) << "Invalid destination ID, aborted send";
    if (response_functor)
      response_functor(std::vector<std::string>());
    return;
  }

  if (data.empty() || (data.size() > Parameters::max_data_size)) {
    LOG(kError) << "Data size not allowed : " << data.size();
    if (response_functor)
      response_functor(std::vector<std::string>());
    return;
  }

  protobuf::Message proto_message;
  proto_message.set_destination_id(destination_id.string());
  proto_message.set_routing_message(false);
  proto_message.add_data(data);
  proto_message.set_type(static_cast<int32_t>(MessageType::kNodeLevel));
  proto_message.set_cacheable(cacheable);
  proto_message.set_direct(direct);
  proto_message.set_client_node(routing_table_.client_mode());
  proto_message.set_request(true);
  proto_message.set_hops_to_live(Parameters::hops_to_live);
  uint16_t replication(1);
  if (!group_claim.IsZero())
    proto_message.set_group_claim(group_claim.string());

  if (!direct) {
    replication = Parameters::node_group_size;
    if (response_functor)
      proto_message.set_id(timer_.AddTask(timeout, response_functor, Parameters::node_group_size));
  } else {
    if (response_functor)
      proto_message.set_id(timer_.AddTask(timeout, response_functor, 1));
  }

  proto_message.set_replication(replication);
  // Anonymous node /Partial join state
  if (kAnonymousNode_ || (routing_table_.Size() == 0)) {
    proto_message.set_relay_id(kNodeId_.string());
    proto_message.set_relay_connection_id(network_.this_node_relay_connection_id().string());
    NodeId bootstrap_connection_id(network_.bootstrap_connection_id());
    assert(proto_message.has_relay_connection_id() && "did not set this_node_relay_connection_id");
    rudp::MessageSentFunctor message_sent(
        [=](int result) {
          asio_service_.service().post([=]() {
              if (rudp::kSuccess != result) {
                timer_.CancelTask(proto_message.id());
                if (kAnonymousNode_) {
                  LOG(kError) << "Anonymous Session Ended, Send not allowed anymore";
                  NotifyNetworkStatus(kAnonymousSessionEnded);
                } else {
                  LOG(kError) << "Partial join Session Ended, Send not allowed anymore";
                  NotifyNetworkStatus(kPartialJoinSessionEnded);
                }
              } else {
                LOG(kInfo) << "Message Sent from Anonymous/Partial joined node";
              }
            });
          });
    network_.SendToDirect(proto_message, bootstrap_connection_id, message_sent);
    return;
  }

  // Non Anonymous, normal node
  proto_message.set_source_id(kNodeId_.string());

  if (kNodeId_ != destination_id) {
    network_.SendToClosestNode(proto_message);
  } else if (routing_table_.client_mode()) {
    LOG(kVerbose) << "Client sending request to self id";
    network_.SendToClosestNode(proto_message);
  } else {
    LOG(kInfo) << "Sending request to self";
    OnMessageReceived(proto_message.SerializeAsString());
  }
}

void Routing::Impl::OnMessageReceived(const std::string& message) {
  std::lock_guard<std::mutex> lock(running_mutex_);
  if (running_)
    asio_service_.service().post([=]() { DoOnMessageReceived(message); });  // NOLINT (Fraser)
}

void Routing::Impl::DoOnMessageReceived(const std::string& message) {
  protobuf::Message pb_message;
  if (pb_message.ParseFromString(message)) {
    bool relay_message(!pb_message.has_source_id());
    LOG(kInfo) << "   [" << DebugId(kNodeId_) << "] rcvd : "
               << MessageTypeString(pb_message) << " from "
               << (relay_message ? HexSubstr(pb_message.relay_id()) :
                                   HexSubstr(pb_message.source_id()))
               << "   (id: " << pb_message.id() << ")"
               << (relay_message ? " --Relay--" : "");
    if (((kAnonymousNode_ || !pb_message.client_node()) && pb_message.has_source_id()) ||
        (!pb_message.direct() && !pb_message.request())) {
      NodeId source_id(pb_message.source_id());
      if (!source_id.IsZero())
        random_node_helper_.Add(source_id);
    }
    message_handler_->HandleMessage(pb_message);
  } else {
    LOG(kWarning) << "Message received, failed to parse";
  }
}

void Routing::Impl::OnConnectionLost(const NodeId& lost_connection_id) {
  std::lock_guard<std::mutex> lock(running_mutex_);
  if (running_)
    asio_service_.service().post([=]() { DoOnConnectionLost(lost_connection_id); });  // NOLINT (Fraser)
}

void Routing::Impl::DoOnConnectionLost(const NodeId& lost_connection_id) {
  LOG(kVerbose) << "Routing::ConnectionLost with ----------------------------"
                << DebugId(lost_connection_id);
  {
    std::lock_guard<std::mutex> lock(running_mutex_);
    if (!running_)
      return;
  }

  NodeInfo dropped_node;
  bool resend(routing_table_.GetNodeInfo(lost_connection_id, dropped_node) &&
           routing_table_.IsThisNodeInRange(dropped_node.node_id, Parameters::closest_nodes_size));

  // Checking routing table
  dropped_node = routing_table_.DropNode(lost_connection_id, true);
  if (!dropped_node.node_id.IsZero()) {
    LOG(kWarning) << "[" << HexSubstr(kFob_.identity) << "]"
                  << "Lost connection with routing node " << DebugId(dropped_node.node_id);
    random_node_helper_.Remove(dropped_node.node_id);
  }

  // Checking non-routing table
  if (dropped_node.node_id.IsZero()) {
    resend = false;
    dropped_node = non_routing_table_.DropConnection(lost_connection_id);
    if (!dropped_node.node_id.IsZero()) {
      LOG(kWarning) << "[" << HexSubstr(kFob_.identity) << "]"
                    << "Lost connection with non-routing node "
                    << HexSubstr(dropped_node.node_id.string());
    } else if (!network_.bootstrap_connection_id().IsZero() &&
               lost_connection_id == network_.bootstrap_connection_id()) {
      LOG(kWarning) << "[" << HexSubstr(kFob_.identity) << "]"
                    << "Lost temporary connection with bootstrap node. connection id :"
                    << DebugId(lost_connection_id);
      {
        std::lock_guard<std::mutex> lock(running_mutex_);
        if (!running_)
          return;
      }
      network_.clear_bootstrap_connection_info();
      if (kAnonymousNode_) {
        LOG(kError) << "Anonymous Session Ended, Send not allowed anymore";
        NotifyNetworkStatus(kAnonymousSessionEnded);
        // TODO(Prakash) cancel all pending tasks
        return;
      }

      if (routing_table_.Size() == 0)
        resend = true;  // This will trigger rebootstrap
    } else {
      LOG(kWarning) << "[" << HexSubstr(kFob_.identity) << "]"
                    << "Lost connection with unknown/internal connection id "
                    << DebugId(lost_connection_id);
    }
  }

  if (resend) {
    std::lock_guard<std::mutex> lock(running_mutex_);
    if (!running_)
      return;
    // Close node lost, get more nodes
    LOG(kWarning) << "Lost close node, getting more.";
    recovery_timer_.expires_from_now(Parameters::recovery_time_lag);
    recovery_timer_.async_wait([=](const boost::system::error_code& error_code) {
                                   if (error_code != boost::asio::error::operation_aborted)
                                     ReSendFindNodeRequest(error_code, true);
                                 });
  }
}

void Routing::Impl::RemoveNode(const NodeInfo& node, bool internal_rudp_only) {
  if (node.connection_id.IsZero() || node.node_id.IsZero())
    return;

  network_.Remove(node.connection_id);
  if (internal_rudp_only) {  // No recovery
    LOG(kInfo) << "Routing: removed node : " << DebugId(node.node_id)
               << ". Removed internal rudp connection id : " << DebugId(node.connection_id);
    return;
  }

  LOG(kInfo) << "Routing: removed node : " << DebugId(node.node_id)
             << ". Removed rudp connection id : " << DebugId(node.connection_id);

  // TODO(Prakash): Handle pseudo connection removal here and NRT node removal

  bool resend(routing_table_.IsThisNodeInRange(node.node_id, Parameters::closest_nodes_size));
  if (resend) {
    std::lock_guard<std::mutex> lock(running_mutex_);
    if (!running_)
      return;
    // Close node removed by routing, get more nodes
    LOG(kWarning) << "Removed close node, sending find node to get more nodes.";
    recovery_timer_.expires_from_now(Parameters::recovery_time_lag);
    recovery_timer_.async_wait([=](const boost::system::error_code& error_code) {
                                   if (error_code != boost::asio::error::operation_aborted)
                                     ReSendFindNodeRequest(error_code, true);
                                 });
  }
}

bool Routing::Impl::ConfirmGroupMembers(const NodeId& node1, const NodeId& node2) {
  return routing_table_.ConfirmGroupMembers(node1, node2);
}

void Routing::Impl::ReSendFindNodeRequest(const boost::system::error_code& error_code,
                                          bool ignore_size) {
  if (error_code == boost::asio::error::operation_aborted)
    return;

  if (routing_table_.Size() == 0) {
    LOG(kError) << "[" << HexSubstr(kFob_.identity)
                << "]'s' Routing table is empty."
                << " Scheduling Re-Bootstrap .... !!!";
    ReBootstrap();
    return;
  } else if (ignore_size || (routing_table_.Size() < Parameters::routing_table_size_threshold)) {
    if (!ignore_size)
      LOG(kInfo) << "[" << DebugId(kNodeId_)
                  << "] Routing table smaller than " << Parameters::routing_table_size_threshold
                  << " nodes.  Sending another FindNodes. Routing table size < "
                  << routing_table_.Size() << " >";
    else
      LOG(kInfo) << "[" << DebugId(kNodeId_) << "] lost close node."
                  << "Sending another FindNodes. Current routing table size : "
                  << routing_table_.Size();

    int num_nodes_requested(0);
    if (ignore_size && (routing_table_.Size() > Parameters::routing_table_size_threshold))
      num_nodes_requested = static_cast<int>(Parameters::closest_nodes_size);
    else
      num_nodes_requested = static_cast<int>(Parameters::max_routing_table_size);

    protobuf::Message find_node_rpc(rpcs::FindNodes(kNodeId_, kNodeId_, num_nodes_requested));
    network_.SendToClosestNode(find_node_rpc);

    std::lock_guard<std::mutex> lock(running_mutex_);
    if (!running_)
      return;
    recovery_timer_.expires_from_now(Parameters::find_node_interval);
    recovery_timer_.async_wait([=](boost::system::error_code error_code_local) {
                                    if (error_code != boost::asio::error::operation_aborted)
                                      ReSendFindNodeRequest(error_code_local, false);
                                  });
  }
}

void Routing::Impl::ReBootstrap() {
  std::lock_guard<std::mutex> lock(running_mutex_);
  if (!running_)
    return;
  re_bootstrap_timer_.expires_from_now(Parameters::re_bootstrap_time_lag);
  re_bootstrap_timer_.async_wait([=](boost::system::error_code error_code_local) {
      if (error_code_local != boost::asio::error::operation_aborted)
        DoReBootstrap(error_code_local);
    });
}

void Routing::Impl::DoReBootstrap(const boost::system::error_code& error_code) {
  if (error_code == boost::asio::error::operation_aborted)
    return;
  {
    std::lock_guard<std::mutex> lock(running_mutex_);
    if (!running_)
      return;
  }
  LOG(kError) << "[" << HexSubstr(kFob_.identity) << "]'s' Routing table is empty."
              << " ReBootstrapping ....";
  DoJoin(std::vector<Endpoint>());
}

void Routing::Impl::NotifyNetworkStatus(int return_code) const {
  if (functors_.network_status)
    functors_.network_status(return_code);
}


}  // namespace routing

}  // namespace maidsafe