#include "Checker/Pulse/PulseTaint.h"
#include "Checker/Pulse/PulseDomain.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include <llvm/IR/Instructions.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// TaintDomain Implementation
//===----------------------------------------------------------------------===//

void TaintDomain::add(AbstractValue v, TaintItem item) {
    taints_[v].insert(item);
}

void TaintDomain::remove(AbstractValue v) {
    taints_.erase(v);
}

bool TaintDomain::has(AbstractValue v) const {
    return taints_.find(v) != taints_.end();
}

const std::set<TaintItem>& TaintDomain::get(AbstractValue v) const {
    auto it = taints_.find(v);
    static const std::set<TaintItem> empty;
    return (it != taints_.end()) ? it->second : empty;
}

void TaintDomain::join(const TaintDomain& other) {
    for (const auto& kv : other.taints_) {
        taints_[kv.first].insert(kv.second.begin(), kv.second.end());
    }
}

//===----------------------------------------------------------------------===//
// TaintOperations Implementation
//===----------------------------------------------------------------------===//

unsigned TaintOperations::global_timestamp_ = 1;

void TaintOperations::taint(AbductiveDomain& astate,
                            AbstractValue v,
                            TaintKind kind,
                            const llvm::Instruction* source) {
    const llvm::Function* func = source ? source->getFunction() : nullptr;
    unsigned timestamp = getNextTimestamp();
    TaintItem item(kind, source, func, timestamp);
    item.history.addEvent(ValueHistory::EventKind::Unknown, source, func); // Start of taint
    astate.getTaintDomain().add(v, item);
    astate.getPostAttrs().add(v, Attribute::Tainted);
}

bool TaintOperations::checkSink(AbductiveDomain& astate,
                                AbstractValue v,
                                const std::string& sink_name,
                                const llvm::Instruction* sink_loc) {
    if (!astate.getTaintDomain().has(v)) {
        return false;
    }

    const auto& taints = astate.getTaintDomain().get(v);
    if (taints.empty()) return false;

    // Find unsanitized taint items
    std::vector<const TaintItem*> unsanitized_taints;
    for (const auto& item : taints) {
        if (!item.isSanitized()) {
            unsanitized_taints.push_back(&item);
        }
    }
    
    if (unsanitized_taints.empty()) {
        return false;  // All taints are sanitized
    }

    // Production-ready: report all unsanitized taints, not just the first
    // But for now, report the most critical one
    const TaintItem* critical_item = nullptr;
    TaintKind critical_kind = TaintKind::Unknown;
    
    for (const auto* item : unsanitized_taints) {
        // Prioritize: Sensitive > Network > UserInput > FileSystem > Environment > Unknown
        if (!critical_item || 
            (item->kind == TaintKind::Sensitive) ||
            (critical_kind != TaintKind::Sensitive && item->kind == TaintKind::Network) ||
            (critical_kind == TaintKind::Unknown && item->kind != TaintKind::Unknown)) {
            critical_item = item;
            critical_kind = item->kind;
        }
    }
    
    if (!critical_item) {
        return false;
    }
    
    const TaintItem& item = *critical_item;
    
    BugReportMgr& mgr = BugReportMgr::get_instance();
    // Register type if not already (hacky, should be in PulseChecker::registerBugTypes)
    int typeId = mgr.register_bug_type("Taint Flow", BugDescription::BI_HIGH, BugDescription::BC_SECURITY, "CWE-20");
    
    BugReport* report = new BugReport(typeId);
    
    // Add sink step with detailed information
    std::string sink_msg = "Tainted data flows into sink '";
    sink_msg += sink_name;
    sink_msg += "'";
    if (sink_name == "system" || sink_name == "exec" || sink_name == "popen") {
        sink_msg += " (command injection risk)";
    } else if (sink_name == "printf" || sink_name == "sprintf") {
        sink_msg += " (format string vulnerability)";
    } else if (sink_name == "strcpy" || sink_name == "strcat") {
        sink_msg += " (buffer overflow risk)";
    }
    
    report->append_step(const_cast<llvm::Instruction*>(sink_loc),
                        sink_msg, 0, {}, "sink");

    // Add trace from history (production-ready: show full propagation path)
    const auto& events = item.history.getEvents();
    unsigned step_num = 1;
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->location) {
            std::string trace_msg = "Taint propagated here";
            if (it->kind == ValueHistory::EventKind::Store) {
                trace_msg += " (via store)";
            } else if (it->kind == ValueHistory::EventKind::Load) {
                trace_msg += " (via load)";
            } else if (it->kind == ValueHistory::EventKind::FunctionCall) {
                trace_msg += " (via function call)";
            }
            report->append_step(const_cast<llvm::Instruction*>(it->location),
                               trace_msg, step_num++, {}, "trace");
        }
    }

    // Add source step with detailed information
    if (item.source_instruction) {
        std::string src_msg = "Taint source: ";
        switch (item.kind) {
            case TaintKind::Network:
                src_msg += "Network input";
                break;
            case TaintKind::UserInput:
                src_msg += "User input";
                break;
            case TaintKind::FileSystem:
                src_msg += "File system";
                break;
            case TaintKind::Environment:
                src_msg += "Environment variable";
                break;
            case TaintKind::Sensitive:
                src_msg += "Sensitive data";
                break;
            default:
                src_msg += "Unknown source";
                break;
        }
        
        report->append_step(const_cast<llvm::Instruction*>(item.source_instruction),
                            src_msg, step_num++, {}, "source");
    }

    // Set confidence score based on taint kind and sink type
    int confidence = 80;
    if (critical_kind == TaintKind::Sensitive || critical_kind == TaintKind::Network) {
        confidence = 95;
    } else if (critical_kind == TaintKind::UserInput) {
        confidence = 90;
    }
    
    // Increase confidence for dangerous sinks
    if (sink_name == "system" || sink_name == "exec") {
        confidence = std::min(95, confidence + 5);
    }
    
    report->set_conf_score(confidence);
    mgr.insert_report(typeId, report, true);
    return true;
}

void TaintOperations::propagate(AbductiveDomain& astate,
                                AbstractValue src,
                                AbstractValue dest,
                                const llvm::Instruction* loc) {
    if (!astate.getTaintDomain().has(src)) {
        return;
    }

    const auto& src_taints = astate.getTaintDomain().get(src);
    for (TaintItem item : src_taints) {
        // Update history
        item.history.addEvent(ValueHistory::EventKind::Store, loc, loc ? loc->getFunction() : nullptr);
        astate.getTaintDomain().add(dest, item);
    }
    astate.getPostAttrs().add(dest, Attribute::Tainted);
}

void TaintOperations::sanitize(AbductiveDomain& astate,
                                AbstractValue v,
                                TaintKind sanitizer_kind,
                                const llvm::Instruction* sanitizer_loc) {
    if (!astate.getTaintDomain().has(v)) {
        return;
    }
    
    unsigned sanitizer_timestamp = getNextTimestamp();
    auto& taints = const_cast<std::set<TaintItem>&>(astate.getTaintDomain().get(v));
    
    // Add sanitizer to all taint items
    for (auto& item : taints) {
        const_cast<TaintItem&>(item).addSanitizer(sanitizer_kind, sanitizer_loc, sanitizer_timestamp);
    }
    
    // Sanitizers are tracked in TaintItem, no separate attribute needed
}

bool TaintOperations::isSanitized(const AbductiveDomain& astate, AbstractValue v) {
    if (!astate.getTaintDomain().has(v)) {
        return false;
    }
    
    const auto& taints = astate.getTaintDomain().get(v);
    for (const auto& item : taints) {
        if (!item.isSanitized()) {
            return false;  // At least one unsanitized taint
        }
    }
    return true;
}

void TaintOperations::propagateThroughLoad(AbductiveDomain& astate,
                                           AbstractValue src_addr,
                                           AbstractValue dest_val,
                                           const llvm::Instruction* loc) {
    // Propagate taint from memory location to loaded value
    propagate(astate, src_addr, dest_val, loc);
}

void TaintOperations::propagateThroughStore(AbductiveDomain& astate,
                                            AbstractValue src_val,
                                            AbstractValue dest_addr,
                                            const llvm::Instruction* loc) {
    // Propagate taint from stored value to memory location
    propagate(astate, src_val, dest_addr, loc);
}

void TaintOperations::propagateThroughCall(AbductiveDomain& astate,
                                           const llvm::CallInst* call,
                                           const std::vector<AbstractValue>& args,
                                           AbstractValue ret_val) {
    // Propagate taint from arguments to return value
    // Production-ready implementation with proper taint propagation rules
    
    const llvm::Function* callee = call->getCalledFunction();
    if (!callee) {
        // Indirect call - conservatively propagate all taints
        for (AbstractValue arg : args) {
            if (astate.getTaintDomain().has(arg)) {
                propagate(astate, arg, ret_val, call);
            }
        }
        return;
    }
    
    std::string func_name = callee->getName().str();
    
    // Check for taint sources (functions that introduce taint)
    // These would typically be in a model file, but for now we hardcode common ones
    bool is_source = (func_name == "read" || func_name == "fread" || 
                      func_name == "recv" || func_name == "recvfrom" ||
                      func_name == "scanf" || func_name == "fgets" ||
                      func_name == "getenv" || func_name == "getcwd");
    
    if (is_source && !args.empty()) {
        // Mark return value as tainted
        TaintKind kind = TaintKind::UserInput;
        if (func_name.find("recv") != std::string::npos) {
            kind = TaintKind::Network;
        } else if (func_name.find("env") != std::string::npos) {
            kind = TaintKind::Environment;
        } else if (func_name.find("read") != std::string::npos || 
                   func_name.find("fread") != std::string::npos) {
            kind = TaintKind::FileSystem;
        }
        taint(astate, ret_val, kind, call);
    }
    
    // Check for taint sinks (functions that should not receive tainted data)
    bool is_sink = (func_name == "system" || func_name == "exec" || 
                    func_name == "popen" || func_name == "eval" ||
                    func_name == "printf" || func_name == "sprintf" ||
                    func_name == "strcpy" || func_name == "strcat");
    
    if (is_sink) {
        // Check all pointer arguments for taint
        for (size_t i = 0; i < args.size() && i < call->arg_size(); ++i) {
            const llvm::Value* arg_val = call->getArgOperand(i);
            if (arg_val->getType()->isPointerTy()) {
                AbstractValue arg_av = args[i];
                if (astate.getTaintDomain().has(arg_av)) {
                    checkSink(astate, arg_av, func_name, call);
                }
            }
        }
    }
    
    // Propagate taint through function calls
    // For known functions, use specific propagation rules
    // For unknown functions, conservatively propagate all taints
    bool known_function = false;
    
    // Sanitizers: functions that remove taint
    bool is_sanitizer = (func_name == "strlen" || func_name == "atoi" ||
                         func_name == "strtol" || func_name == "strtoul");
    
    if (is_sanitizer && !args.empty()) {
        // Sanitize the first argument
        TaintKind sanitizer_kind = TaintKind::Unknown;
        sanitize(astate, args[0], sanitizer_kind, call);
        return;  // Don't propagate taint from sanitizers
    }
    
    // Default: propagate taint from all arguments to return value
    // (unless it's a sanitizer, handled above)
    for (AbstractValue arg : args) {
        if (astate.getTaintDomain().has(arg)) {
            propagate(astate, arg, ret_val, call);
        }
    }
}

} // namespace pulse
