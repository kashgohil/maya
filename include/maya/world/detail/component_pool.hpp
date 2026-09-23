#pragma once

#include "maya/world/entity.hpp"
#include <algorithm>
#include <concepts>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace maya {
/// Packed storage relocates values. Resource-owning, move-only components are supported.
/// Moves/destructors must not call back into their owning World.
template<class T>
concept Component = std::same_as<T, std::remove_cvref_t<T>> && std::is_object_v<T> &&
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
    std::is_nothrow_destructible_v<T>;

namespace detail {
template<class T>
void reserve_for(std::vector<T>& values, size_t required) {
    if (required <= values.capacity()) return;
    if (required > values.max_size()) throw std::length_error("World storage exhausted");
    const auto spare = values.max_size() - values.capacity();
    const auto growth = std::min(spare, values.capacity() / 2 + 1);
    values.reserve(std::max(required, values.capacity() + growth));
}

class ComponentPoolBase {
public:
    virtual ~ComponentPoolBase() = default;
    virtual void prepare(size_t slot_count, size_t additions) = 0;
    virtual void remove(uint32_t slot) noexcept = 0;
    bool contains(uint32_t slot) const noexcept {
        return slot < m_sparse.size() && m_sparse[slot] != invalid_entity_slot;
    }
    size_t size() const noexcept { return m_slots.size(); }
    std::span<const uint32_t> slots() const noexcept { return m_slots; }
protected:
    std::vector<uint32_t> m_sparse;
    std::vector<uint32_t> m_slots;
};

template<Component T>
class ComponentPool final : public ComponentPoolBase {
public:
    void prepare(size_t slot_count, size_t additions) override {
        // All allocations occur before a transaction publishes any changes.
        m_sparse.resize(slot_count, invalid_entity_slot);
        if (additions > m_values.max_size() - m_values.size() ||
            additions > m_slots.max_size() - m_slots.size())
            throw std::length_error("World component storage exhausted");
        reserve_for(m_slots, m_slots.size() + additions);
        reserve_for(m_values, m_values.size() + additions);
    }
    void add(uint32_t slot, T&& value) noexcept {
        m_sparse[slot] = static_cast<uint32_t>(m_values.size());
        m_slots.push_back(slot);
        m_values.push_back(std::move(value));
    }
    void remove(uint32_t slot) noexcept override {
        if (!contains(slot)) return;
        const auto index = m_sparse[slot];
        m_sparse[slot] = invalid_entity_slot;
        if (index != m_values.size() - 1) {
            m_values[index] = std::move(m_values.back());
            m_slots[index] = m_slots.back();
            m_sparse[m_slots[index]] = index;
        }
        m_values.pop_back();
        m_slots.pop_back();
    }
    T& value(uint32_t slot) noexcept { return m_values[m_sparse[slot]]; }
    const T& value(uint32_t slot) const noexcept { return m_values[m_sparse[slot]]; }
private:
    std::vector<T> m_values;
};
} // namespace detail
} // namespace maya
