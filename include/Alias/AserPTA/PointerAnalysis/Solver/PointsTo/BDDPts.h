#ifndef ASER_PTA_BDDPTS_H
#define ASER_PTA_BDDPTS_H

#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>

#include "Alias/PtsSet/BDDPtsSet.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/PTSTrait.h"
#include "Alias/SparrowAA/PtsSet.h"

namespace aser {

extern llvm::cl::opt<bool> ConfigUseBDDPts;
extern llvm::cl::opt<bool> ConfigBDDPtsReorder;
extern llvm::cl::opt<std::string> ConfigBDDPtsReorderMethod;

// A runtime-selectable points-to set for AserPTA. Backends:
// - SparseBitVector (default)
// - BDDAndersPtsSet (when --pta-use-bdd-pts is specified)
// Note: pointed-by sets are not supported by this backend.
class ConfigurablePTS {
private:
    using TargetID = NodeID;
    using SparseSet = AndersPtsSet;

    class VariantSet {
    public:
        using iterator = std::vector<TargetID>::const_iterator;

        VariantSet();
        VariantSet(const VariantSet &);
        VariantSet &operator=(const VariantSet &);
        VariantSet(VariantSet &&) noexcept = default;
        VariantSet &operator=(VariantSet &&) noexcept = default;
        ~VariantSet() = default;

        bool has(TargetID idx) const;
        bool insert(TargetID idx);
        bool contains(const VariantSet &other) const;
        bool intersectWith(const VariantSet &other) const;
        bool unionWith(const VariantSet &other);
        void clear();
        size_t count() const;
        bool isEmpty() const;
        bool empty() const { return isEmpty(); }  // Alias for SparseBitVector compatibility
        bool equals(const VariantSet &other) const;
        
        // Compute: this = lhs \ rhs (elements in lhs but not in rhs)
        bool intersectWithComplement(const VariantSet &lhs, const VariantSet &rhs);
        
        // Union assignment operator for compatibility (returns true if changed)
        bool operator|=(const VariantSet &other);

        iterator begin() const;
        iterator end() const;

    private:
        struct Concept {
            virtual ~Concept() = default;
            virtual bool has(TargetID) const = 0;
            virtual bool insert(TargetID) = 0;
            virtual bool contains(const Concept &) const = 0;
            virtual bool intersectWith(const Concept &) const = 0;
            virtual bool unionWith(const Concept &) = 0;
            virtual void clear() = 0;
            virtual size_t count() const = 0;
            virtual bool isEmpty() const = 0;
            virtual bool equals(const Concept &) const = 0;
            virtual std::unique_ptr<Concept> clone() const = 0;
            virtual void materialize(std::vector<TargetID> &out) const = 0;
        };

        template <typename Impl> struct Model : Concept {
            Impl set;

            using IndexT = typename std::conditional<
                std::is_same<Impl, BDDAndersPtsSet>::value,
                BDDAndersPtsSet::Index, unsigned>::type;

            static IndexT toIndex(TargetID idx) {
                return static_cast<IndexT>(idx);
            }

            bool has(TargetID idx) const override {
                return set.has(toIndex(idx));
            }
            bool insert(TargetID idx) override {
                return set.insert(toIndex(idx));
            }

            bool contains(const Concept &other) const override {
                if (auto *same = dynamic_cast<const Model *>(&other))
                    return set.contains(same->set);
                std::vector<TargetID> tmp;
                other.materialize(tmp);
                for (TargetID v : tmp)
                    if (!set.has(toIndex(v)))
                        return false;
                return true;
            }

            bool intersectWith(const Concept &other) const override {
                if (auto *same = dynamic_cast<const Model *>(&other))
                    return set.intersectWith(same->set);
                std::vector<TargetID> tmp;
                other.materialize(tmp);
                for (TargetID v : tmp)
                    if (set.has(toIndex(v)))
                        return true;
                return false;
            }

            bool unionWith(const Concept &other) override {
                if (auto *same = dynamic_cast<const Model *>(&other))
                    return set.unionWith(same->set);
                std::vector<TargetID> tmp;
                other.materialize(tmp);
                bool changed = false;
                for (TargetID v : tmp)
                    changed |= set.insert(toIndex(v));
                return changed;
            }

            void clear() override { set.clear(); }
            size_t count() const override { return set.getSize(); }
            bool isEmpty() const override { return set.isEmpty(); }

            bool equals(const Concept &other) const override {
                if (auto *same = dynamic_cast<const Model *>(&other))
                    return set == same->set;
                std::vector<TargetID> lhs, rhs;
                materialize(lhs);
                other.materialize(rhs);
                return lhs == rhs;
            }

            std::unique_ptr<Concept> clone() const override {
                return std::make_unique<Model>(*this);
            }

            void materialize(std::vector<TargetID> &out) const override {
                for (auto it = set.begin(), ie = set.end(); it != ie; ++it)
                    out.push_back(static_cast<TargetID>(*it));
            }
        };

        static std::unique_ptr<Concept> makeImpl();
        void refreshCache() const;

        std::unique_ptr<Concept> impl;
        mutable std::shared_ptr<std::vector<TargetID>> cache;
    };

public:
    using PtsTy = VariantSet;
    using iterator = PtsTy::iterator;

    static inline void onNewNodeCreation(NodeID id) {
        ensureBackendConfigured();
        assert(id == ptsVec.size());
        ptsVec.emplace_back();
        assert(ptsVec.size() == id + 1);
    }

    static inline void clearAll() {
        ptsVec.clear();
        backendLocked = false;
    }

    [[nodiscard]] static inline const PtsTy &getPointsTo(NodeID id) {
        validateId(id);
        return ptsVec[id];
    }

    static inline bool unionWith(NodeID src, NodeID dst) {
        validateId(src);
        validateId(dst);
        return ptsVec[src].unionWith(ptsVec[dst]);
    }

    [[nodiscard]] static inline bool intersectWith(NodeID src, NodeID dst) {
        validateId(src);
        validateId(dst);
        return ptsVec[src].intersectWith(ptsVec[dst]);
    }

    [[nodiscard]] static inline bool intersectWithNoSpecialNode(NodeID src, NodeID dst) {
        validateId(src);
        validateId(dst);
        const auto &lhs = ptsVec[src];
        const auto &rhs = ptsVec[dst];
        if (lhs.isEmpty() || rhs.isEmpty())
            return false;
        for (auto it = lhs.begin(), ie = lhs.end(); it != ie; ++it) {
            if (*it < NORMAL_NODE_START_ID)
                continue;
            if (rhs.has(*it))
                return true;
        }
        return false;
    }

    static inline bool insert(NodeID src, TargetID idx) {
        validateId(src);
        return ptsVec[src].insert(idx);
    }

    [[nodiscard]] static inline bool has(NodeID src, TargetID idx) {
        validateId(src);
        return ptsVec[src].has(idx);
    }

    [[nodiscard]] static inline bool equal(NodeID src, NodeID dst) {
        validateId(src);
        validateId(dst);
        return ptsVec[src].equals(ptsVec[dst]);
    }

    [[nodiscard]] static inline bool contains(NodeID src, NodeID dst) {
        validateId(src);
        validateId(dst);
        return ptsVec[src].contains(ptsVec[dst]);
    }

    [[nodiscard]] static inline bool isEmpty(NodeID id) {
        validateId(id);
        return ptsVec[id].isEmpty();
    }

    [[nodiscard]] static inline iterator begin(NodeID id) {
        validateId(id);
        return ptsVec[id].begin();
    }

    [[nodiscard]] static inline iterator end(NodeID id) {
        validateId(id);
        return ptsVec[id].end();
    }

    static inline void clear(NodeID id) {
        validateId(id);
        ptsVec[id].clear();
    }

    static inline size_t count(NodeID id) {
        validateId(id);
        return ptsVec[id].count();
    }

    static inline const PtsTy &getPointedBy(NodeID) {
        llvm_unreachable(
            "pointed-by is not supported by ConfigurablePTS; use PointedByPts");
    }

    static inline constexpr bool supportPointedBy() { return false; }

    static inline void selectBackend(bool useBDD) {
        if (backendLocked && useBDD != useBDDBackend)
            llvm::report_fatal_error("Cannot switch points-to backend after initialization");
        useBDDBackend = useBDD;
    }

    static inline bool usingBDD() { return useBDDBackend; }

private:
    static inline void ensureBackendConfigured() {
        if (!backendLocked) {
            useBDDBackend = ConfigUseBDDPts;
            if (useBDDBackend) {
                if (ConfigBDDPtsReorder) {
                    auto methodName = ConfigBDDPtsReorderMethod.getValue();
                    BDDAndersPtsSet::ReorderingMethod method =
                        BDDAndersPtsSet::ReorderingMethod::Sift;
                    if (!BDDAndersPtsSet::parseReorderingMethod(methodName,
                                                                method)) {
                        llvm::report_fatal_error(
                            llvm::Twine("Unknown BDD reordering method: ") +
                            methodName);
                    }
                    BDDAndersPtsSet::configureReordering(true, method);
                } else {
                    BDDAndersPtsSet::configureReordering(false);
                }
            }
            backendLocked = true;
        }
    }

    static inline void validateId(NodeID id) {
        assert(id < ptsVec.size());
    }

    static std::vector<PtsTy> ptsVec;
    static bool useBDDBackend;
    static bool backendLocked;

    friend struct PTSTrait<ConfigurablePTS>;
};

}  // namespace aser

// === Inline implementation ============================================= //

namespace aser {

inline std::unique_ptr<ConfigurablePTS::VariantSet::Concept>
ConfigurablePTS::VariantSet::makeImpl() {
    if (ConfigurablePTS::useBDDBackend)
        return std::make_unique<Model<BDDAndersPtsSet>>();
    return std::make_unique<Model<SparseSet>>();
}

inline ConfigurablePTS::VariantSet::VariantSet() : impl(makeImpl()) {}

inline ConfigurablePTS::VariantSet::VariantSet(const VariantSet &other)
    : impl(other.impl->clone()), cache(other.cache) {}

inline ConfigurablePTS::VariantSet &
ConfigurablePTS::VariantSet::operator=(const VariantSet &other) {
    if (this == &other)
        return *this;
    impl = other.impl->clone();
    cache = other.cache;
    return *this;
}

inline bool ConfigurablePTS::VariantSet::has(TargetID idx) const {
    return impl->has(idx);
}

inline bool ConfigurablePTS::VariantSet::insert(TargetID idx) {
    cache.reset();
    return impl->insert(idx);
}

inline bool ConfigurablePTS::VariantSet::contains(const VariantSet &other) const {
    return impl->contains(*other.impl);
}

inline bool ConfigurablePTS::VariantSet::intersectWith(const VariantSet &other) const {
    return impl->intersectWith(*other.impl);
}

inline bool ConfigurablePTS::VariantSet::unionWith(const VariantSet &other) {
    cache.reset();
    return impl->unionWith(*other.impl);
}

inline void ConfigurablePTS::VariantSet::clear() {
    cache.reset();
    impl->clear();
}

inline size_t ConfigurablePTS::VariantSet::count() const {
    return impl->count();
}

inline bool ConfigurablePTS::VariantSet::isEmpty() const { return impl->isEmpty(); }

inline bool ConfigurablePTS::VariantSet::equals(const VariantSet &other) const {
    return impl->equals(*other.impl);
}

inline void ConfigurablePTS::VariantSet::refreshCache() const {
    if (cache)
        return;
    auto elems = std::make_shared<std::vector<TargetID>>();
    impl->materialize(*elems);
    cache = elems;
}

inline ConfigurablePTS::VariantSet::iterator
ConfigurablePTS::VariantSet::begin() const {
    refreshCache();
    return cache->begin();
}

inline ConfigurablePTS::VariantSet::iterator
ConfigurablePTS::VariantSet::end() const {
    refreshCache();
    return cache->end();
}

inline bool ConfigurablePTS::VariantSet::intersectWithComplement(
    const VariantSet &lhs, const VariantSet &rhs) {
    // Compute: this = lhs \ rhs (elements in lhs but not in rhs)
    cache.reset();
    impl->clear();
    bool changed = false;
    for (auto it = lhs.begin(), ie = lhs.end(); it != ie; ++it) {
        if (!rhs.has(*it)) {
            impl->insert(*it);
            changed = true;
        }
    }
    return changed;
}

inline bool
ConfigurablePTS::VariantSet::operator|=(const VariantSet &other) {
    return unionWith(other);
}

} // namespace aser

DEFINE_PTS_TRAIT(ConfigurablePTS)

#endif
