#include <utest/utest.hpp>

// Test that pfr is includable
#include <boost/pfr/precise.hpp>

#include <utest/simple_server.hpp>

#include <engine/io/socket.hpp>

namespace {
using testing::SimpleServer;

static const std::string kOkRequest = "OK";
static const std::string kOkResponse = "OK RESPONSE DATA";

SimpleServer::Response assert_received_ok(const SimpleServer::Request& r) {
  EXPECT_EQ(r, kOkRequest) << "SimpleServer received: " << r;
  return {kOkResponse, SimpleServer::Response::kWriteAndClose};
}

SimpleServer::Response assert_received_nothing(const SimpleServer::Request& r) {
  EXPECT_TRUE(false) << "SimpleServer received: " << r;
  return {"", SimpleServer::Response::kWriteAndClose};
}

}  // namespace

TEST(SimpleServer, NothingReceived) {
  TestInCoro([] { SimpleServer{assert_received_nothing}; });
}

TEST(SimpleServer, ExampleTcpIpV4) {
  TestInCoro([] {
    SimpleServer s(assert_received_ok);

    // ... invoke code that sends "OK" to localhost:8080 or localhost:8042.
    engine::io::AddrStorage addr_storage;
    auto* sa = addr_storage.As<struct sockaddr_in>();
    sa->sin_family = AF_INET;
    sa->sin_port = htons(s.GetPort());
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    engine::io::Addr addr(addr_storage, SOCK_STREAM, 0);

    engine::io::Socket worksock = engine::io::Connect(addr, {});
    ASSERT_EQ(kOkRequest.size(),
              worksock.SendAll(kOkRequest.data(), kOkRequest.size(), {}));

    std::string response;
    response.resize(100);
    const auto size = worksock.RecvAll(&response[0], response.size(), {});
    response.resize(size);
    EXPECT_EQ(response, kOkResponse) << "Received " << response;
  });
}

TEST(SimpleServer, ExampleTcpIpV6) {
  TestInCoro([] {
    SimpleServer s(assert_received_ok, SimpleServer::kTcpIpV6);

    // ... invoke code that sends "OK" to localhost:8080 or localhost:8042.
    engine::io::AddrStorage addr_storage;
    auto* sa = addr_storage.As<struct sockaddr_in6>();
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons(s.GetPort());
    sa->sin6_addr = in6addr_loopback;
    engine::io::Addr addr(addr_storage, SOCK_STREAM, 0);

    engine::io::Socket worksock = engine::io::Connect(addr, {});
    ASSERT_EQ(kOkRequest.size(),
              worksock.SendAll(kOkRequest.data(), kOkRequest.size(), {}));

    std::string response;
    response.resize(100);
    const auto size = worksock.RecvAll(&response[0], response.size(), {});
    response.resize(size);
    EXPECT_EQ(response, kOkResponse) << "Received " << response;
  });
}

TEST(SimpleServer, ExampleTcpIpV4Twice) {
  auto assert_received_twice = [i = 0](const SimpleServer::Request& r) mutable {
    EXPECT_EQ(r, kOkRequest) << "SimpleServer received: " << r;
    EXPECT_LE(++i, 2) << "Callback was called more than twice: " << i;

    const auto command = (i == 1 ? SimpleServer::Response::kWriteAndContinue
                                 : SimpleServer::Response::kWriteAndClose);

    return SimpleServer::Response{kOkResponse, command};
  };

  TestInCoro([assert_received_twice] {
    SimpleServer s(assert_received_twice);

    // ... invoke code that sends "OK" to localhost:8080 or localhost:8042.
    engine::io::AddrStorage addr_storage;
    auto* sa = addr_storage.As<struct sockaddr_in>();
    sa->sin_family = AF_INET;
    sa->sin_port = htons(s.GetPort());
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    engine::io::Addr addr(addr_storage, SOCK_STREAM, 0);

    engine::io::Socket worksock = engine::io::Connect(addr, {});

    ASSERT_EQ(kOkRequest.size(),
              worksock.SendAll(kOkRequest.data(), kOkRequest.size(), {}));
    std::string response;
    response.resize(100);
    const auto size = worksock.RecvSome(&response[0], response.size(), {});
    response.resize(size);
    EXPECT_EQ(response, kOkResponse) << "Received " << response;

    ASSERT_EQ(kOkRequest.size(),
              worksock.SendAll(kOkRequest.data(), kOkRequest.size(), {}));
    response.clear();
    response.resize(100);
    const auto size2 = worksock.RecvAll(&response[0], response.size(), {});
    response.resize(size2);
    EXPECT_EQ(response, kOkResponse) << "Received " << response;

    EXPECT_EQ(0, worksock.RecvAll(&response[0], response.size(), {}));
  });
}
