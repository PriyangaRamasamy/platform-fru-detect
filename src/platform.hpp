/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "notify.hpp"

#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>

extern "C"
{
#include <sys/timerfd.h>
}

class Inventory;

template <class T>
class Connector
{
  public:
    template <typename... Args>
    Connector(Args&&... args) :
        device(), ctor([this, args...]() mutable {
            device.emplace(std::forward<Args>(args)...);
        })
    {}
    ~Connector() = default;

    void populate()
    {
        ctor();
    }

    void depopulate()
    {
        device.reset();
    }

    bool isPopulated()
    {
        return device.has_value();
    }

    T& getDevice()
    {
        return device.value();
    }

  private:
    std::optional<T> device;
    std::function<void(void)> ctor;
};

class FRU
{
  public:
    FRU() = default;
    virtual ~FRU() = default;

    virtual std::string getInventoryPath() const = 0;
    virtual void addToInventory(Inventory* inventory) = 0;
    virtual void removeFromInventory(Inventory* inventory) = 0;
};

class Device
{
  public:
    enum
    {
        UNPLUG_RETAINS_INVENTORY = 1,
        UNPLUG_REMOVES_INVENTORY,
    };

    virtual ~Device() = default;

    virtual void plug(Notifier& notifier) = 0;
    virtual void unplug(Notifier& notifier,
                        int mode = UNPLUG_REMOVES_INVENTORY) = 0;
};

template <typename T>
concept DerivesDevice = std::is_base_of<Device, T>::value;

/*
 * TODO: Add multiple instances to a single timerfd to make it more efficient
 *
 * Haven't done this yet as I already have enough to think about to get it all
 * to work.
 */
template <DerivesDevice T>
class PolledGPIODevicePresence : public NotifySink
{
  public:
    PolledGPIODevicePresence() = default;
    PolledGPIODevicePresence(gpiod::line* line, Connector<T>* connector) :
        line(line), connector(connector), timerfd(-1)
    {}
    ~PolledGPIODevicePresence() = default;

    PolledGPIODevicePresence<T>&
        operator=(const PolledGPIODevicePresence<T>& other) = default;

    /* NotifySink */
    virtual void arm() override
    {
        assert(timerfd == -1 && "Bad state: timer already armed");

        timerfd = ::timerfd_create(CLOCK_MONOTONIC, 0);
        if (timerfd == -1)
        {
            lg2::error("Failed to create timerfd: {ERRNO_DESCRIPTION}",
                       "ERRNO_DESCRIPTION", ::strerror(errno), "ERRNO", errno);
            std::system_category().default_error_condition(errno);
        }

        struct itimerspec interval
        {
            {1, 0},
            {
                1, 0
            }
        };

        int rc = ::timerfd_settime(timerfd, 0, &interval, nullptr);
        if (rc == -1)
        {
            close(timerfd);

            lg2::error(
                "Failed to set interval for timerfd: {ERRNO_DESCRIPTION}",
                "ERRNO_DESCRIPTION", ::strerror(errno), "ERRNO", errno);
            std::system_category().default_error_condition(errno);
        }
    }

    virtual int getFD() override
    {
        return timerfd;
    }

    virtual void notify(Notifier& notifier) override
    {
        uint64_t res;
        ssize_t rc;

        rc = ::read(timerfd, &res, sizeof(res));
        if (rc != sizeof(res))
        {
            if (rc == -1)
            {
                lg2::error(
                    "Failed to read from timerfd {TIMER_FD}: {ERRNO_DESCRIPTION}",
                    "TIMER_FD", timerfd, "ERRNO_DESCRIPTION", ::strerror(errno),
                    "ERRNO", errno);
                std::system_category().default_error_condition(errno);
            }
            else
            {
                lg2::warning(
                    "Short read from timerfd {TIMER_FD}: {READ_LENGTH}",
                    "TIMER_FD", timerfd, "READ_LENGTH", rc);
            }
        }

        if (line->get_value())
        {
            if (!connector->isPopulated())
            {
                lg2::debug("Presence GPIO asserted with unpopulated connector");
                connector->populate();
                connector->getDevice().plug(notifier);
            }
        }
        else
        {
            if (connector->isPopulated())
            {
                lg2::debug("Presence GPIO deasserted with populated connector");
                connector->getDevice().unplug(notifier);
                connector->depopulate();
            }
        }
    }

    virtual void disarm() override
    {
        assert(timer > -1 && "Bad state: Timer already disarmed");
        ::close(timerfd);
    }

  private:
    gpiod::line* line;
    Connector<T>* connector;
    int timerfd;
};

class Platform
{
  public:
    static std::string getModel();
    static bool isSupported();
};
