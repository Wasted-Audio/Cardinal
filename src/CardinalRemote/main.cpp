/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2022 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the LICENSE file.
 */

#include "Application.hpp"
#include "Window.hpp"
#include "RemoteUI.hpp"

#include <asset.hpp>
#include <patch.hpp>
#include <random.hpp>
#include <settings.hpp>
#include <system.hpp>

#include <app/Browser.hpp>
#include <app/Scene.hpp>
#include <engine/Engine.hpp>
#include <ui/common.hpp>


#include "PluginContext.hpp"

namespace rack {
namespace plugin {
    void initStaticPlugins();
    void destroyStaticPlugins();
}
}

START_NAMESPACE_DISTRHO

bool isUsingNativeAudio() noexcept { return false; }
bool supportsAudioInput() { return false; }
bool supportsBufferSizeChanges() { return false; }
bool supportsMIDI() { return false; }
bool isAudioInputEnabled() { return false; }
bool isMIDIEnabled() { return false; }
uint getBufferSize() { return 0; }
bool requestAudioInput() { return false; }
bool requestBufferSizeChange(uint) { return false; }
bool requestMIDI() { return false; }
const char* getPluginFormatName() noexcept { return "Remote"; }

uint32_t Plugin::getBufferSize() const noexcept { return 128; }
double Plugin::getSampleRate() const noexcept { return 48000; }
bool Plugin::writeMidiEvent(const MidiEvent&) noexcept { return false; }

END_NAMESPACE_DISTRHO

int main(const int argc, const char* argv[])
{
    using namespace rack;

    settings::allowCursorLock = false;
    settings::autoCheckUpdates = false;
    settings::autosaveInterval = 0;
    settings::devMode = true;
    settings::isPlugin = true;
    settings::skipLoadOnLaunch = true;
    settings::showTipsOnLaunch = false;
    settings::windowPos = math::Vec(0, 0);

    // copied from https://community.vcvrack.com/t/16-colour-cable-palette/15951
    settings::cableColors = {
        color::fromHexString("#ff5252"),
        color::fromHexString("#ff9352"),
        color::fromHexString("#ffd452"),
        color::fromHexString("#e8ff52"),
        color::fromHexString("#a8ff52"),
        color::fromHexString("#67ff52"),
        color::fromHexString("#52ff7d"),
        color::fromHexString("#52ffbe"),
        color::fromHexString("#52ffff"),
        color::fromHexString("#52beff"),
        color::fromHexString("#527dff"),
        color::fromHexString("#6752ff"),
        color::fromHexString("#a852ff"),
        color::fromHexString("#e952ff"),
        color::fromHexString("#ff52d4"),
        color::fromHexString("#ff5293"),
    };

    system::init();
    logger::init();
    random::init();
    ui::init();

    std::string templatePath;
   #ifdef CARDINAL_PLUGIN_SOURCE_DIR
    // Make system dir point to source code location as fallback
    asset::systemDir = CARDINAL_PLUGIN_SOURCE_DIR DISTRHO_OS_SEP_STR "Rack";

    if (system::exists(system::join(asset::systemDir, "res")))
    {
        templatePath = CARDINAL_PLUGIN_SOURCE_DIR DISTRHO_OS_SEP_STR "template.vcv";
    }
    // If source code dir does not exist use install target prefix as system dir
    else
   #endif
    {
       #if defined(ARCH_MAC)
        asset::systemDir = "/Library/Application Support/Cardinal";
       #elif defined(ARCH_WIN)
        const std::string commonprogfiles = getSpecialPath(kSpecialPathCommonProgramFiles);
        if (! commonprogfiles.empty())
            asset::systemDir = system::join(commonprogfiles, "Cardinal");
       #else
        asset::systemDir = CARDINAL_PLUGIN_PREFIX "/share/cardinal";
       #endif

        if (! asset::systemDir.empty())
        {
            asset::bundlePath = system::join(asset::systemDir, "PluginManifests");
            templatePath = system::join(asset::systemDir, "template.vcv");
        }
    }
    
    asset::userDir = asset::systemDir;

    // Log environment
    INFO("%s %s version %s", APP_NAME.c_str(), APP_EDITION.c_str(), APP_VERSION.c_str());
    INFO("%s", system::getOperatingSystemInfo().c_str());
//     INFO("Binary filename: %s", getBinaryFilename());
    INFO("System directory: %s", asset::systemDir.c_str());
    INFO("User directory: %s", asset::userDir.c_str());
    INFO("Template patch: %s", templatePath.c_str());

    // Report to user if something is wrong with the installation
    if (asset::systemDir.empty())
    {
        d_stderr2("Failed to locate Cardinal plugin bundle.\n"
                    "Install Cardinal with its bundle folder intact and try again.");
    }
    else if (! system::exists(asset::systemDir))
    {
        d_stderr2("System directory \"%s\" does not exist.\n"
                    "Make sure Cardinal was downloaded and installed correctly.", asset::systemDir.c_str());
    }

    INFO("Initializing plugins");
    plugin::initStaticPlugins();

    INFO("Initializing plugin browser DB");
    app::browserInit();

    // create unique temporary path for this instance
    std::string autosavePath;

    try {
        char uidBuf[24];
        const std::string tmp = rack::system::getTempDirectory();

        for (int i=1;; ++i)
        {
            std::snprintf(uidBuf, sizeof(uidBuf), "CardinalRemote.%04d", i);
            const std::string trypath = rack::system::join(tmp, uidBuf);

            if (! rack::system::exists(trypath))
            {
                if (rack::system::createDirectories(trypath))
                    autosavePath = trypath;
                break;
            }
        }
    } DISTRHO_SAFE_EXCEPTION("create unique temporary path");

    CardinalPluginContext context(nullptr);
    rack::contextSet(&context);

    context.bufferSize = 512;
    rack::settings::sampleRate = context.sampleRate = 48000;

    context.engine = new rack::engine::Engine;
    context.engine->setSampleRate(context.sampleRate);

    context.history = new rack::history::State;
    context.patch = new rack::patch::Manager;
    context.patch->autosavePath = autosavePath;
    context.patch->templatePath = templatePath;

    context.event = new rack::widget::EventState;
    context.scene = new rack::app::Scene;
    context.event->rootWidget = context.scene;
    context.window = new rack::window::Window;

    context.patch->loadTemplate();
    context.scene->rackScroll->reset();

    Application app;
    Window win(app);
    win.setResizable(true);
    win.setTitle("CardinalRemote");
    ScopedPointer<CardinalRemoteUI> remoteUI;

    {
        const Window::ScopedGraphicsContext sgc(win);
        remoteUI = new CardinalRemoteUI(win, templatePath);
    }

    win.show();
    app.exec();

    context.patch->clear();

    if (! autosavePath.empty())
        rack::system::removeRecursively(autosavePath);

    INFO("Clearing asset paths");
    asset::bundlePath.clear();
    asset::systemDir.clear();
    asset::userDir.clear();

    INFO("Destroying plugins");
    plugin::destroyStaticPlugins();

    INFO("Destroying settings");
    settings::destroy();

    INFO("Destroying logger");
    logger::destroy();

    return 0;
}
