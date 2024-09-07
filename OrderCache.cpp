#include "OrderCache.h"
#include <algorithm>
#include <shared_mutex>

// Add an order to the cache
void OrderCache::addOrder(Order order) {
    // Lock only during modifications
    {
        std::unique_lock lockSecId(secIdMutex);  // Lock ordersBySecId for writing
        auto it = ordersBySecId.emplace(order.securityId(), order);
        
        std::unique_lock lockOrderId(orderIdMutex);  // Lock ordersById for writing
        ordersById[order.orderId()] = it;
        
        std::unique_lock lockUser(userMutex);  // Lock ordersByUser for writing
        ordersByUser[order.user()].push_back(it);
    }
}

// Cancel a specific order by its orderId
void OrderCache::cancelOrder(const std::string& orderId) {
    std::unique_lock lockOrderId(orderIdMutex);  // Lock ordersById for writing
    auto it = ordersById.find(orderId);
    if (it != ordersById.end()) {
        // Lock ordersBySecId for removing
        {
            std::unique_lock lockSecId(secIdMutex);
            ordersBySecId.erase(it->second);
        }

        // Lock ordersByUser for removing
        {
            std::unique_lock lockUser(userMutex);
            removeOrderFromUserMap(it->second->second.user(), it->second);
        }
        ordersById.erase(it);
    }
}

// Cancel all orders for a specific user
void OrderCache::cancelOrdersForUser(const std::string& user) {
    std::unique_lock lockUser(userMutex);  // Lock ordersByUser for writing
    auto it = ordersByUser.find(user);
    if (it != ordersByUser.end()) {
        for (auto orderIt : it->second) {
            // Lock ordersBySecId and ordersById individually
            {
                std::unique_lock lockSecId(secIdMutex);
                ordersBySecId.erase(orderIt);
            }
            {
                std::unique_lock lockOrderId(orderIdMutex);
                ordersById.erase(orderIt->second.orderId());
            }
        }
        ordersByUser.erase(it);
    }
}

// Cancel orders for a specific security with a minimum quantity
void OrderCache::cancelOrdersForSecIdWithMinimumQty(const std::string& securityId, unsigned int minQty) {
    std::unique_lock lockSecId(secIdMutex);  // Lock ordersBySecId for writing
    auto range = ordersBySecId.equal_range(securityId);
    for (auto it = range.first; it != range.second; ) {
        if (it->second.qty() >= minQty) {
            {
                std::unique_lock lockUser(userMutex);
                removeOrderFromUserMap(it->second.user(), it);
            }
            {
                std::unique_lock lockOrderId(orderIdMutex);
                ordersById.erase(it->second.orderId());
            }
            it = ordersBySecId.erase(it);  // Move to the next iterator
        } else {
            ++it;
        }
    }
}

// Get the total matching size for a security
unsigned int OrderCache::getMatchingSizeForSecurity(const std::string& securityId) {
    std::shared_lock lockSecId(secIdMutex);  // Lock ordersBySecId for reading
    unsigned int totalMatchingSize = 0;
    auto range = ordersBySecId.equal_range(securityId);

    std::vector<std::multimap<std::string, Order>::iterator> buyOrders;
    std::vector<std::multimap<std::string, Order>::iterator> sellOrders;

    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.side() == "Buy") {
            buyOrders.push_back(it);
        } else if (it->second.side() == "Sell") {
            sellOrders.push_back(it);
        }
    }

    for (auto buyIt = buyOrders.begin(); buyIt != buyOrders.end(); ++buyIt) {
        for (auto sellIt = sellOrders.begin(); sellIt != sellOrders.end(); ) {
            Order& buyOrder = (*buyIt)->second;
            Order& sellOrder = (*sellIt)->second;

            if (buyOrder.company() != sellOrder.company()) {
                unsigned int matchQty = std::min(buyOrder.qty(), sellOrder.qty());
                totalMatchingSize += matchQty;

                buyOrder.reduceQty(matchQty);
                sellOrder.reduceQty(matchQty);

                if (sellOrder.qty() == 0) {
                    sellIt = sellOrders.erase(sellIt);
                } else {
                    ++sellIt;
                }

                if (buyOrder.qty() == 0) {
                    break;
                }
            } else {
                ++sellIt;
            }
        }
    }

    return totalMatchingSize;
}

// Get all orders
std::vector<Order> OrderCache::getAllOrders() const {
    std::shared_lock lockSecId(secIdMutex);  // Lock ordersBySecId for reading
    std::vector<Order> allOrders;
    for (const auto& orderPair : ordersBySecId) {
        allOrders.push_back(orderPair.second);
    }
    return allOrders;
}

// Helper method to remove an order from the user map
void OrderCache::removeOrderFromUserMap(const std::string& user, std::multimap<std::string, Order>::iterator orderIt) {
    auto& userOrders = ordersByUser[user];
    userOrders.erase(std::remove(userOrders.begin(), userOrders.end(), orderIt), userOrders.end());
    if (userOrders.empty()) {
        ordersByUser.erase(user);
    }
}
