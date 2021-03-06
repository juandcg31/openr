/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <forward_list>
#include <mutex>
#include <thread>

#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/ThreadLocal.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sodium.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/spark/IoProvider.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/spark/tests/MockIoProvider.h>

using namespace openr;

using apache::thrift::CompactSerializer;

namespace {
const std::string iface1{"iface1"};
const std::string iface2{"iface2"};

const int ifIndex1{1};
const int ifIndex2{2};

const folly::CIDRNetwork ip1V4 =
    folly::IPAddress::createNetwork("192.168.0.1", 24, false /* apply mask */);
const folly::CIDRNetwork ip2V4 =
    folly::IPAddress::createNetwork("192.168.0.2", 24, false /* apply mask */);

const folly::CIDRNetwork ip1V6 = folly::IPAddress::createNetwork("fe80::1/128");
const folly::CIDRNetwork ip2V6 = folly::IPAddress::createNetwork("fe80::2/128");

// Domain name (same for all Tests except in DomainTest)
const std::string kDomainName("Fire_and_Blood");

// the URL for the spark server
const std::string kSparkCounterCmdUrl("inproc://spark_server_counter_cmd");

// the hold time we use during the tests
const std::chrono::milliseconds kGRHoldTime(500);

// the keep-alive for spark2 hello messages
const std::chrono::milliseconds kKeepAliveTime(50);

// the time interval for spark2 hello msg
const std::chrono::milliseconds kHelloTime(200);

// the time interval for spark2 handhshake msg
const std::chrono::milliseconds kHandshakeTime(50);

// the time interval for spark2 heartbeat msg
const std::chrono::milliseconds kHeartbeatTime(50);

// the hold time for spark2 negotiate stage
const std::chrono::milliseconds kNegotiateHoldTime(500);

// the hold time for spark2 heartbeat msg
const std::chrono::milliseconds kHeartbeatHoldTime(200);
}; // namespace

class Spark2Fixture : public testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider = std::make_shared<MockIoProvider>();

    // Start mock IoProvider thread
    mockIoProviderThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting mockIoProvider thread.";
      mockIoProvider->start();
      LOG(INFO) << "mockIoProvider thread got stopped.";
    });
    mockIoProvider->waitUntilRunning();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping mockIoProvider thread.";
    mockIoProvider->stop();
    mockIoProviderThread->join();
  }

  std::shared_ptr<SparkWrapper>
  createSpark(
      std::string const& domainName,
      std::string const& myNodeName,
      uint32_t spark2Id,
      bool enableSpark2 = true,
      bool increaseHelloInterval = true,
      std::shared_ptr<thrift::OpenrConfig> config = nullptr,
      std::chrono::milliseconds grHoldTime = kGRHoldTime,
      std::chrono::milliseconds keepAliveTime = kKeepAliveTime,
      std::chrono::milliseconds fastInitKeepAliveTime = kKeepAliveTime,
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      SparkTimeConfig timeConfig = SparkTimeConfig(
          kHelloTime,
          kKeepAliveTime,
          kHandshakeTime,
          kHeartbeatTime,
          kNegotiateHoldTime,
          kHeartbeatHoldTime)) {
    return std::make_unique<SparkWrapper>(
        domainName,
        myNodeName,
        grHoldTime,
        keepAliveTime,
        fastInitKeepAliveTime,
        true, /* enableV4 */
        version,
        context,
        mockIoProvider,
        config,
        enableSpark2,
        increaseHelloInterval,
        timeConfig);
  }

  fbzmq::Context context;
  std::shared_ptr<MockIoProvider> mockIoProvider{nullptr};
  std::unique_ptr<std::thread> mockIoProviderThread{nullptr};
  CompactSerializer serializer_;
};

class SimpleSpark2Fixture : public Spark2Fixture {
 protected:
  void
  createAndConnectSpark2Nodes() {
    // Define interface names for the test
    mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

    // connect interfaces directly
    ConnectedIfPairs connectedPairs = {
        {iface1, {{iface2, 10}}},
        {iface2, {{iface1, 10}}},
    };
    mockIoProvider->setConnectedPairs(connectedPairs);

    // start one spark2 instance
    node1 = createSpark(kDomainName, "node-1", 1);

    // start another spark2 instance
    node2 = createSpark(kDomainName, "node-2", 2);

    // start tracking iface1
    EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

    // start tracking iface2
    EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

    LOG(INFO) << "Start to receive messages from Spark2";

    // Now wait for sparks to detect each other
    {
      auto event =
          node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
      ASSERT_TRUE(event.has_value());
      EXPECT_EQ(iface1, event->ifName);
      EXPECT_EQ("node-2", event->neighbor.nodeName);
      EXPECT_EQ(
          std::make_pair(ip2V4.first, ip2V6.first),
          SparkWrapper::getTransportAddrs(*event));
      LOG(INFO) << "node-1 reported adjacency to node-2";
    }

    {
      auto event =
          node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
      ASSERT_TRUE(event.has_value());
      EXPECT_EQ(iface2, event->ifName);
      EXPECT_EQ("node-1", event->neighbor.nodeName);
      EXPECT_EQ(
          std::make_pair(ip1V4.first, ip1V6.first),
          SparkWrapper::getTransportAddrs(*event));
      LOG(INFO) << "node-2 reported adjacency to node-1";
    }
  }

  std::shared_ptr<SparkWrapper> node1;
  std::shared_ptr<SparkWrapper> node2;
};

//
// Start 2 Spark instances and wait them forming adj. Then
// increase/decrease RTT, expect NEIGHBOR_RTT_CHANGE event
//
TEST_F(SimpleSpark2Fixture, RttTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture RttTest finished";
  };

  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  LOG(INFO) << "Change rtt between nodes to 40ms (asymmetric)";

  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 15}}},
      {iface2, {{iface1, 25}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // wait for spark nodes to detecct Rtt change
  {
    auto event = node1->waitForEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE);
    ASSERT_TRUE(event.has_value());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (40 - 10) * 1000);
    EXPECT_LE(event->rttUs, (40 + 10) * 1000);
    LOG(INFO) << "node-1 reported new RTT to node-2 to be "
              << event->rttUs / 1000.0 << "ms";
  }

  {
    auto event = node2->waitForEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RTT_CHANGE);
    ASSERT_TRUE(event.has_value());
    // 25% tolerance
    EXPECT_GE(event->rttUs, (40 - 10) * 1000);
    EXPECT_LE(event->rttUs, (40 + 10) * 1000);
    LOG(INFO) << "node-2 reported new RTT to node-1 to be "
              << event->rttUs / 1000.0 << "ms";
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// make it uni-directional, expect both side to lose adj
// due to missing node info in `ReflectedNeighborInfo`
//
TEST_F(SimpleSpark2Fixture, UnidirectionTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fxiture UnidirectionTest finished";
  };

  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  LOG(INFO) << "Stopping communications from iface2 to iface1";

  // stop packet flowing iface2 -> iface1. Expect both ends drops
  //  1. node1 drops due to: heartbeat hold timer expired
  //  2. node2 drops due to: helloMsg doesn't contains neighborInfo
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // wait for sparks to lose each other
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-1 reported down adjacency to node-2";
  }

  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-2 reported down adjacency to node-1";
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// restart one of them within GR window, make sure we get neighbor
// "RESTARTED" event due to graceful restart window.
//
TEST_F(SimpleSpark2Fixture, GRTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture GracefulRestartTest finished";
  };

  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  // Kill node2
  LOG(INFO) << "Kill and restart node-2";

  node2.reset();

  // node-1 should report node-2 as 'RESTARTING'
  {
    auto event = node1->waitForEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-1 reported node-2 as RESTARTING";
  }

  node2 = createSpark(kDomainName, "node-2", 3 /* spark2Id change */);

  LOG(INFO) << "Adding iface2 to node-2 to let it start helloMsg adverstising";

  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  // node-1 should report node-2 as 'RESTARTED' when receiving helloMsg
  // with wrapped seqNum
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-1 reported node-2 as 'RESTARTED'";
  }

  // node-2 should ultimately report node-1 as 'UP'
  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  // should NOT receive any event( e.g.NEIGHBOR_DOWN)
  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node2
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// gracefully shut down one of them but NOT bring it back,
// make sure we get neighbor "DOWN" event due to GR timer expiring.
//
TEST_F(SimpleSpark2Fixture, GRTimerExpireTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture GRTimerExpiredTest finished";
  };

  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  // Kill node2
  LOG(INFO) << "Kill and restart node-2";

  auto startTime = std::chrono::steady_clock::now();
  node2.reset();

  // Since node2 doesn't come back, will lose adj and declare DOWN
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-1 reporte down adjacency to node-2";

    // Make sure 'down' event is triggered by GRTimer expire
    // and NOT related with heartbeat holdTimer( no hearbeatTimer started )
    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime >= kGRHoldTime);
    ASSERT_TRUE(endTime - startTime <= kGRHoldTime + kHeartbeatHoldTime);
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// stop the bi-direction communication from each other.
// Observe neighbor going DOWN due to hold timer expiration.
//
TEST_F(SimpleSpark2Fixture, HeartbeatTimerExpireTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture HeartbeatTimerExpireTest finished";
  };

  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  // record time for future comparison
  auto startTime = std::chrono::steady_clock::now();

  // remove underneath connections between to nodes
  ConnectedIfPairs connectedPairs = {};
  mockIoProvider->setConnectedPairs(connectedPairs);

  // wait for sparks to lose each other
  {
    LOG(INFO) << "Waiting for both nodes to time out with each other";

    auto event1 =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event1.has_value());

    auto event2 =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event2.has_value());

    // record time for expiration time test
    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime >= kHeartbeatHoldTime);
    ASSERT_TRUE(endTime - startTime <= kGRHoldTime);
  }
}

//
// Start 2 Spark instances and wait them forming adj. Then
// remove/add interface from one instance's perspective
//
TEST_F(SimpleSpark2Fixture, InterfaceRemovalTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture InterfaceRemovalTest finished";
  };

  // create Spark2 instances and establish connections
  createAndConnectSpark2Nodes();

  auto startTime = std::chrono::steady_clock::now();

  // tell node1 to remove interface to mimick request from linkMonitor
  EXPECT_TRUE(node1->updateInterfaceDb({}));

  LOG(INFO) << "Waiting for node-1 to report loss of adj to node-2";

  // since the removal of intf happens instantly. down event should
  // be reported ASAP.
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(
        endTime - startTime <= std::min(kGRHoldTime, kHeartbeatHoldTime));
    LOG(INFO)
        << "node-1 reported down adjacency to node-2 due to interface removal";
  }

  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_DOWN);
    ASSERT_TRUE(event.has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime <= kGRHoldTime);
    LOG(INFO)
        << "node-2 reported down adjacency to node-2 due to heartbeat expired";
  }

  {
    // should NOT receive any event after down adj
    EXPECT_TRUE(node1->recvNeighborEvent(kGRHoldTime).hasError());
    EXPECT_TRUE(node2->recvNeighborEvent(kGRHoldTime).hasError());
  }

  // Resume interface connection
  LOG(INFO) << "Bringing iface-1 back online";

  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  startTime = std::chrono::steady_clock::now();

  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime <= kNegotiateHoldTime + kHeartbeatHoldTime);
    LOG(INFO) << "node-1 reported up adjacency to node-2";
  }

  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.has_value());

    auto endTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(endTime - startTime <= kNegotiateHoldTime + kHeartbeatHoldTime);
    LOG(INFO) << "node-2 reported up adjacency to node-1";
  }
}

//
// Start 2 Spark instances within different domains. Then
// make sure they can't form adj as helloMsg being ignored.
//
TEST_F(Spark2Fixture, DomainTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture DomainTest finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start 2 spark instances within different domain
  std::string domainLannister = "A_Lannister_Always_Pays_His_Debts";
  std::string domainStark = "Winter_Is_Coming";
  std::string nodeLannister = "Lannister";
  std::string nodeStark = "Stark";
  auto node1 = createSpark(domainLannister, nodeLannister, 1);
  auto node2 = createSpark(domainStark, nodeStark, 2);

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node2
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node1->getSparkNeighState(iface1, nodeStark).has_value());
    EXPECT_FALSE(node2->getSparkNeighState(iface2, nodeLannister).has_value());
  }
}

//
// Start 2 Spark instances, but block one from hearing another. Then
// shutdown the peer that cannot hear, and make sure there is no DOWN
// event generated for this one.
//
TEST_F(Spark2Fixture, IgnoreUnidirectionalPeer) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture IgnoreUnidirectionalPeerTest finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start one spark2 instance
  auto node1 = createSpark(kDomainName, "node-1", 1);

  // start another spark2 instance
  auto node2 = createSpark(kDomainName, "node-2", 2);

  // start tracking iface1
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

  // start tracking iface2
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_TRUE(node1->recvNeighborEvent(kGRHoldTime * 2).hasError());
    LOG(INFO) << "node-1 doesn't have any neighbor event";

    EXPECT_TRUE(node2->recvNeighborEvent(kGRHoldTime * 2).hasError());
    LOG(INFO) << "node-2 doesn't have any neighbor event";
  }

  {
    // check for neighbor state on node1, should be WARM
    // since will NOT receive helloMsg containing my own info
    EXPECT_TRUE(
        node1->getSparkNeighState(iface1, "node-2") == SparkNeighState::WARM);
    LOG(INFO) << "node-1 have neighbor: node-2 in WARM state";

    // check for neighbor state on node2, should return std::nullopt
    // since node2 can't receive pkt from node1
    EXPECT_FALSE(node2->getSparkNeighState(iface2, "node-1").has_value());
    LOG(INFO) << "node-2 doesn't have any neighbor";
  }
}

//
// Start an old Spark instance and another Spark2 instance and
// make sure they can form adj due to backward compatibiility.
//
TEST_F(Spark2Fixture, BackwardCompatibilityTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture BackwardCompatibilityTest finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start one spark2 instance
  auto node1 = createSpark(kDomainName, "node-1", 1, true, false);

  // start one old spark instance
  auto node2 = createSpark(kDomainName, "node-2", 2, false, false);

  // start tracking iface1
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

  // start tracking iface2
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  LOG(INFO) << "Wait spark2 and old spark instances to form adj";

  // Now wait for sparks to detect each other
  {
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(iface1, event->ifName);
    EXPECT_EQ("node-2", event->neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip2V4.first, ip2V6.first),
        SparkWrapper::getTransportAddrs(*event));
    LOG(INFO) << "node-1 reported adjacency to node-2";
  }

  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(iface2, event->ifName);
    EXPECT_EQ("node-1", event->neighbor.nodeName);
    EXPECT_EQ(
        std::make_pair(ip1V4.first, ip1V6.first),
        SparkWrapper::getTransportAddrs(*event));
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  // now let old spark instance restart and BECOMES spark2 instance
  // to mimick an upgrade
  {
    node2.reset();

    // node-1 will report node-2 as RESTARTING
    auto event = node1->waitForEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-1 reported node-2 restarting";

    // create a new Spark2 instead of old Spark
    node2 = createSpark(kDomainName, "node-2", 3 /* spark2Id change */);

    LOG(INFO)
        << "Adding iface2 to node-2 to let it start helloMsg adverstising";

    EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));
  }

  {
    // node-1 will finally report node-2 as RESTARTED
    auto event =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-1 reported node-2 as 'RESTARTED'";
  }

  // node-2 should ultimately report node-1 as 'UP'
  {
    auto event =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event.has_value());
    LOG(INFO) << "node-2 reported adjacency to node-1";
  }

  // should NOT receive any event( e.g.NEIGHBOR_DOWN)
  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node2
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
  }
}

//
// Start 1 Spark instace and make its interfaces connected to its own
// Make sure pkt loop can be handled gracefully and no ADJ will be formed.
//
TEST_F(Spark2Fixture, LoopedHelloPktTest) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture LoopedHelloPktTest finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}});

  // connect iface1 directly with itself to mimick
  // self-looped helloPkt
  ConnectedIfPairs connectedPairs = {
      {iface1, {{iface1, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start one spark2 instance
  auto node1 = createSpark(kDomainName, "node-1", 1);

  // start tracking iface1.
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));

  // should NOT receive any event( e.g.NEIGHBOR_DOWN)
  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node1->getSparkNeighState(iface1, "node-1").has_value());
  }
}

//
// Start 2 Spark instances within different v4 subnet. Then
// make sure they can't form adj as NEGOTIATION failed. Bring
// down the interface and make sure no crash happened for tracked
// neighbors. Then put them in same subnet, make sure instances
// will form adj with each other.
//
TEST_F(Spark2Fixture, LinkDownWithoutAdjFormed) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture LinkDownWithoutAdjFormed finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark2 instances
  auto node1 = createSpark(kDomainName, "node-1", 1);
  auto node2 = createSpark(kDomainName, "node-2", 2);

  // enable v4 subnet validation to put adddres in different /31 subnet
  // on purpose.
  const folly::CIDRNetwork ip1V4WithSubnet =
      folly::IPAddress::createNetwork("192.168.0.2", 31);
  const folly::CIDRNetwork ip2V4WithSameSubnet =
      folly::IPAddress::createNetwork("192.168.0.3", 31);
  const folly::CIDRNetwork ip2V4WithDiffSubnet =
      folly::IPAddress::createNetwork("192.168.0.4", 31);

  // start tracking iface1
  EXPECT_TRUE(
      node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4WithSubnet, ip1V6}}));

  // start tracking iface2
  EXPECT_TRUE(node2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4WithDiffSubnet, ip2V6}}));

  // won't form adj as v4 validation should fail
  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());

    EXPECT_FALSE(node2
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
  }

  {
    // bring down interface of node1 to make sure no crash happened
    EXPECT_TRUE(node1->updateInterfaceDb({}));

    // bring up interface of node1 to make sure no crash happened
    EXPECT_TRUE(
        node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4WithSubnet, ip1V6}}));
  }

  {
    // bring up interface with SAME subnet and verify ADJ UP event
    EXPECT_TRUE(node2->updateInterfaceDb(
        {{iface2, ifIndex2, ip2V4WithSameSubnet, ip2V6}}));

    auto event1 =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event1.has_value());

    auto event2 =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event2.has_value());
    LOG(INFO) << "node-1 and node-2 successfully form adjacency";
  }
}

//
// Start 2 Spark instances within different v4 subnet. Then
// make sure they can't form adj as NEGOTIATION failed. Check
// neighbor state within NEGOTIATE/WARM depending on whether
// new helloMsg is received.
//
TEST_F(Spark2Fixture, InvalidV4Subnet) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture InvalidV4Subnet finished";
  };

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  // start spark2 instances
  std::string nodeName1 = "node-1";
  std::string nodeName2 = "node-2";
  auto node1 = createSpark(kDomainName, nodeName1, 1);
  auto node2 = createSpark(kDomainName, nodeName2, 2);

  // enable v4 subnet validation to put adddres in different /31 subnet
  // on purpose.
  const folly::CIDRNetwork ip1V4WithSubnet =
      folly::IPAddress::createNetwork("192.168.0.2", 31);
  const folly::CIDRNetwork ip2V4WithDiffSubnet =
      folly::IPAddress::createNetwork("192.168.0.4", 31);

  // start tracking iface1 and iface2
  EXPECT_TRUE(
      node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4WithSubnet, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb(
      {{iface2, ifIndex2, ip2V4WithDiffSubnet, ip2V6}}));

  // won't form adj as v4 validation should fail
  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());

    EXPECT_FALSE(node2
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
  }

  // check neighbor state: should be in WARM/NEGOTIATE stage
  {
    auto neighState1 = node1->getSparkNeighState(iface1, nodeName2);
    EXPECT_TRUE(
        neighState1 == SparkNeighState::WARM ||
        neighState1 == SparkNeighState::NEGOTIATE);

    auto neighState2 = node2->getSparkNeighState(iface2, nodeName1);
    EXPECT_TRUE(
        neighState2 == SparkNeighState::WARM ||
        neighState2 == SparkNeighState::NEGOTIATE);
  }
}

//
// Positive case for AREA:
//
// Start 2 Spark instances with areaConfig and make sure they
// can form adj with each other in specified AREA.
//
TEST_F(Spark2Fixture, AreaMatch) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture AreaMatch finished";
  };

  // Explicitly set regex to be capital letters to make sure
  // regex is NOT case-sensative
  auto areaConfig11 = SparkWrapper::createAreaConfig("1", {"RSW.*"}, {".*"});
  auto areaConfig12 = SparkWrapper::createAreaConfig("2", {"FSW.*"}, {".*"});
  auto areaConfig21 = SparkWrapper::createAreaConfig("1", {"FSW.*"}, {".*"});
  auto areaConfig22 = SparkWrapper::createAreaConfig("2", {"RSW.*"}, {".*"});

  // RSW: { 1 -> "RSW.*", 2 -> "FSW.*"}
  // FSW: { 1 -> "FSW.*", 2 -> "RSW.*"}
  auto config1 = std::make_shared<thrift::OpenrConfig>();
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config1->areas.emplace_back(areaConfig11);
  config1->areas.emplace_back(areaConfig12);
  config2->areas.emplace_back(areaConfig21);
  config2->areas.emplace_back(areaConfig22);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  LOG(INFO) << "Starting node-1 and node-2...";
  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, config1);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  // RSW001 and FSW002 node should form adj in area 2 due to regex matching
  {
    auto event1 =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(event1.value().neighbor.nodeName, nodeName2);
    EXPECT_EQ(event1.value().area, "2");

    auto event2 =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(event2.value().neighbor.nodeName, nodeName1);
    EXPECT_EQ(event2.value().area, "2");
  }
}

//
// Negative case for AREA:
//
// Start 2 Spark instances with areaConfig and make sure they
// can NOT form adj due to wrong AREA regex matching.
//
TEST_F(Spark2Fixture, NoAreaMatch) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture NoAreaMatch finished";
  };

  // AreaConfig:
  //  rsw001: { 1 -> "RSW.*"}
  //  fsw002: { 1 -> "FSW.*"}
  //
  //  rsw001 and fsw002 will receive each other's helloMsg, but won't proceed.
  //  rsw001 can ONLY pair with "RSW.*", whereas fsw002 can ONLY pair with
  //  "FSW.*".
  auto areaConfig1 = SparkWrapper::createAreaConfig("1", {"RSW.*"}, {".*"});
  auto areaConfig2 = SparkWrapper::createAreaConfig("1", {"FSW.*"}, {".*"});

  auto config1 = std::make_shared<thrift::OpenrConfig>();
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config1->areas.emplace_back(areaConfig1);
  config2->areas.emplace_back(areaConfig2);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  LOG(INFO) << "Starting node-1 and node-2...";
  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, config1);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  // start tracking iface1 and iface2
  EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
  EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));

  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node2
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node1->getSparkNeighState(iface1, nodeName2).has_value());
    EXPECT_FALSE(node2->getSparkNeighState(iface2, nodeName1).has_value());
  }
}

//
// Negative case for AREA:
//
// Start 2 Spark instances with areaConfig and make sure they
// can NOT form adj due to inconsistent AREA negotiation result.
//
TEST_F(Spark2Fixture, InconsistentAreaNegotiation) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture InconsistentArea finished";
  };

  // AreaConfig:
  //  rsw001: { 1 -> "FSW.*"}
  //  fsw002: { 2 -> "RSW.*"}
  //
  //  rsw001 and fsw002 will receive each other's helloMsg and proceed to
  //  NEGOTIATE stage. However, rsw001 thinks fsw002 should reside in
  //  area "1", whereas fsw002 thinks rsw001 should be in area "2".
  //
  //  AREA negotiation won't go through. Will fall back to WARM
  auto areaConfig1 = SparkWrapper::createAreaConfig("1", {"FSW.*"}, {".*"});
  auto areaConfig2 = SparkWrapper::createAreaConfig("2", {"RSW.*"}, {".*"});

  auto config1 = std::make_shared<thrift::OpenrConfig>();
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config1->areas.emplace_back(areaConfig1);
  config2->areas.emplace_back(areaConfig2);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  LOG(INFO) << "Starting node-1 and node-2...";
  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, config1);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  {
    // start tracking iface1 and iface2
    EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
    EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));
  }

  {
    EXPECT_FALSE(node1
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());
    EXPECT_FALSE(node2
                     ->waitForEvent(
                         thrift::SparkNeighborEventType::NEIGHBOR_UP,
                         kGRHoldTime,
                         kGRHoldTime * 2)
                     .has_value());

    auto neighState1 = node1->getSparkNeighState(iface1, nodeName2);
    EXPECT_TRUE(
        neighState1 == SparkNeighState::WARM ||
        neighState1 == SparkNeighState::NEGOTIATE);

    auto neighState2 = node2->getSparkNeighState(iface2, nodeName1);
    EXPECT_TRUE(
        neighState2 == SparkNeighState::WARM ||
        neighState2 == SparkNeighState::NEGOTIATE);
  }
}

//
// Positive case for AREA:
//
// Start 1 Spark without AREA config supported, whereas starting
// another Spark with areaConfig passed in. Make sure they can
// form adj in `defaultArea` for backward compatibility.
//
TEST_F(Spark2Fixture, NoAreaSupportNegotiation) {
  SCOPE_EXIT {
    LOG(INFO) << "Spark2Fixture InconsistentArea finished";
  };

  // AreaConfig:
  //  rsw001: {}
  //  fsw002: { 2 -> "RSW.*"}
  //
  //  rsw001 doesn't know anything about AREA, whereas fsw002 is configured
  //  with areaConfig. Make sure AREA negotiation will go through and they can
  //  form adj inside `defaultArea`.
  auto areaConfig2 = SparkWrapper::createAreaConfig("2", {"RSW.*"}, {".*"});
  auto config2 = std::make_shared<thrift::OpenrConfig>();
  config2->areas.emplace_back(areaConfig2);

  // Define interface names for the test
  mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});

  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface2, {{iface1, 10}}},
      {iface1, {{iface2, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  LOG(INFO) << "Starting node-1 and node-2...";
  std::string nodeName1 = "rsw001";
  std::string nodeName2 = "fsw002";
  auto node1 = createSpark(kDomainName, nodeName1, 1, true, true, nullptr);
  auto node2 = createSpark(kDomainName, nodeName2, 2, true, true, config2);

  {
    // start tracking iface1 and iface2
    EXPECT_TRUE(node1->updateInterfaceDb({{iface1, ifIndex1, ip1V4, ip1V6}}));
    EXPECT_TRUE(node2->updateInterfaceDb({{iface2, ifIndex2, ip2V4, ip2V6}}));
  }

  {
    auto event1 =
        node1->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event1.has_value());
    EXPECT_EQ(event1.value().neighbor.nodeName, nodeName2);
    EXPECT_EQ(event1.value().area, thrift::KvStore_constants::kDefaultArea());

    auto event2 =
        node2->waitForEvent(thrift::SparkNeighborEventType::NEIGHBOR_UP);
    ASSERT_TRUE(event2.has_value());
    EXPECT_EQ(event2.value().neighbor.nodeName, nodeName1);
    EXPECT_EQ(event2.value().area, thrift::KvStore_constants::kDefaultArea());
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  CHECK(!::sodium_init());

  // Run the tests
  return RUN_ALL_TESTS();
}
