/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <condition_variable>
#include <vector>

#include "mongo/db/free_mon/free_mon_protocol_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Message types for free monitoring.
 *
 * Some are generated internally by FreeMonProcessor to handle async HTTP requests.
 */
enum class FreeMonMessageType {
    /**
     * Register server from command-line/config.
     */
    RegisterServer,

    /**
     * Register server from server command.
     */
    RegisterCommand,

    /**
     * Internal: Generated when an async HTTP request completes succesfully.
     */
    AsyncRegisterComplete,

    /**
     * Internal: Generated when an async HTTP request completes with an error.
     */
    AsyncRegisterFail,

    /**
     * Unregister server from server command.
     */
    UnregisterCommand,

    // TODO - add metrics messages
    // MetricsCollect - Cloud wants the "wait" time to calculated when the message processing
    // starts, not ends
    // AsyncMetricsComplete,
    // AsyncMetricsFail,

    // TODO - add replication messages
    // OnPrimary,
    // OpObserver,
};

/**
 * Supported types of registration that occur on server startup.
 */
enum class RegistrationType {
    /**
    * Do not register on start because it was not configured via commandline/config file.
    */
    DoNotRegister,

    /**
    * Register immediately on start since we are a standalone.
    */
    RegisterOnStart,

    /**
    * Register after transition to becoming primary because we are in a replica set.
    */
    RegisterAfterOnTransitionToPrimary,
};

/**
 * Message class that encapsulate a message to the FreeMonMessageProcessor
 *
 * Has a type and a deadline for when to start processing the message.
 */
class FreeMonMessage {
public:
    virtual ~FreeMonMessage();

    /**
     * Create a message that should processed immediately.
     */
    static std::shared_ptr<FreeMonMessage> createNow(FreeMonMessageType type) {
        return std::make_shared<FreeMonMessage>(type, Date_t::min());
    }

    /**
     * Create a message that should processed after the specified deadline.
     */
    static std::shared_ptr<FreeMonMessage> createWithDeadline(FreeMonMessageType type,
                                                              Date_t deadline) {
        return std::make_shared<FreeMonMessage>(type, deadline);
    }

    FreeMonMessage(const FreeMonMessage&) = delete;
    FreeMonMessage(FreeMonMessage&&) = default;

    /**
     * Get the type of message.
     */
    FreeMonMessageType getType() const {
        return _type;
    }

    /**
     * Get the deadline for the message.
     */
    Date_t getDeadline() const {
        return _deadline;
    }

public:
    FreeMonMessage(FreeMonMessageType type, Date_t deadline) : _type(type), _deadline(deadline) {}

private:
    // Type of message
    FreeMonMessageType _type;

    // Deadline for when to process message
    Date_t _deadline;
};


/**
 * Most messages have a simple payload, and this template ensures we create type-safe messages for
 * each message type without copy-pasting repeatedly.
 */
template <FreeMonMessageType typeT>
struct FreeMonPayloadForMessage {
    using payload_type = void;
};

template <>
struct FreeMonPayloadForMessage<FreeMonMessageType::AsyncRegisterComplete> {
    using payload_type = FreeMonRegistrationResponse;
};

template <>
struct FreeMonPayloadForMessage<FreeMonMessageType::RegisterServer> {
    using payload_type = std::pair<RegistrationType, std::vector<std::string>>;
};

template <>
struct FreeMonPayloadForMessage<FreeMonMessageType::AsyncRegisterFail> {
    using payload_type = Status;
};

/**
 * Message with a generic payload based on the type of message.
 */
template <FreeMonMessageType typeT>
class FreeMonMessageWithPayload : public FreeMonMessage {
public:
    using payload_type = typename FreeMonPayloadForMessage<typeT>::payload_type;

    /**
     * Create a message that should processed immediately.
     */
    static std::shared_ptr<FreeMonMessageWithPayload> createNow(payload_type t) {
        return std::make_shared<FreeMonMessageWithPayload>(t, Date_t::min());
    }

    /**
     * Get message payload.
     */
    const payload_type& getPayload() const {
        return _t;
    }

public:
    FreeMonMessageWithPayload(payload_type t, Date_t deadline)
        : FreeMonMessage(typeT, deadline), _t(std::move(t)) {}

private:
    // Message payload
    payload_type _t;
};

/**
 * Single-shot class that encapsulates a Status and allows a caller to wait for a time.
 *
 * Basically, a single producer, single consumer queue with one event.
 */
class WaitableResult {
public:
    WaitableResult() : _status(Status::OK()) {}

    /**
     * Set Status and signal waiter.
     */
    void set(Status status) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        invariant(!_set);
        if (!_set) {
            _set = true;
            _status = std::move(status);
            _condvar.notify_one();
        }
    }

    /**
     * Waits for duration until status has been set.
     *
     * Returns boost::none on timeout.
     */
    boost::optional<Status> wait_for(Milliseconds duration) {
        stdx::unique_lock<stdx::mutex> lock(_mutex);

        if (!_condvar.wait_for(lock, duration.toSystemDuration(), [this]() { return _set; })) {
            return {};
        }

        return _status;
    }

private:
    // Condition variable to signal consumer
    stdx::condition_variable _condvar;

    // Lock for condition variable and to protect state
    stdx::mutex _mutex;

    // Indicates whether _status has been set
    bool _set{false};

    // Provided status
    Status _status;
};

/**
 * Custom waitable message for Register Command message.
 */
class FreeMonRegisterCommandMessage : public FreeMonMessage {
public:
    /**
     * Create a message that should processed immediately.
     */
    static std::shared_ptr<FreeMonRegisterCommandMessage> createNow(
        const std::vector<std::string>& tags) {
        return std::make_shared<FreeMonRegisterCommandMessage>(tags, Date_t::min());
    }

    /**
     * Create a message that should processed after the specified deadline.
     */
    static std::shared_ptr<FreeMonRegisterCommandMessage> createWithDeadline(
        const std::vector<std::string>& tags, Date_t deadline) {
        return std::make_shared<FreeMonRegisterCommandMessage>(tags, deadline);
    }

    /**
     * Get tags.
     */
    const std::vector<std::string>& getTags() const {
        return _tags;
    }

    /**
     * Set Status and signal waiter.
     */
    void setStatus(Status status) {
        _waitable.set(std::move(status));
    }

    /**
     * Waits for duration until status has been set.
     *
     * Returns boost::none on timeout.
     */
    boost::optional<Status> wait_for(Milliseconds duration) {
        return _waitable.wait_for(duration);
    }

public:
    FreeMonRegisterCommandMessage(std::vector<std::string> tags, Date_t deadline)
        : FreeMonMessage(FreeMonMessageType::RegisterCommand, deadline), _tags(std::move(tags)) {}

private:
    // WaitaleResult to notify caller
    WaitableResult _waitable{};

    // Tags
    const std::vector<std::string> _tags;
};

}  // namespace mongo
