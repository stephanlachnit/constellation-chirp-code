#include "Manager.hpp"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <functional>
#include <iterator>
#include <utility>

#include <iostream>

#include "CHIRP/exceptions.hpp"

using namespace cnstln::CHIRP;
using namespace std::literals::chrono_literals;

bool RegisteredService::operator<(const RegisteredService& other) const {
    // Sort first by service id
    auto ord_id = std::to_underlying(identifier) <=> std::to_underlying(other.identifier);
    if (std::is_lt(ord_id)) {
        return true;
    }
    if (std::is_gt(ord_id)) {
        return false;
    }
    // Then by port
    return port < other.port;
}

bool DiscoveredService::operator<(const DiscoveredService& other) const {
    // Ignore IP when sorting, we only care about the host
    auto ord_host_id = host_id <=> other.host_id;
    if (std::is_lt(ord_host_id)) {
        return true;
    }
    if (std::is_gt(ord_host_id)) {
        return false;
    }
    // Same as RegisteredService::operator<
    auto ord_id = std::to_underlying(identifier) <=> std::to_underlying(other.identifier);
    if (std::is_lt(ord_id)) {
        return true;
    }
    if (std::is_gt(ord_id)) {
        return false;
    }
    return port < other.port;
}

bool DiscoverCallbackEntry::operator<(const DiscoverCallbackEntry& other) const {
    // First sort after callback address
    auto ord_callback = reinterpret_cast<std::uintptr_t>(callback) <=> reinterpret_cast<std::uintptr_t>(other.callback);
    if (std::is_lt(ord_callback)) {
        return true;
    }
    if (std::is_gt(ord_callback)) {
        return false;
    }
    // Then after service identifier to listen to
    return std::to_underlying(service_id) < std::to_underlying(other.service_id);
}

Manager::Manager(asio::ip::address brd_address, asio::ip::address any_address, std::string_view group_name, std::string_view host_name)
  : receiver_(any_address), sender_(brd_address), group_id_(MD5Hash(group_name)), host_id_(MD5Hash(host_name)) {}

Manager::Manager(std::string_view brd_ip, std::string_view any_ip, std::string_view group_name, std::string_view host_name)
  : Manager(asio::ip::make_address(brd_ip), asio::ip::make_address(any_ip), group_name, host_name) {}

Manager::~Manager() {
    // First stop Run function
    run_thread_.request_stop();
    if (run_thread_.joinable()) {
        run_thread_.join();
    }
    // Now unregister all services
    UnregisterServices();
}

void Manager::Start() {
    // jthread immediatly starts on construction
    run_thread_ = std::jthread(std::bind_front(&Manager::Run, this));
}

bool Manager::RegisterService(ServiceIdentifier service_id, Port port) {
    RegisteredService service {service_id, port};

    std::unique_lock registered_services_lock {registered_services_mutex_};
    const auto insert_ret = registered_services_.insert(service);
    const bool actually_inserted = insert_ret.second;

    // Lock not needed anymore
    registered_services_lock.unlock();
    if (actually_inserted) {
        SendMessage(OFFER, service);
    }
    return actually_inserted;
}

bool Manager::UnregisterService(ServiceIdentifier service_id, Port port) {
    RegisteredService service {service_id, port};

    std::unique_lock registered_services_lock {registered_services_mutex_};
    const auto erase_ret = registered_services_.erase(service);
    bool actually_erased = erase_ret > 0 ? true : false;

    // Lock not needed anymore
    registered_services_lock.unlock();
    if (actually_erased) {
        SendMessage(DEPART, service);
    }
    return actually_erased;
}

void Manager::UnregisterServices() {
    const std::lock_guard registered_services_lock {registered_services_mutex_};
    for (auto service : registered_services_) {
        SendMessage(DEPART, service);
    }
    registered_services_.clear();
}

std::set<RegisteredService> Manager::GetRegisteredServices() {
    const std::lock_guard registered_services_lock {registered_services_mutex_};
    return registered_services_;
}

bool Manager::RegisterDiscoverCallback(DiscoverCallback* callback, ServiceIdentifier service_id, std::any user_data) {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    const auto insert_ret = discover_callbacks_.emplace(callback, service_id, user_data);

    // Return if actually inserted
    return insert_ret.second;
}

bool Manager::UnregisterDiscoverCallback(DiscoverCallback* callback, ServiceIdentifier service_id) {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    const auto erase_ret = discover_callbacks_.erase({callback, service_id, {}});

    // Return if actually erased
    return erase_ret > 0;
}

void Manager::UnregisterDiscoverCallbacks() {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    discover_callbacks_.clear();
}

void Manager::ForgetDiscoveredServices() {
    const std::lock_guard discovered_services_lock {discovered_services_mutex_};
    discovered_services_.clear();
}

std::vector<DiscoveredService> Manager::GetDiscoveredServices() {
    std::vector<DiscoveredService> ret {};
    const std::lock_guard discovered_services_lock {discovered_services_mutex_};
    std::copy(discovered_services_.begin(), discovered_services_.end(), std::back_inserter(ret));
    return ret;
}

std::vector<DiscoveredService> Manager::GetDiscoveredServices(ServiceIdentifier service_id) {
    std::vector<DiscoveredService> ret {};
    const std::lock_guard discovered_services_lock {discovered_services_mutex_};
    for (const auto& discovered_service : discovered_services_) {
        if (discovered_service.identifier == service_id) {
            ret.push_back(discovered_service);
        }
    }
    return ret;
}

void Manager::SendRequest(ServiceIdentifier service) {
    SendMessage(REQUEST, {service, 0});
}

void Manager::SendMessage(MessageType type, RegisteredService service) {
    const auto asm_msg = Message(type, group_id_, host_id_, service.identifier, service.port).Assemble();
    sender_.SendBroadcast(asm_msg.data(), asm_msg.size());
}

void Manager::Run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        try {
            const auto raw_msg_opt = receiver_.AsyncRecvBroadcast(100ms);

            // Check for timeout
            if (!raw_msg_opt.has_value()) {
                continue;
            }

            const auto raw_msg = raw_msg_opt.value();
            auto chirp_msg = Message(AssembledMessage(raw_msg.content));

            if (chirp_msg.GetGroupID() != group_id_) {
                // Broadcast from different group, ignore
                continue;
            }
            if (chirp_msg.GetHostID() == host_id_) {
                // Broadcast from self, ignore
                continue;
            }

            DiscoveredService discovered_service {raw_msg.address, chirp_msg.GetHostID(), chirp_msg.GetServiceIdentifier(), chirp_msg.GetPort()};

            switch (chirp_msg.GetType()) {
            case REQUEST: {
                auto service_id = discovered_service.identifier;
                const std::lock_guard registered_services_lock {registered_services_mutex_};
                // Replay OFFERs for registered services with same service identifier
                for (const auto& service : registered_services_) {
                    if (service.identifier == service_id) {
                        SendMessage(OFFER, service);
                    }
                }
                break;
            }
            case OFFER: {
                std::unique_lock discovered_services_lock {discovered_services_mutex_};
                if (!discovered_services_.contains(discovered_service)) {
                    discovered_services_.insert(discovered_service);

                    // Unlock discovered_services_lock for user callback
                    discovered_services_lock.unlock();
                    // Acquire lock for discover_callbacks_
                    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
                    // Loop over callback and run as detached threads
                    for (const auto& cb_entry : discover_callbacks_) {
                        if (cb_entry.service_id == discovered_service.identifier) {
                            std::thread(cb_entry.callback, discovered_service, false, cb_entry.user_data).detach();
                        }
                    }
                }
                break;
            }
            case DEPART: {
                std::unique_lock discovered_services_lock {discovered_services_mutex_};
                if (discovered_services_.contains(discovered_service)) {
                    discovered_services_.erase(discovered_service);

                    // Unlock discovered_services_lock for user callback
                    discovered_services_lock.unlock();
                    // Acquire lock for discover_callbacks_
                    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
                    // Loop over callback and run as detached threads
                    for (const auto& cb_entry : discover_callbacks_) {
                        if (cb_entry.service_id == discovered_service.identifier) {
                            std::thread(cb_entry.callback, discovered_service, true, cb_entry.user_data).detach();
                        }
                    }
                }
                break;
            }
            default: std::unreachable();
            }
        }
        catch (const DecodeError& error) {
            continue;
        }
    }
}
