/* SPDX-License-Identifier: Apache-2.0 */
#include "devices/nvme.hpp"
#include "inventory.hpp"
#include "platforms/rainier.hpp"
#include "sysfs/gpio.hpp"
#include "sysfs/i2c.hpp"

#include <gpiod.hpp>
#include <phosphor-logging/lg2.hpp>

#include <cassert>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string>

PHOSPHOR_LOG2_USING;

static constexpr const char* app_name = "NVMe Drive Management";

static const std::map<int, int> flett_mux_channel_map = {
    {8, 3},
    {9, 2},
    {10, 0},
    {11, 1},
};

static const std::map<int, int> flett_slot_presence_map = {
    {8, 8},
    {9, 9},
    {10, 10},
    {11, 11},
};

static const std::map<int, int> flett_index_map = {
    {8, 0},
    {9, 1},
    {10, 0},
    {11, 2},
};

static const std::map<int, int> flett_connector_slot_map = {
    {0, 8},
    {1, 9},
    {2, 10},
    {3, 11},
};

int Nisqually::getFlettIndex(int slot)
{
    return flett_index_map.at(slot);
}

SysfsI2CBus Nisqually::getFlettSlotI2CBus(int slot)
{
    SysfsI2CBus rootBus = Ingraham::getPCIeSlotI2CBus(slot);
    SysfsI2CMux mux(rootBus, Nisqually::slotMuxAddress);

    debug("Looking up mux channel for Flett in slot {PCIE_SLOT}", "PCIE_SLOT",
          slot);

    int channel = flett_mux_channel_map.at(slot);

    return SysfsI2CBus(mux, channel);
}

Nisqually::Nisqually(Inventory* inventory) :
    inventory(inventory),
    flettPresenceChip(SysfsGPIOChip(std::filesystem::path(
                                        Nisqually::flett_presence_device_path))
                          .getName()
                          .string(),
                      gpiod::chip::OPEN_BY_NAME),
    flettConnectors{{
        Connector<Flett>(this->inventory, this, 8),
        Connector<Flett>(this->inventory, this, 9),
        Connector<Flett>(this->inventory, this, 10),
        Connector<Flett>(this->inventory, this, 11),
    }},
    williwakasPresenceChip(
        SysfsGPIOChip(
            std::filesystem::path(Nisqually::williwakas_presence_device_path))
            .getName()
            .string(),
        gpiod::chip::OPEN_BY_NAME),
    williwakasConnectors{{
        Connector<Williwakas>(this->inventory, this, 0),
        Connector<Williwakas>(this->inventory, this, 1),
        Connector<Williwakas>(this->inventory, this, 2),
    }}
{
    /* Slot 9 is on the same mux as slot 8 */
    Ingraham::getPCIeSlotI2CBus(8).probeDevice("pca9546",
                                               Nisqually::slotMuxAddress);
    /* Slot 11 is on the same mux as slot 10 */
    Ingraham::getPCIeSlotI2CBus(10).probeDevice("pca9546",
                                                Nisqually::slotMuxAddress);

    /* TODO: Need a PolledGPIODevicePresence! */

    /* FIXME: Consolidate this with the presence map, but it's the identity map
     */
    std::vector<unsigned int> flettPresentOffsets({8, 9, 10, 11});
    flettPresenceLines = flettPresenceChip.get_lines(flettPresentOffsets);
    flettPresenceLines.request({app_name, gpiod::line_request::DIRECTION_INPUT,
                                gpiod::line_request::FLAG_ACTIVE_LOW});

    std::vector<unsigned int> williwakasPresenceOffsets(
        williwakas_presence_map.begin(), williwakas_presence_map.end());
    williwakasPresenceLines =
        williwakasPresenceChip.get_lines(williwakasPresenceOffsets);
    williwakasPresenceLines.request({app_name,
                                     gpiod::line_request::DIRECTION_INPUT,
                                     gpiod::line_request::FLAG_ACTIVE_LOW});
}

void Nisqually::plug(Notifier& notifier)
{
    detectFlettCards();
    for (auto& connector : flettConnectors)
    {
        if (connector.isPopulated())
        {
            connector.getDevice().plug(notifier);
        }
    }

    detectWilliwakasCards();
    for (auto& connector : williwakasConnectors)
    {
        if (connector.isPopulated())
        {
            connector.getDevice().plug(notifier);
        }
    }
}

void Nisqually::unplug(Notifier& notifier, int mode)
{
    for (auto& connector : williwakasConnectors)
    {
        if (connector.isPopulated())
        {
            connector.getDevice().unplug(notifier, mode);
            connector.depopulate();
        }
    }

    for (auto& connector : flettConnectors)
    {
        if (connector.isPopulated())
        {
            connector.getDevice().unplug(notifier, mode);
            connector.depopulate();
        }
    }
}

void Nisqually::detectWilliwakasCards(void)
{
    debug("Locating Williwakas cards");

    for (std::size_t index = 0; index < williwakasConnectors.size(); index++)
    {
        debug("Testing for Williwakas presence at index {WILLIWAKAS_ID}",
              "WILLIWAKAS_ID", index);

        if (!isWilliwakasPresent(index))
        {
            info("Williwakas {WILLIWAKAS_ID} is not present", "WILLIWAKAS_ID",
                 index);
            continue;
        }

        try
        {
            williwakasConnectors[index].populate();
            debug("Initialised Williwakas {WILLIWAKAS_ID}", "WILLIWAKAS_ID",
                  index);
        }
        catch (const SysfsI2CDeviceDriverBindException& ex)
        {
            std::string what(ex.what());
            error(
                "Required drivers failed to bind for devices on Williwakas {WILLIWAKAS_ID}: {EXCEPTION_DESCRIPTION}",
                "WILLIWAKAS_ID", index, "EXCEPTION_DESCRIPTION", what);
            warning("Skipping FRU detection on Williwakas {WILLIWAKAS_ID}",
                    "WILLIWAKAS_ID", index);
            continue;
        }
    }
}

std::string Nisqually::getInventoryPath() const
{
    return "/system/chassis/motherboard";
}

void Nisqually::addToInventory([[maybe_unused]] Inventory* inventory)
{
    std::logic_error("Unimplemented");
}

void Nisqually::removeFromInventory([[maybe_unused]] Inventory* inventory)
{
    std::logic_error("Unimplemented");
}

/*
 * Note that Bear River and Bear Lake cards also assert the presence GPIO.
 * However, if they are present in slots that can also house Flett cards there
 * will be no associated Williwakas card and as such there will be no drives
 * detected.
 */
bool Nisqually::isFlettPresentAt(int slot)
{
    bool present = flettPresenceLines.get(slot - 8).get_value();

    debug("Flett {FLETT_ID} presence for slot {PCIE_SLOT}: {FLETT_PRESENT}",
          "FLETT_ID", getFlettIndex(slot), "PCIE_SLOT", slot, "FLETT_PRESENT",
          present);

    return present;
}

bool Nisqually::isWilliwakasPresent(int index)
{
    bool present = williwakasPresenceLines.get(index).get_value();

    debug("Williwakas presence at index {WILLIWAKAS_ID}: {WILLIWAKAS_PRESENT}",
          "WILLIWAKAS_ID", index, "WILLIWAKAS_PRESENT", present);

    return present;
}

void Nisqually::detectFlettCards()
{
    debug("Locating Flett cards");

    /* FIXME: do something more ergonomic */
    for (auto& [connector, slot] : flett_connector_slot_map)
    {
        if (!isFlettPresentAt(slot))
        {
            debug("No Flett in slot {PCIE_SLOT}", "PCIE_SLOT", slot);
            continue;
        }

        try
        {
            flettConnectors[connector].populate();
            debug("Initialised Flett {FLETT_ID} in slot {PCIE_SLOT}",
                  "FLETT_ID", getFlettIndex(slot), "PCIE_SLOT", slot);
        }
        catch (const SysfsI2CDeviceDriverBindException& ex)
        {
            debug(
                "Required drivers failed to bind for devices on Flett {FLETT_ID} in slot {PCIE_SLOT}: {EXCEPTION}",
                "FLETT_ID", getFlettIndex(slot), "PCIE_SLOT", slot, "EXCEPTION",
                ex);
            warning("Failed to detect Flett in slot {PCIE_SLOT}", "PCIE_SLOT",
                    slot);
            continue;
        }
    }
}
