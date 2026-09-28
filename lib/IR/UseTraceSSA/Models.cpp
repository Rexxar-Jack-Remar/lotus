#include "IR/UseTraceSSA/Models.h"
#include <stdexcept>

namespace lotus {
namespace usetracessa {
namespace {
void annotate(TraceFlowGraph &g, const std::vector<FlowNodeID> &nodes, Event event,
              Certainty certainty = Certainty::Must) {
  for (auto n : nodes) g.annotate(n, event, certainty);
}
void connect(TraceFlowGraph &g, const std::vector<FlowNodeID> &from,
             const std::vector<FlowNodeID> &to, FlowKind kind, CallSiteID site) {
  for (auto a : from) for (auto b : to) {
    FlowEdge e; e.from = a; e.to = b; e.kind = kind; e.callSite = site;
    g.addEdge(e);
  }
}
bool need(TraceFlowGraph &g, const CallPorts &c, std::size_t args) {
  if (c.arguments.size() >= args) return true;
  g.addIssue("incomplete call ports for " + c.callee); return false;
}
void needPorts(TraceFlowGraph &g, const CallPorts &c, const std::vector<FlowNodeID> &ports,
               const char *what) {
  if (ports.empty()) g.addIssue(c.callee + ": missing " + what + " (no address-as-content fallback)");
}
} // namespace
LibraryModels::LibraryModels() {
  auto read = [](TraceFlowGraph &g, const CallPorts &c) {
    if (!need(g, c, 3)) return;
    const auto &out = c.arguments[1].memoryOut;
    needPorts(g, c, out, "buffer memory output");
    annotate(g, out, Event::Source | Event::Read);
    annotate(g, c.returnValue, Event::Source);
  };
  registerModel("read", read); registerModel("recv", read); registerModel("recvfrom", read);
  auto write = [](TraceFlowGraph &g, const CallPorts &c) {
    if (!need(g, c, 3)) return;
    const auto &in = c.arguments[1].memoryIn;
    needPorts(g, c, in, "buffer memory input");
    annotate(g, in, Event::Sink);
  };
  registerModel("write", write); registerModel("send", write); registerModel("sendto", write);
  auto copy = [](TraceFlowGraph &g, const CallPorts &c) {
    if (!need(g, c, 3)) return;
    const auto &src = c.arguments[1].memoryIn, &dst = c.arguments[0].memoryOut;
    needPorts(g, c, src, "copy source memory"); needPorts(g, c, dst, "copy destination memory");
    connect(g, src, dst, FlowKind::Summary, c.callSite);
    connect(g, c.arguments[0].value, c.returnValue, FlowKind::Summary, c.callSite);
  };
  registerModel("memcpy", copy); registerModel("memmove", copy);
  auto release = [](TraceFlowGraph &g, const CallPorts &c) {
    if (!need(g, c, 1)) return;
    const auto &arg = c.arguments[0];
    if (arg.resourceEffects.empty()) g.addIssue(c.callee + ": missing resource effect ports");
    for (const auto &port : arg.resourceEffects)
      g.annotate(port.node, Event::Release, port.objects, port.certainty);
  };
  registerModel("free", release);
  auto allocate = [](TraceFlowGraph &g, const CallPorts &c) {
    if (c.allocationEffects.empty()) g.addIssue(c.callee + ": missing allocation effect ports");
    // A successful fresh-object reset requires a more precise custom model.
    // malloc can fail and allocation-site objects can summarize old instances.
    for (const auto &port : c.allocationEffects)
      g.annotate(port.node, Event::Allocate, port.objects, Certainty::May);
  };
  registerModel("malloc", allocate); registerModel("calloc", allocate);
  registerModel("memset", [](TraceFlowGraph &g, const CallPorts &c) {
    if (!need(g, c, 3)) return;
    needPorts(g, c, c.arguments[0].memoryOut, "memory output");
    connect(g, c.arguments[1].value, c.arguments[0].memoryOut, FlowKind::Summary, c.callSite);
    connect(g, c.arguments[0].value, c.returnValue, FlowKind::Summary, c.callSite);
    // No sanitizer: the written range may cover only part of the tracked region.
  });
}
void LibraryModels::registerModel(std::string name, Model model) {
  if (name.empty() || !model) throw std::invalid_argument("UseTraceSSA: invalid library model");
  Models[std::move(name)] = std::move(model);
}
void LibraryModels::apply(TraceFlowGraph &g, const CallPorts &call) const {
  // Validate all supplied ports before invoking a model.
  auto validate = [&](const std::vector<FlowNodeID> &ports) { for (auto n : ports) g.node(n); };
  validate(call.returnValue);
  for (const auto &port : call.allocationEffects) g.node(port.node);
  for (const auto &a : call.arguments) {
    validate(a.value); validate(a.memoryIn); validate(a.memoryOut);
    for (const auto &port : a.resourceEffects) g.node(port.node);
  }
  auto found = Models.find(call.callee);
  // LLVM intrinsic suffixes carry overload types. Only recognized memcpy /
  // memmove / memset families are canonicalized, not arbitrary name prefixes.
  std::string canonical;
  for (const char *name : {"memcpy", "memmove", "memset"})
    if (call.callee.compare(0, std::string("llvm.").size() + std::string(name).size() + 1,
                            std::string("llvm.") + name + ".") == 0) canonical = name;
  if (found == Models.end() && !canonical.empty()) found = Models.find(canonical);
  if (found != Models.end()) { found->second(g, call); return; }
  std::vector<FlowNodeID> inputs, outputs = call.returnValue;
  for (const auto &a : call.arguments) {
    inputs.insert(inputs.end(), a.value.begin(), a.value.end());
    inputs.insert(inputs.end(), a.memoryIn.begin(), a.memoryIn.end());
    outputs.insert(outputs.end(), a.memoryOut.begin(), a.memoryOut.end());
    // Unknown external lifetime effects are not guessed as free or allocation.
  }
  connect(g, inputs, outputs, FlowKind::Summary, call.callSite);
  g.addIssue("unknown external call semantics: " + call.callee);
}
namespace queries {
Query taint(const TraceFlowGraph &g) {
  Query q; q.sources = g.select(Event::Source); q.sinks = g.select(Event::Sink);
  q.trapEvents = Event::Sanitize; return q;
}
// A resource may enter through a formal parameter without a visible allocation.
// Starting at release events preserves these bugs while Allocate still resets state.
Query doubleFree(const TraceFlowGraph &g) {
  Query q; q.sources = g.select(Event::Allocate | Event::Release); q.sinks = g.select(Event::Release);
  q.automaton = Automaton::doubleFree(); return q;
}
Query useAfterFree(const TraceFlowGraph &g) {
  Query q; q.sources = g.select(Event::Allocate | Event::Release); q.sinks = g.select(Event::Dereference);
  q.automaton = Automaton::useAfterFree(); return q;
}
Query uncheckedUse(std::vector<FlowNodeID> sources, std::vector<FlowNodeID> uses) {
  Query q; q.sources = std::move(sources); q.sinks = std::move(uses);
  q.trapEvents = Event::NonNull; return q;
}
} // namespace queries
} // namespace usetracessa
} // namespace lotus
