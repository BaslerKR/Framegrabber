#pragma once

#include "FramegrabberSourceController.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Synthetic Framegrabber module surface for adapter tests.
 *
 * No SDK, board, camera or applet is touched. The state copies the shapes the real module
 * returns, so the controller sees the same tree, access modes and readback rules.
 */
struct FramegrabberTestState
{
    std::mutex mutex;
    std::vector<FramegrabberSystem::BoardInfo> boards;
    std::vector<Framegrabber::AppletInfo> catalog;
    std::vector<Framegrabber::AppletFeatureNode> appletModel;
    std::map<std::int64_t, std::int64_t> integers;
    std::map<std::int64_t, double> floats;
    std::map<std::int64_t, std::string> strings;
    std::map<std::string, std::string> cameraValues;
    bool opened = false;
    bool grabbing = false;
    bool control = false;
    std::uint64_t generation = 0;
    int dmaCount = 2;
    int appletCommands = 0;
    int cameraCommands = 0;
    std::string appletPath;
    std::string appletName;
    std::string configuration;
    std::string savedConfiguration;
    std::string outsideMcfApplet = "C:/outside/legacy.hap";
};

/** @brief Builds one ready-to-use synthetic backend and its owned state. */
struct FramegrabberTestBackend
{
    FramegrabberTestState state;

    FramegrabberTestBackend()
    {
        FramegrabberSystem::BoardInfo board;
        board.index = 0;
        board.name = "Test Board";
        board.serial = "SN1";
        board.firmware = "1.0";
        board.driver = "5.11";
        board.hardware = "imaFlex CXP-12 Penta";
        state.boards.push_back(board);

        Framegrabber::AppletInfo first;
        first.name = "Acquisition";
        first.file = "Acquisition.hap";
        first.path = "C:/applets/Acquisition.hap";
        first.version = "1.2";
        first.description = "Synthetic acquisition applet";
        first.dmaCount = 2;
        first.cameraCount = 1;
        first.active = true;
        first.supportsCameraControl = true;
        Framegrabber::AppletInfo second;
        second.name = "Second";
        second.file = "Second.hap";
        second.path = "C:/applets/Second.hap";
        second.version = "1.0";
        second.dmaCount = 1;
        state.catalog = {first, second};

        auto integer = [](const char* name, const std::int64_t id, const bool writable) {
            Framegrabber::AppletFeatureNode node;
            node.name = name;
            node.accessName = name;
            node.displayName = name;
            node.parameterId = id;
            node.kind = Framegrabber::AppletFeatureKind::Integer;
            node.readable = true;
            node.writable = writable;
            return node;
        };
        Framegrabber::AppletFeatureNode acquisition;
        acquisition.name = "Acquisition";
        acquisition.displayName = "Acquisition";
        acquisition.kind = Framegrabber::AppletFeatureKind::Category;
        acquisition.children.push_back(integer("Width", 1, true));
        Framegrabber::AppletFeatureNode mode;
        mode.name = "Mode";
        mode.accessName = "Mode";
        mode.displayName = "Mode";
        mode.parameterId = 2;
        mode.kind = Framegrabber::AppletFeatureKind::Enumeration;
        mode.readable = true;
        mode.writable = true;
        mode.enumEntries = {{"Off", "Off", 0}, {"On", "On", 1}};
        acquisition.children.push_back(mode);
        Framegrabber::AppletFeatureNode acquire;
        acquire.name = "Acquire";
        acquire.accessName = "Acquire";
        acquire.displayName = "Acquire";
        acquire.parameterId = 3;
        acquire.kind = Framegrabber::AppletFeatureKind::Command;
        acquire.readable = false;
        acquire.writable = true;
        acquisition.children.push_back(acquire);
        Framegrabber::AppletFeatureNode device;
        device.name = "Device";
        device.displayName = "Device";
        device.kind = Framegrabber::AppletFeatureKind::Category;
        Framegrabber::AppletFeatureNode model;
        model.name = "Model";
        model.accessName = "Model";
        model.displayName = "Model";
        model.parameterId = 4;
        model.kind = Framegrabber::AppletFeatureKind::String;
        model.readable = true;
        model.writable = false;
        device.children.push_back(model);
        Framegrabber::AppletFeatureNode gain;
        gain.name = "Gain";
        gain.accessName = "Gain";
        gain.displayName = "Gain";
        gain.parameterId = 5;
        gain.kind = Framegrabber::AppletFeatureKind::Float;
        gain.readable = true;
        gain.writable = true;
        device.children.push_back(gain);
        state.appletModel = {acquisition, device};

        state.integers[1] = 640;
        state.integers[2] = 0;
        state.integers[3] = 0;
        state.strings[4] = "Synthetic";
        state.floats[5] = 1.5;
        state.cameraValues["Width"] = "1280";
        state.cameraValues["PixelFormat"] = "Mono8";
    }

    [[nodiscard]] std::string cameraXml() const
    {
        return R"(<?xml version="1.0"?>
<RegisterDescription>
  <RegisterBlock>
    <Category Name="Root">
      <pFeature>Image</pFeature>
    </Category>
    <Category Name="Image">
      <DisplayName>Image</DisplayName>
      <pFeature>Width</pFeature>
      <pFeature>PixelFormat</pFeature>
      <pFeature>Test</pFeature>
    </Category>
    <Integer Name="Width">
      <DisplayName>Width</DisplayName>
      <Description>Sensor width</Description>
      <Unit>px</Unit>
      <Visibility>Beginner</Visibility>
      <AccessMode>RW</AccessMode>
      <pValue>WidthReg</pValue>
    </Integer>
    <Integer Name="WidthReg">
      <AccessMode>RW</AccessMode>
    </Integer>
    <Enumeration Name="PixelFormat">
      <DisplayName>Pixel format</DisplayName>
      <Visibility>Expert</Visibility>
      <AccessMode>RW</AccessMode>
      <EnumEntry Name="Mono8"><Value>0</Value></EnumEntry>
      <EnumEntry Name="RGB8"><Value>1</Value></EnumEntry>
      <pValue>PixelFormatReg</pValue>
    </Enumeration>
    <Enumeration Name="PixelFormatReg">
      <AccessMode>RW</AccessMode>
    </Enumeration>
    <Command Name="Test">
      <DisplayName>Test</DisplayName>
      <Visibility>Guru</Visibility>
      <AccessMode>WO</AccessMode>
      <pCommand>TestReg</pCommand>
    </Command>
    <Command Name="TestReg">
      <AccessMode>WO</AccessMode>
    </Command>
  </RegisterBlock>
</RegisterDescription>)";
    }

    [[nodiscard]] FramegrabberSourceController::Backend backend()
    {
        FramegrabberSourceController::Backend result;
        auto* shared = &state;
        result.boards = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->boards;
        };
        result.isOpened = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->opened;
        };
        result.connectedBoard = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->opened ? shared->boards.front().displayName() : std::string{};
        };
        result.loadedAppletName = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->opened ? shared->appletName : std::string{};
        };
        result.loadedAppletVersion = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->opened ? std::string("1.2") : std::string{};
        };
        result.appletPath = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->opened ? shared->appletPath : std::string{};
        };
        result.configurationPath = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->opened ? shared->configuration : std::string{};
        };
        result.connectionGeneration = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->generation;
        };
        result.refreshAppletCatalog = [shared](const std::string&) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return !shared->opened;
        };
        result.appletCatalog = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->catalog;
        };
        result.resolveApplet = [shared](const std::string& reference) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            for (const auto& applet : shared->catalog)
                if (applet.name == reference || applet.file == reference || applet.path == reference)
                    return applet.path;
            return std::string{};
        };
        result.loadApplet = [shared](const std::string& reference, const std::string&) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            for (const auto& applet : shared->catalog)
            {
                if (applet.name != reference && applet.file != reference && applet.path != reference)
                    continue;
                shared->opened = true;
                shared->grabbing = false;
                ++shared->generation;
                shared->appletName = applet.name;
                shared->appletPath = applet.path;
                shared->configuration.clear();
                return true;
            }
            return false;
        };
        result.appletPathFromConfiguration = [shared](const std::string& configuration, const std::string&) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (configuration == "outside.mcf") return shared->outsideMcfApplet;
            if (configuration == "installed.mcf" && !shared->catalog.empty())
                return shared->catalog.front().path;
            return std::string{};
        };
        result.loadAppletConfiguration =
            [shared](const std::string& applet, const std::string& configuration, const std::string&) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                std::string path = applet;
                if (path.empty())
                    path = configuration == "installed.mcf" && !shared->catalog.empty()
                        ? shared->catalog.front().path
                        : shared->outsideMcfApplet;
                shared->opened = true;
                shared->grabbing = false;
                ++shared->generation;
                shared->appletName = "Configured";
                shared->appletPath = path;
                shared->configuration = configuration;
                return true;
            };
        result.saveConfiguration = [shared](const std::string& configuration) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->savedConfiguration = configuration;
            return true;
        };
        result.close = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->opened = false;
            shared->grabbing = false;
        };
        result.isGrabbing = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->grabbing;
        };
        result.dmaCount = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->dmaCount;
        };
        result.tryBeginControl = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->control) return false;
            shared->control = true;
            return true;
        };
        result.endControl = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->control = false;
        };
        result.controlBusy = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->control;
        };
        result.lastError = [] { return Framegrabber::OperationError{0, {}}; };
        result.acquisitionStatistics = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            std::vector<Framegrabber::ChannelStatistics> channels;
            for (int dma = 0; dma < shared->dmaCount; ++dma)
            {
                Framegrabber::ChannelStatistics channel;
                channel.dmaIndex = static_cast<unsigned int>(dma);
                channels.push_back(channel);
            }
            return channels;
        };
        result.cameraDiscovery = [] { return Framegrabber::CameraDiscovery{0, {}, false}; };
        result.refreshCameras = [](Framegrabber::CameraTransport) { return true; };
        result.cachedCameras = [shared](Framegrabber::CameraTransport transport) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (transport != Framegrabber::CameraTransport::CoaXPress) return std::vector<Framegrabber::CameraInfo>{};
            return std::vector<Framegrabber::CameraInfo>{
                Framegrabber::CameraInfo{transport, 0, "Basler", "acA1300", "12345", true}};
        };
        result.runtimeDiagnostics = [shared](std::optional<unsigned>) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return Framegrabber::RuntimeDiagnostics{shared->grabbing, 0, 0};
        };
        result.grab = [shared](std::size_t) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->grabbing = true;
        };
        result.stop = [shared] {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->grabbing = false;
        };
        result.appletFeatureModel = [shared](unsigned) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            return shared->appletModel;
        };
        result.appletParameterLimits = [](std::int64_t id, unsigned) {
            Framegrabber::AppletParameterLimits limits;
            if (id == 1)
            {
                limits.minimum = std::int64_t{1};
                limits.maximum = std::int64_t{65535};
                limits.increment = std::int64_t{1};
            }
            else if (id == 5)
            {
                limits.minimum = 0.0;
                limits.maximum = 10.0;
                limits.increment = 0.5;
            }
            return limits;
        };
        result.getAppletParameter = [shared](std::int64_t id, unsigned,
                                             Framegrabber::ParameterValue& value, bool) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            switch (id)
            {
            case 1: value = shared->integers.at(1); return true;
            case 2: value = shared->integers.at(2); return true;
            case 3: value = std::int64_t{0}; return true;
            case 4: value = shared->strings.at(4); return true;
            case 5: value = shared->floats.at(5); return true;
            default: return false;
            }
        };
        result.appletParameterPrototype = [](std::int64_t id, unsigned) -> std::optional<Framegrabber::ParameterValue> {
            switch (id)
            {
            case 1:
            case 2:
            case 3: return std::int64_t{0};
            case 4: return std::string{};
            case 5: return 0.0;
            default: return std::nullopt;
            }
        };
        result.setAppletParameter = [shared](std::int64_t id, unsigned,
                                             const Framegrabber::ParameterValue& value, bool, bool,
                                             int* sdkResult) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (const auto* number = std::get_if<std::int64_t>(&value)) shared->integers[id] = *number;
            else if (const auto* number = std::get_if<double>(&value)) shared->floats[id] = *number;
            else if (const auto* text = std::get_if<std::string>(&value)) shared->strings[id] = *text;
            if (sdkResult) *sdkResult = 0;
            return true;
        };
        result.executeAppletCommand = [shared](unsigned, std::int64_t, int* sdkResult) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            ++shared->appletCommands;
            if (sdkResult) *sdkResult = 0;
            return true;
        };
        result.cameraCapabilities = [] {
            return std::vector<Framegrabber::CameraControlCapability>{
                Framegrabber::CameraControlCapability{
                    Framegrabber::CameraTransport::CoaXPress, true, true, true}};
        };
        const std::string xml = cameraXml();
        result.cameraFeatureXml = [xml](Framegrabber::CameraTransport transport, unsigned dma) {
            if (transport != Framegrabber::CameraTransport::CoaXPress || dma != 0) return std::string{};
            return xml;
        };
        result.getCameraFeature = [shared](Framegrabber::CameraTransport, unsigned,
                                           const std::string& name, Framegrabber::ParameterValue& value) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (name == "Width")
            {
                const auto stored = shared->cameraValues.find(name);
                value = std::int64_t{stored == shared->cameraValues.end()
                    ? 1280 : std::stoll(stored->second)};
                return true;
            }
            if (name == "PixelFormat")
            {
                value = shared->cameraValues.at(name);
                return true;
            }
            if (name == "Test")
            {
                value = std::int64_t{0};
                return true;
            }
            return false;
        };
        result.cameraFeatureLimits = [](Framegrabber::CameraTransport, unsigned,
                                        const std::string& name, bool floating) {
            Framegrabber::AppletParameterLimits limits;
            if (name == "Width" && !floating)
            {
                limits.minimum = std::int64_t{1};
                limits.maximum = std::int64_t{8192};
                limits.increment = std::int64_t{1};
            }
            return limits;
        };
        result.setCameraFeature = [shared](Framegrabber::CameraTransport, unsigned,
                                           const std::string& name,
                                           const Framegrabber::ParameterValue& value, bool) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (const auto* number = std::get_if<std::int64_t>(&value))
                shared->cameraValues[name] = std::to_string(*number);
            else if (const auto* text = std::get_if<std::string>(&value))
                shared->cameraValues[name] = *text;
            return true;
        };
        result.executeCameraCommand = [shared](Framegrabber::CameraTransport, unsigned,
                                               const std::string&) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            ++shared->cameraCommands;
            return true;
        };
        return result;
    }
};

/** @brief Installs a synthetic backend into a controller created without a Framegrabber. */
struct FramegrabberControllerTestAccess
{
    static void install(FramegrabberSourceController& controller, FramegrabberTestBackend& backend)
    {
        controller._backend = backend.backend();
        controller._sessionAvailable.store(true, std::memory_order_release);
    }

    /** @brief Publishes one delta through the module-owned bounded queue. */
    static void publishDelta(FramegrabberSourceController& controller, const std::string& connection,
                             std::vector<DeviceParameterDescriptor> descriptors)
    {
        controller._notifications.publishDelta(connection, std::move(descriptors), {});
    }

    static void drain(FramegrabberSourceController& controller)
    {
        controller._notifications.drain();
    }
};
