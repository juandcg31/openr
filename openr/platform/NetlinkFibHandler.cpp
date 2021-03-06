/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkFibHandler.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/gen/Base.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Platform_constants.h>
#include <openr/platform/NetlinkFibHandler.h>

namespace openr {

namespace {

const std::chrono::seconds kSyncStaticRouteTimeout{30};

// iproute2 protocol IDs in the kernel are a shared resource
// Various well known and custom protocols use it
// This is a *Weak* attempt to protect against some already
// known protocols
const uint8_t kMinRouteProtocolId = 17;
const uint8_t kMaxRouteProtocolId = 253;

} // namespace

NetlinkFibHandler::NetlinkFibHandler(
    fbzmq::ZmqEventLoop* zmqEventLoop,
    std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket)
    : netlinkSocket_(netlinkSocket),
      evl_{zmqEventLoop},
      startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count()) {
  CHECK_NOTNULL(zmqEventLoop);
  // TODO: Implement this neighbor in this class
  netlinkSocket_->registerNeighborListener(
      [this](const fbnl::NetlinkSocket::NeighborUpdate& neighborUpdate) {
        std::lock_guard<std::mutex> g(listenersMutex_);
        for (auto& listener : listeners_.accessAllThreads()) {
          LOG(INFO) << "Sending notification to bgpD";
          listener.eventBase->runInEventBaseThread(
              [this, neighborUpdate, listenerPtr = &listener] {
                LOG(INFO) << "firing off notification";
                invokeNeighborListeners(listenerPtr, neighborUpdate);
              });
        }
      });
}

NetlinkFibHandler::~NetlinkFibHandler() {}

template <class A>
folly::Expected<int16_t, bool>
NetlinkFibHandler::getProtocol(folly::Promise<A>& promise, int16_t clientId) {
  auto ret = thrift::Platform_constants::clientIdtoProtocolId().find(clientId);
  if (ret == thrift::Platform_constants::clientIdtoProtocolId().end()) {
    auto ex =
        fbnl::NlException(folly::sformat("Invalid ClientId : {}", clientId));
    promise.setException(ex);
    return folly::makeUnexpected(false);
  }
  if (ret->second < kMinRouteProtocolId || ret->second > kMaxRouteProtocolId) {
    auto ex = fbnl::NlException(
        folly::sformat("Invalid Protocol Id : {}", ret->second));
    promise.setException(ex);
    return folly::makeUnexpected(false);
  }
  return ret->second;
}

std::string
NetlinkFibHandler::getClientName(const int16_t clientId) {
  return apache::thrift::util::enumNameSafe(
      static_cast<thrift::FibClient>(clientId));
}

uint8_t
NetlinkFibHandler::protocolToPriority(const uint8_t protocol) {
  // Lookup in protocol to priority mapping
  auto& priorityMap = openr::thrift::Platform_constants::protocolIdtoPriority();
  auto priorityIt = priorityMap.find(protocol);
  if (priorityIt != priorityMap.end()) {
    return priorityIt->second;
  }

  // Default priority is unknown
  return openr::thrift::Platform_constants::kUnknowProtAdminDistance();
}

std::vector<thrift::NextHopThrift>
NetlinkFibHandler::buildNextHops(const fbnl::NextHopSet& nextHops) {
  std::vector<thrift::NextHopThrift> thriftNextHops;

  for (auto const& nh : nextHops) {
    CHECK(nh.getGateway().has_value());
    const auto& ifName = nh.getIfIndex().has_value()
        ? getIfName(nh.getIfIndex().value()).value()
        : "";
    thrift::NextHopThrift nextHop;
    nextHop.address = toBinaryAddress(nh.getGateway().value());
    nextHop.address.ifName_ref() = ifName;
    auto labelAction = nh.getLabelAction();
    if (labelAction.has_value()) {
      if (labelAction.value() == thrift::MplsActionCode::POP_AND_LOOKUP ||
          labelAction.value() == thrift::MplsActionCode::PHP) {
        nextHop.mplsAction_ref() = createMplsAction(labelAction.value());
      } else if (labelAction.value() == thrift::MplsActionCode::SWAP) {
        nextHop.mplsAction_ref() =
            createMplsAction(labelAction.value(), nh.getSwapLabel().value());
      } else if (labelAction.value() == thrift::MplsActionCode::PUSH) {
        nextHop.mplsAction_ref() = createMplsAction(
            labelAction.value(), std::nullopt, nh.getPushLabels().value());
      }
    }
    thriftNextHops.emplace_back(std::move(nextHop));
  }
  return thriftNextHops;
}

std::vector<thrift::UnicastRoute>
NetlinkFibHandler::toThriftUnicastRoutes(const fbnl::NlUnicastRoutes& routeDb) {
  std::vector<thrift::UnicastRoute> routes;

  for (auto const& kv : routeDb) {
    thrift::UnicastRoute route;
    route.dest = toIpPrefix(kv.first);
    route.nextHops = buildNextHops(kv.second.getNextHops());
    routes.emplace_back(std::move(route));
  }
  return routes;
}

std::vector<thrift::MplsRoute>
NetlinkFibHandler::toThriftMplsRoutes(const fbnl::NlMplsRoutes& routeDb) {
  std::vector<thrift::MplsRoute> routes;

  for (auto const& kv : routeDb) {
    thrift::MplsRoute route;
    route.topLabel = kv.first;
    route.nextHops = buildNextHops(kv.second.getNextHops());

    routes.emplace_back(std::move(route));
  }
  return routes;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoute(
    int16_t clientId, std::unique_ptr<thrift::UnicastRoute> route) {
  VLOG(1) << "Adding/Updating route for " << toString(route->dest);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  return netlinkSocket_->addRoute(buildRoute(*route, protocol.value()));
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteUnicastRoute(
    int16_t clientId, std::unique_ptr<thrift::IpPrefix> prefix) {
  VLOG(1) << "Deleting route for " << toString(*prefix);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setDestination(toIPNetwork(*prefix))
      .setProtocolId(protocol.value());
  return netlinkSocket_->delRoute(rtBuilder.build());
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addUnicastRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Adding/Updates routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  // Run all route updates in a single eventloop
  evl_->runImmediatelyOrInEventLoop([this,
                                     clientId,
                                     promise = std::move(promise),
                                     routes = std::move(routes)]() mutable {
    for (auto& route : *routes) {
      auto ptr = std::make_unique<thrift::UnicastRoute>(std::move(route));
      try {
        // This is going to be synchronous call as we are invoking from
        // within event loop
        future_addUnicastRoute(clientId, std::move(ptr)).get();
      } catch (std::exception const& e) {
        promise.setException(e);
        return;
      }
    }
    promise.setValue();
  });

  return future;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteUnicastRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) {
  LOG(INFO) << "Deleting routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     clientId,
                                     promise = std::move(promise),
                                     prefixes = std::move(prefixes)]() mutable {
    for (auto& prefix : *prefixes) {
      auto ptr = std::make_unique<thrift::IpPrefix>(std::move(prefix));
      try {
        future_deleteUnicastRoute(clientId, std::move(ptr)).get();
      } catch (std::exception const& e) {
        promise.setException(e);
        return;
      }
    }
    promise.setValue();
  });

  return future;
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addMplsRoute(
    int16_t clientId, std::unique_ptr<thrift::MplsRoute> route) {
  VLOG(1) << "Adding/Updating MPLS route for " << route->topLabel;

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }
  // TODO: Replace this with addRoute()
  return netlinkSocket_->addMplsRoute(buildMplsRoute(*route, protocol.value()));
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteMplsRoute(int16_t clientId, int32_t topLabel) {
  VLOG(1) << "Deleting mpls route " << topLabel;

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setMplsLabel(topLabel).setProtocolId(protocol.value());
  // TODO: Replace this with deleteRoute()
  return netlinkSocket_->delMplsRoute(rtBuilder.build());
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_addMplsRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::MplsRoute>> routes) {
  LOG(INFO) << "Adding/Updates routes of client: " << getClientName(clientId);

  std::vector<folly::Future<folly::Unit>> futures;
  for (auto& route : *routes) {
    auto ptr = std::make_unique<thrift::MplsRoute>(std::move(route));
    futures.emplace_back(future_addMplsRoute(clientId, std::move(ptr)));
  }

  return collectAllUnsafe(std::move(futures))
      .then([](folly::Try<std::vector<folly::Try<folly::Unit>>>&& retvals) {
        for (auto& retval : retvals.value()) {
          retval.value(); // Throw exception if any
        }
        return folly::Unit(); // Return unit on success
      });
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_deleteMplsRoutes(
    int16_t clientId, std::unique_ptr<std::vector<int32_t>> topLabels) {
  LOG(INFO) << "Deleting mpls routes of client: " << getClientName(clientId);

  std::vector<folly::Future<folly::Unit>> futures;
  for (auto& label : *topLabels) {
    futures.emplace_back(future_deleteMplsRoute(clientId, label));
  }

  return collectAllUnsafe(std::move(futures))
      .then([](folly::Try<std::vector<folly::Try<folly::Unit>>>&& retvals) {
        for (auto& retval : retvals.value()) {
          retval.value(); // Throw exception if any
        }
        return folly::Unit(); // Return unit on success
      });
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_syncFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Syncing FIB with provided routes. Client: "
            << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  // Build new routeDb
  fbnl::NlUnicastRoutes newRoutes;
  for (auto const& route : *routes) {
    newRoutes.emplace(
        toIPNetwork(route.dest), buildRoute(route, protocol.value()));
  }

  // TODO: Implement sync routes in here. `NetlinkSocket` is deprecated
  return netlinkSocket_->syncUnicastRoutes(
      protocol.value(), std::move(newRoutes));
}

folly::Future<folly::Unit>
NetlinkFibHandler::future_syncMplsFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::MplsRoute>> mplsRoutes) {
  LOG(INFO) << "Syncing MPLS FIB with provided routes. Client: "
            << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  fbnl::NlMplsRoutes newMplsRoutes;
  for (auto const& mplsRoute : *mplsRoutes) {
    newMplsRoutes.emplace(
        mplsRoute.topLabel, buildMplsRoute(mplsRoute, protocol.value()));
  }

  // TODO: Implement sync mpls routes in here. `NetlinkSocket` is deprecated
  return netlinkSocket_->syncMplsRoutes(
      protocol.value(), std::move(newMplsRoutes));
}

int64_t
NetlinkFibHandler::aliveSince() {
  return startTime_;
}

facebook::fb303::cpp2::fb303_status
NetlinkFibHandler::getStatus() {
  VLOG(3) << "Received getStatus";
  return facebook::fb303::cpp2::fb303_status::ALIVE;
}

openr::thrift::SwitchRunState
NetlinkFibHandler::getSwitchRunState() {
  VLOG(3) << "Received getSwitchRunState";
  return openr::thrift::SwitchRunState::CONFIGURED;
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
NetlinkFibHandler::future_getRouteTableByClient(int16_t clientId) {
  LOG(INFO) << "Get unicast routes from FIB for clientId " << clientId;

  // promise here is used only for error case
  folly::Promise<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
      promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  // TODO: Replace this with `getIPv4Routes(protocol)` and
  // `getIPv6Routes(protocol)`
  return netlinkSocket_->getCachedUnicastRoutes(protocol.value())
      .thenValue([this](fbnl::NlUnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            toThriftUnicastRoutes(res));
      })
      .thenError<std::runtime_error>([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get unicast routing table by client: "
                   << ex.what() << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>();
      });
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>
NetlinkFibHandler::future_getMplsRouteTableByClient(int16_t clientId) {
  LOG(INFO) << "Get Mpls routes from FIB for clientId " << clientId;

  // promise here is used only for error case
  folly::Promise<std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>
      promise;
  auto future = promise.getFuture();
  auto protocol = getProtocol(promise, clientId);
  if (protocol.hasError()) {
    return future;
  }

  // TODO: Replace this with `getMplsRoutes(protocol)`
  return netlinkSocket_->getCachedMplsRoutes(protocol.value())
      .thenValue([this](fbnl::NlMplsRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::MplsRoute>>(
            toThriftMplsRoutes(res));
      })
      .thenError<std::runtime_error>([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get Mpls routing table by client: "
                   << ex.what() << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::MplsRoute>>();
      });
}
void
NetlinkFibHandler::buildMplsAction(
    fbnl::NextHopBuilder& nhBuilder, const thrift::NextHopThrift& nhop) {
  if (!nhop.mplsAction_ref().has_value()) {
    return;
  }
  auto mplsAction = nhop.mplsAction.value();
  nhBuilder.setLabelAction(mplsAction.action);
  if (mplsAction.action == thrift::MplsActionCode::SWAP) {
    if (!mplsAction.swapLabel_ref().has_value()) {
      throw fbnl::NlException("Swap label not provided");
    }
    nhBuilder.setSwapLabel(mplsAction.swapLabel_ref().value());
  } else if (mplsAction.action == thrift::MplsActionCode::PUSH) {
    if (!mplsAction.pushLabels_ref().has_value()) {
      throw fbnl::NlException("Push label(s) not provided");
    }
    nhBuilder.setPushLabels(mplsAction.pushLabels_ref().value());
  } else if (mplsAction.action == thrift::MplsActionCode::POP_AND_LOOKUP) {
    auto lpbkIfIndex = getLoopbackIfIndex();
    if (lpbkIfIndex.has_value()) {
      nhBuilder.setIfIndex(lpbkIfIndex.value());
    } else {
      throw fbnl::NlException("POP action, loopback interface not available");
    }
  }
  return;
}

void
NetlinkFibHandler::buildNextHop(
    fbnl::RouteBuilder& rtBuilder,
    const std::vector<thrift::NextHopThrift>& nhop) {
  // add nexthops
  fbnl::NextHopBuilder nhBuilder;
  for (const auto& nh : nhop) {
    if (nh.address.ifName_ref()) {
      nhBuilder.setIfIndex(getIfIndex(*nh.address.ifName_ref()).value());
    }
    nhBuilder.setGateway(toIPAddress(nh.address));
    buildMplsAction(nhBuilder, nh);
    rtBuilder.addNextHop(nhBuilder.setWeight(0).build());
    nhBuilder.reset();
  }
}

fbnl::Route
NetlinkFibHandler::buildRoute(const thrift::UnicastRoute& route, int protocol) {
  // Create route object
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setDestination(toIPNetwork(route.dest))
      .setProtocolId(protocol)
      .setPriority(protocolToPriority(protocol))
      .setFlags(0)
      .setValid(true);

  if (route.nextHops.empty()) {
    // Empty nexthops is same as DROP (aka RTN_BLACKHOLE)
    rtBuilder.setType(RTN_BLACKHOLE);
  } else {
    // Add nexthops
    buildNextHop(rtBuilder, route.nextHops);
  }

  return rtBuilder.build();
}

fbnl::Route
NetlinkFibHandler::buildMplsRoute(
    const thrift::MplsRoute& mplsRoute, int protocol) {
  // Craete route object
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setMplsLabel(static_cast<uint32_t>(mplsRoute.topLabel))
      .setProtocolId(protocol)
      .setPriority(protocolToPriority(protocol))
      .setFlags(0)
      .setValid(true);

  if (mplsRoute.nextHops.empty()) {
    // Empty nexthops is same as DROP (aka RTN_BLACKHOLE)
    rtBuilder.setType(RTN_BLACKHOLE);
  } else {
    // Add nexthops
    buildNextHop(rtBuilder, mplsRoute.nextHops);
  }

  return rtBuilder.setValid(true).build();
}

std::optional<int>
NetlinkFibHandler::getIfIndex(const std::string& ifName) {
  // Lambda function to lookup ifName in cache
  auto getCachedIndex = [this, &ifName]() -> std::optional<int> {
    auto cache = ifNameToIndex_.rlock();
    auto it = cache->find(ifName);
    if (it != cache->end()) {
      return it->second;
    }
    return std::nullopt;
  };

  // Lookup in cache. Return if exists
  auto maybeIndex = getCachedIndex();
  if (maybeIndex.has_value()) {
    return maybeIndex;
  }

  // Update cache and return cached index
  initializeInterfaceCache();
  return getCachedIndex();
}

std::optional<std::string>
NetlinkFibHandler::getIfName(const int ifIndex) {
  // Lambda function to lookup ifIndex in cache
  auto getCachedName = [this, ifIndex]() -> std::optional<std::string> {
    auto cache = ifIndexToName_.rlock();
    auto it = cache->find(ifIndex);
    if (it != cache->end()) {
      return it->second;
    }
    return std::nullopt;
  };

  // Lookup in cache. Return if exists
  auto maybeName = getCachedName();
  if (maybeName.has_value()) {
    return maybeName;
  }

  // Update cache and return cached index
  initializeInterfaceCache();
  return getCachedName();
}

std::optional<int>
NetlinkFibHandler::getLoopbackIfIndex() {
  auto index = loopbackIfIndex_.load();
  if (index < 0) {
    initializeInterfaceCache();
    index = loopbackIfIndex_.load();
  }

  if (index < 0) {
    return std::nullopt;
  }
  return index;
}

void
NetlinkFibHandler::initializeInterfaceCache() noexcept {
  auto links = netlinkSocket_->getProtocolSocket()->getAllLinks().get();

  // Acquire locks on the cache
  auto lockedIfNameToIndex = ifNameToIndex_.wlock();
  auto lockedIfIndexToName = ifIndexToName_.wlock();

  // NOTE: We don't clear cache instead override entries
  for (auto const& link : links) {
    // Update name <-> index mappings
    (*lockedIfNameToIndex)[link.getLinkName()] = link.getIfIndex();
    (*lockedIfIndexToName)[link.getIfIndex()] = link.getLinkName();

    // Update loopbackIfIndex_
    if (link.isLoopback()) {
      loopbackIfIndex_.store(link.getIfIndex());
    }
  }
}

void
NetlinkFibHandler::getCounters(std::map<std::string, int64_t>& counters) {
  // TODO: Since this is stateless, we won't be able to report this counter
  // anymore. Anyway FBOSS doesn't report this similar counter.
  counters["fibagent.num_of_routes"] = netlinkSocket_->getRouteCount().get();
}

void
NetlinkFibHandler::sendNeighborDownInfo(
    std::unique_ptr<std::vector<std::string>> neighborIp) {
  fbnl::NetlinkSocket::NeighborUpdate neighborUpdate;
  neighborUpdate.delNeighbors(*neighborIp);
  std::lock_guard<std::mutex> g(listenersMutex_);
  for (auto& listener : listeners_.accessAllThreads()) {
    LOG(INFO) << "Sending notification to bgpD";
    listener.eventBase->runInEventBaseThread(
        [this, neighborUpdate, listenerPtr = &listener] {
          LOG(INFO) << "firing off notification";
          invokeNeighborListeners(listenerPtr, neighborUpdate);
        });
  }
}

void
NetlinkFibHandler::async_eb_registerForNeighborChanged(
    std::unique_ptr<apache::thrift::HandlerCallback<void>> cb) {
  auto ctx = cb->getConnectionContext()->getConnectionContext();
  auto client = ctx->getDuplexClient<
      thrift::NeighborListenerClientForFibagentAsyncClient>();

  LOG(INFO) << "registered for bgp";
  std::lock_guard<std::mutex> g(listenersMutex_);
  auto info = listeners_.get();
  CHECK(cb->getEventBase()->isInEventBaseThread());
  if (!info) {
    info = new ThreadLocalListener(cb->getEventBase());
    listeners_.reset(info);
  }

  // make sure the eventbase is same, because later we want to run callback in
  // cb's eventbase
  DCHECK_EQ(info->eventBase, cb->getEventBase());
  if (!info->eventBase) {
    info->eventBase = cb->getEventBase();
  }
  info->clients.clear();
  info->clients.emplace(ctx, client);
  LOG(INFO) << "registered for bgp success";
  cb->done();
}

void
NetlinkFibHandler::invokeNeighborListeners(
    ThreadLocalListener* listener,
    fbnl::NetlinkSocket::NeighborUpdate neighborUpdate) {
  // Collect the iterators to avoid erasing and potentially reordering
  // the iterators in the list.
  for (const auto& ctx : brokenClients_) {
    listener->clients.erase(ctx);
  }
  brokenClients_.clear();
  for (auto& client : listener->clients) {
    auto clientDone = [&](apache::thrift::ClientReceiveState&& state) {
      try {
        thrift::NeighborListenerClientForFibagentAsyncClient::
            recv_neighborsChanged(state);
      } catch (const std::exception& ex) {
        LOG(ERROR) << "Exception in neighbor listener: " << ex.what();
        brokenClients_.push_back(client.first);
      }
    };
    client.second->neighborsChanged(
        clientDone,
        neighborUpdate.getAddedNeighbor(),
        neighborUpdate.getRemovedNeighbor());
  }
}

} // namespace openr
