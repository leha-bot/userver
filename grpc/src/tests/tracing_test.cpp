#include <userver/utest/utest.hpp>

#include <userver/tracing/span.hpp>
#include <userver/utils/algo.hpp>

#include <ugrpc/impl/rpc_metadata_keys.hpp>

#include <tests/service_fixture_test.hpp>
#include "unit_test_client.usrv.pb.hpp"
#include "unit_test_service.usrv.pb.hpp"

USERVER_NAMESPACE_BEGIN

using namespace sample::ugrpc;

namespace {

std::string ToString(grpc::string_ref str) {
  return std::string(str.data(), str.size());
}

template <typename MetadataMap>
std::string GetMetadata(const MetadataMap& metadata, const std::string& key) {
  return ToString(utils::FindOrDefault(metadata, key));
}

const std::string kServerTraceId = "server-trace-id";
const std::string kServerSpanId = "server-span-id";
const std::string kServerLink = "server-link";
const std::string kServerParentSpanId = "server-parent-span-id";
const std::string kServerParentLink = "server-parent-link";
const std::string kClientTraceIdEcho = "client-trace-id-echo";
const std::string kClientSpanIdEcho = "client-span-id-echo";
const std::string kClientLinkEcho = "client-link-echo";

class UnitTestServiceWithTracingChecks final : public UnitTestServiceBase {
 public:
  void SayHello(SayHelloCall& call, GreetingRequest&& request) override {
    SetMetadata(call.GetContext());
    call.Finish({});
  }

  void ReadMany(ReadManyCall& call, StreamGreetingRequest&& request) override {
    SetMetadata(call.GetContext());
    call.Finish();
  }

  void WriteMany(WriteManyCall& call) override {
    SetMetadata(call.GetContext());
    call.Finish({});
  }

  void Chat(ChatCall& call) override {
    SetMetadata(call.GetContext());
    call.Finish();
  }

 private:
  void SetMetadata(grpc::ServerContext& context) {
    const auto& span = tracing::Span::CurrentSpan();
    const auto& client_meta = context.client_metadata();

    context.AddInitialMetadata(kServerTraceId, span.GetTraceId());
    context.AddInitialMetadata(kServerSpanId, span.GetSpanId());
    context.AddInitialMetadata(kServerLink, span.GetLink());
    context.AddInitialMetadata(kServerParentSpanId, span.GetParentId());
    context.AddInitialMetadata(kServerParentLink, span.GetParentLink());

    context.AddInitialMetadata(
        kClientTraceIdEcho, GetMetadata(client_meta, ugrpc::impl::kXYaTraceId));
    context.AddInitialMetadata(
        kClientSpanIdEcho, GetMetadata(client_meta, ugrpc::impl::kXYaSpanId));
    context.AddInitialMetadata(
        kClientLinkEcho, GetMetadata(client_meta, ugrpc::impl::kXYaRequestId));
  }
};

using GrpcTracing = GrpcServiceFixtureSimple<UnitTestServiceWithTracingChecks>;

void CheckMetadata(const grpc::ClientContext& context) {
  const auto& metadata = context.GetServerInitialMetadata();
  const auto& span = tracing::Span::CurrentSpan();

  // - TraceId should propagate both to sub-spans within a single service,
  //   and from client to server
  // - Link should propagate within a single service, but not from client to
  //   server (a new link will be generated for the request processing task)
  // - SpanId should not propagate
  // - there are also ParentSpanId and ParentLink with obvious semantics
  // - client uses a detached sub-Span for the RPC

  // the checks below follow EXPECT_EQ(cause, effect) order
  EXPECT_EQ(span.GetTraceId(), GetMetadata(metadata, kClientTraceIdEcho));
  EXPECT_EQ(GetMetadata(metadata, kClientTraceIdEcho),
            GetMetadata(metadata, kServerTraceId));

  EXPECT_NE(span.GetSpanId(), GetMetadata(metadata, kClientSpanIdEcho));
  EXPECT_EQ(GetMetadata(metadata, kClientSpanIdEcho),
            GetMetadata(metadata, kServerParentSpanId));
  EXPECT_NE(GetMetadata(metadata, kServerParentSpanId),
            GetMetadata(metadata, kServerSpanId));

  EXPECT_EQ(span.GetLink(), GetMetadata(metadata, kClientLinkEcho));
  EXPECT_EQ(GetMetadata(metadata, kClientLinkEcho),
            GetMetadata(metadata, kServerParentLink));
  EXPECT_NE(GetMetadata(metadata, kServerParentLink),
            GetMetadata(metadata, kServerLink));
}

}  // namespace

UTEST_F(GrpcTracing, UnaryRPC) {
  auto client = MakeClient<UnitTestServiceClient>();
  GreetingRequest out;
  out.set_name("userver");
  auto call = client.SayHello(out);
  EXPECT_NO_THROW(call.Finish());
  CheckMetadata(call.GetContext());
}

UTEST_F(GrpcTracing, InputStream) {
  auto client = MakeClient<UnitTestServiceClient>();
  StreamGreetingRequest out;
  out.set_name("userver");
  out.set_number(42);
  StreamGreetingResponse in;
  auto call = client.ReadMany(out);
  EXPECT_FALSE(call.Read(in));
  CheckMetadata(call.GetContext());
}

UTEST_F(GrpcTracing, OutputStream) {
  auto client = MakeClient<UnitTestServiceClient>();
  auto call = client.WriteMany();
  EXPECT_NO_THROW(call.Finish());
  CheckMetadata(call.GetContext());
}

UTEST_F(GrpcTracing, BidirectionalStream) {
  auto client = MakeClient<UnitTestServiceClient>();
  StreamGreetingResponse in;
  auto call = client.Chat();
  EXPECT_FALSE(call.Read(in));
  CheckMetadata(call.GetContext());
}

UTEST_F(GrpcTracing, SpansInDifferentRPCs) {
  auto client = MakeClient<UnitTestServiceClient>();
  GreetingRequest out;
  out.set_name("userver");

  auto call1 = client.SayHello(out);
  call1.Finish();
  const auto& metadata1 = call1.GetContext().GetServerInitialMetadata();

  auto call2 = client.SayHello(out);
  call2.Finish();
  const auto& metadata2 = call2.GetContext().GetServerInitialMetadata();

  EXPECT_EQ(GetMetadata(metadata1, kServerTraceId),
            GetMetadata(metadata2, kServerTraceId));
  EXPECT_NE(GetMetadata(metadata1, kServerSpanId),
            GetMetadata(metadata2, kServerSpanId));
  EXPECT_NE(GetMetadata(metadata1, kServerParentSpanId),
            GetMetadata(metadata2, kServerParentSpanId));
  EXPECT_NE(GetMetadata(metadata1, kServerLink),
            GetMetadata(metadata2, kServerLink));
  EXPECT_EQ(GetMetadata(metadata1, kServerParentLink),
            GetMetadata(metadata2, kServerParentLink));
}

USERVER_NAMESPACE_END
