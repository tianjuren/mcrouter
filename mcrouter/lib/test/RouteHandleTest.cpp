/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <folly/Memory.h>

#include "mcrouter/lib/mc/msg.h"
#include "mcrouter/lib/network/TypedThriftMessage.h"
#include "mcrouter/lib/routes/AllAsyncRoute.h"
#include "mcrouter/lib/routes/AllFastestRoute.h"
#include "mcrouter/lib/routes/AllInitialRoute.h"
#include "mcrouter/lib/routes/AllMajorityRoute.h"
#include "mcrouter/lib/routes/AllSyncRoute.h"
#include "mcrouter/lib/routes/ErrorRoute.h"
#include "mcrouter/lib/routes/HashRoute.h"
#include "mcrouter/lib/routes/NullRoute.h"
#include "mcrouter/lib/test/RouteHandleTestUtil.h"
#include "mcrouter/lib/test/TestRouteHandle.h"

using namespace facebook::memcache;

using folly::make_unique;
using std::make_shared;
using std::string;
using std::vector;

using TestHandle = TestHandleImpl<TestRouteHandleIf>;

TEST(routeHandleTest, nullGet) {
  TestRouteHandle<NullRoute<TestRouteHandleIf>> rh;

  TypedThriftRequest<cpp2::McGetRequest> req;
  req.setKey("key");

  auto reply = rh.route(req);
  EXPECT_EQ(mc_res_notfound, reply.result());
}

TEST(routeHandleTest, nullSet) {
  TestRouteHandle<NullRoute<TestRouteHandleIf>> rh;
  TypedThriftRequest<cpp2::McSetRequest> req("key");
  req.setValue("value");
  auto reply = rh.route(std::move(req));
  EXPECT_EQ(mc_res_notstored, reply.result());
}

TEST(routeHandleTest, nullDelete) {
  TestRouteHandle<NullRoute<TestRouteHandleIf>> rh;
  auto reply = rh.route(TypedThriftRequest<cpp2::McDeleteRequest>("key"));
  EXPECT_EQ(mc_res_notfound, reply.result());
}

TEST(routeHandleTest, nullTouch) {
  TestRouteHandle<NullRoute<TestRouteHandleIf>> rh;
  auto reply = rh.route(TypedThriftRequest<cpp2::McTouchRequest>("key"));
  EXPECT_EQ(mc_res_notfound, reply.result());
}

TEST(routeHandleTest, nullIncr) {
  TestRouteHandle<NullRoute<TestRouteHandleIf>> rh;
  TypedThriftRequest<cpp2::McIncrRequest> req("key");
  req->set_delta(1);
  auto reply = rh.route(std::move(req));
  EXPECT_EQ(mc_res_notfound, reply.result());
}

TEST(routeHandleTest, nullAppend) {
  TestRouteHandle<NullRoute<TestRouteHandleIf>> rh;
  TypedThriftRequest<cpp2::McAppendRequest> req("key");
  req.setValue("value");
  auto reply = rh.route(std::move(req));
  EXPECT_EQ(mc_res_notstored, reply.result());
}

TEST(routeHandleTest, nullPrepend) {
  TestRouteHandle<NullRoute<TestRouteHandleIf>> rh;
  TypedThriftRequest<cpp2::McPrependRequest> req("key");
  req.setValue("value");
  auto reply = rh.route(std::move(req));
  EXPECT_EQ(mc_res_notstored, reply.result());
}

TEST(routeHandleTest, error) {
  TestRouteHandle<ErrorRoute<TestRouteHandleIf>> rh;
  auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("key"));
  EXPECT_TRUE(reply.isError());
}

TEST(routeHandleTest, allSync) {
  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "c"))
  };

  TestFiberManager fm;

  TestRouteHandle<AllSyncRoute<TestRouteHandleIf>> rh(
    get_route_handles(test_handles));

  fm.runAll(
    {
      [&]() {
        auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("key"));

        /* Check that we got the worst result back */
        EXPECT_EQ(mc_res_remote_error, reply.result());
        EXPECT_EQ("c", reply.valueRangeSlow().str());

        for (auto& h : test_handles) {
          EXPECT_EQ(vector<string>{"key"}, h->saw_keys);
        }
      }
    });
}

TEST(routeHandleTest, allSyncTyped) {
  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "c"))
  };

  TestFiberManager fm;

  TestRouteHandle<AllSyncRoute<TestRouteHandleIf>> rh(
    get_route_handles(test_handles));

  fm.runAll(
    {
      [&]() {
        TypedThriftRequest<cpp2::McGetRequest> req;
        req.setKey("key");

        auto reply = rh.route(req);

        /* Check that we got the worst result back */
        EXPECT_EQ(mc_res_remote_error, reply.result());
        EXPECT_EQ("c", toString(*reply->get_value()));

        for (auto& h : test_handles) {
          EXPECT_EQ(vector<string>{"key"}, h->saw_keys);
        }
      }
    });
}

TEST(routeHandleTest, allAsync) {
  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "c"))
  };

  TestFiberManager fm;

  TestRouteHandle<AllAsyncRoute<TestRouteHandleIf>> rh(
    get_route_handles(test_handles));

  fm.runAll(
    {
      [&]() {
        auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("key"));

        /* Check that we got no result back */
        EXPECT_EQ(mc_res_notfound, reply.result());
      }
    });

  /* Check that everything is complete in the background */
  for (auto& h : test_handles) {
    EXPECT_EQ(vector<string>{"key"}, h->saw_keys);
  }
}

TEST(routeHandleTest, allInitial) {
  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "c")),
  };

  TestFiberManager fm;
  auto routeHandles = get_route_handles(test_handles);
  TestRouteHandle<AllInitialRoute<TestRouteHandleIf>> rh(routeHandles);

  fm.runAll(
    {
      [&]() {
        auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("key"));

        /* Check that we got the initial result back */
        EXPECT_EQ(mc_res_found, reply.result());
        EXPECT_EQ("a", reply.valueRangeSlow().str());
      }
    });

  /* Check that everything is complete in the background */
  for (auto& h : test_handles) {
    EXPECT_EQ(vector<string>{"key"}, h->saw_keys);
  }

  /* Check that traverse is correct */
  int cnt = 0;
  RouteHandleTraverser<TestRouteHandleIf> t{
    [&cnt](const TestRouteHandleIf&){ ++cnt; }
  };
  rh.traverse(TypedThriftRequest<cpp2::McGetRequest>("key"), t);
  EXPECT_EQ(cnt, routeHandles.size());
}

TEST(routeHandleTest, allMajority) {
  TestFiberManager fm;

  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "c"))
  };

  TestRouteHandle<AllMajorityRoute<TestRouteHandleIf>> rh(
    get_route_handles(test_handles));

  test_handles[1]->pause();

  fm.runAll(
    {
      [&]() {
        auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("key"));

        /* Check that we got the majority reply
           without waiting for "b", which is paused */
        EXPECT_EQ(mc_res_remote_error, reply.result());

        EXPECT_EQ(vector<string>{"key"}, test_handles[0]->saw_keys);
        EXPECT_EQ(vector<string>{}, test_handles[1]->saw_keys);
        EXPECT_EQ(vector<string>{"key"}, test_handles[2]->saw_keys);

        test_handles[1]->unpause();
      }
    });

  /* Check that everything is complete in the background */
  for (auto& h : test_handles) {
    EXPECT_EQ(vector<string>{"key"}, h->saw_keys);
  }
}

TEST(routeHandleTest, allMajorityTie) {
  TestFiberManager fm;

  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "c")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "d"))
  };

  TestRouteHandle<AllMajorityRoute<TestRouteHandleIf>> rh(
    get_route_handles(test_handles));

  fm.runAll(
    {
      [&]() {
        auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("key"));

        /* Check that we got the _worst_ majority reply */
        EXPECT_EQ(mc_res_remote_error, reply.result());
      }
    });

  /* Check that everything is complete */
  for (auto& h : test_handles) {
    EXPECT_EQ(vector<string>{"key"}, h->saw_keys);
  }
}

TEST(routeHandleTest, allFastest) {
  TestFiberManager fm;

  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_remote_error, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_notfound, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "c"))
  };

  TestRouteHandle<AllFastestRoute<TestRouteHandleIf>> rh(
    get_route_handles(test_handles));

  test_handles[1]->pause();

  fm.runAll(
    {
      [&]() {
        auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("key"));

        /* Check that we got the fastest non-error result back
           ('b' is paused) */
        EXPECT_EQ(mc_res_found, reply.result());
        EXPECT_EQ("c", reply.valueRangeSlow().str());

        EXPECT_EQ(vector<string>{"key"}, test_handles[0]->saw_keys);
        EXPECT_EQ(vector<string>{}, test_handles[1]->saw_keys);
        EXPECT_EQ(vector<string>{"key"}, test_handles[2]->saw_keys);

        test_handles[1]->unpause();
     }
    });

  /* Check that everything is complete in the background */
  for (auto& h : test_handles) {
    EXPECT_EQ(vector<string>{"key"}, h->saw_keys);
  }
}

class HashFunc {
 public:
  explicit HashFunc(size_t n) : n_(n) {}

  size_t operator()(folly::StringPiece key) const {
    return std::stoi(key.str()) % n_;
  }

  static std::string type() { return "HashFunc"; }

 private:
  size_t n_;
};

TEST(routeHandleTest, hashNoSalt) {
  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "c")),
  };

  TestFiberManager fm;

  TestRouteHandle<HashRoute<TestRouteHandleIf, HashFunc>> rh(
    get_route_handles(test_handles),
    /* salt= */ "",
    HashFunc(test_handles.size()));

  fm.run([&]() {
      auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("0"));
      EXPECT_EQ("a", reply.valueRangeSlow().str());
    });

  fm.run([&]() {
      auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("1"));
      EXPECT_EQ("b", reply.valueRangeSlow().str());
    });

  fm.run([&]() {
      auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("2"));
      EXPECT_EQ("c", reply.valueRangeSlow().str());
    });
}

TEST(routeHandleTest, hashSalt) {
  vector<std::shared_ptr<TestHandle>> test_handles{
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "a")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "b")),
    make_shared<TestHandle>(GetRouteTestData(mc_res_found, "c")),
  };

  TestFiberManager fm;

  TestRouteHandle<HashRoute<TestRouteHandleIf, HashFunc>> rh(
    get_route_handles(test_handles),
    /* salt= */ "1",
    HashFunc(test_handles.size()));

  fm.run([&]() {
      auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("0"));
      /* 01 % 3 == 1 */
      EXPECT_EQ("b", reply.valueRangeSlow().str());
    });

  fm.run([&]() {
      auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("1"));
      /* 11 % 3 == 2 */
      EXPECT_EQ("c", reply.valueRangeSlow().str());
    });

  fm.run([&]() {
      auto reply = rh.route(TypedThriftRequest<cpp2::McGetRequest>("2"));
      /* 21 % 3 == 0 */
      EXPECT_EQ("a", reply.valueRangeSlow().str());
    });
}
