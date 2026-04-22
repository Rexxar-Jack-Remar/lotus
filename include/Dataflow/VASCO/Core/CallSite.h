#pragma once

#include "Dataflow/VASCO/Core/Context.h"

#include <memory>

namespace vasco {

template <typename M, typename N, typename A> class CallSite {
public:
  using ContextPtr = std::shared_ptr<Context<M, N, A>>;

  CallSite() = default;

  CallSite(ContextPtr CallingContext, N CallNode)
      : CallingContext(std::move(CallingContext)),
        CallNode(std::move(CallNode)) {}

  bool operator<(const CallSite &Other) const {
    if (contextId() != Other.contextId()) {
      return contextId() < Other.contextId();
    }
    return std::less<N>()(CallNode, Other.CallNode);
  }

  bool operator==(const CallSite &Other) const {
    return CallingContext == Other.CallingContext && CallNode == Other.CallNode;
  }

  ContextPtr getCallingContext() const { return CallingContext; }
  const N &getCallNode() const { return CallNode; }

  explicit operator bool() const { return static_cast<bool>(CallingContext); }

private:
  std::size_t contextId() const {
    return CallingContext ? CallingContext->getId() : 0;
  }

  ContextPtr CallingContext;
  N CallNode{};
};

} // namespace vasco
